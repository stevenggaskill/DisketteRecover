/*
 * DisketteRecover - a very small single-threaded HTTP server so the
 * zoomed view can live in a browser without dragging in a GUI toolkit.
 *
 * Endpoints:
 *   GET  /                     the viewer page
 *   GET  /api/scan             sector index + CRC status
 *   GET  /api/view?sector=N    cells, flux bins, decoded bytes
 *   GET  /api/repair?sector=N&weight=W&threshold=P&...
 *   POST /api/apply?sector=N&rank=K
 *   POST /api/export?path=...&format=...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dr_internal.h"

extern const char dr_web_index[];

static int qs_int(const char *q, const char *key, int def)
{
	const char *p;
	char pat[64];

	if (!q)
		return def;
	snprintf(pat, sizeof(pat), "%s=", key);
	p = strstr(q, pat);
	if (!p)
		return def;
	if (p != q && p[-1] != '&' && p[-1] != '?')
		return def;
	return atoi(p + strlen(pat));
}

static double qs_dbl(const char *q, const char *key, double def)
{
	const char *p;
	char pat[64];

	if (!q)
		return def;
	snprintf(pat, sizeof(pat), "%s=", key);
	p = strstr(q, pat);
	if (!p)
		return def;
	if (p != q && p[-1] != '&' && p[-1] != '?')
		return def;
	return atof(p + strlen(pat));
}

static int qs_str(const char *q, const char *key, char *out, size_t n)
{
	const char *p, *e;
	char pat[64];
	size_t i = 0;

	out[0] = 0;
	if (!q)
		return 0;
	snprintf(pat, sizeof(pat), "%s=", key);
	p = strstr(q, pat);
	if (!p)
		return 0;
	p += strlen(pat);
	for (e = p; *e && *e != '&' && i + 1 < n; e++) {
		if (*e == '%' && e[1] && e[2]) {
			char hex[3] = { e[1], e[2], 0 };
			out[i++] = (char)strtol(hex, NULL, 16);
			e += 2;
		} else if (*e == '+') {
			out[i++] = ' ';
		} else {
			out[i++] = *e;
		}
	}
	out[i] = 0;
	return (int)i;
}

static void send_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);
		if (n <= 0)
			return;
		buf += n;
		len -= (size_t)n;
	}
}

static void send_response(int fd, const char *status, const char *ctype,
                          const char *body, size_t len)
{
	char head[256];
	int n = snprintf(head, sizeof(head),
	                 "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
	                 "Content-Length: %zu\r\nCache-Control: no-store\r\n"
	                 "Connection: close\r\n\r\n", status, ctype, len);
	send_all(fd, head, (size_t)n);
	send_all(fd, body, len);
}

/* Render a JSON producer into memory, then ship it. */
typedef void (*json_fn)(void *arg, FILE *f);

static void send_json(int fd, json_fn fn, void *arg)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&buf, &len);

	if (!f) {
		send_response(fd, "500 Internal Error", "text/plain", "oom", 3);
		return;
	}
	fn(arg, f);
	fclose(f);
	send_response(fd, "200 OK", "application/json", buf, len);
	free(buf);
}

/* ---- request context ---------------------------------------------- */
struct srv {
	dr_ctx     *ctx;
	dr_options  opt;
	int         local_only;   /* bound to loopback                     */

	/* cached view + search for the sector currently on screen */
	dr_view          *view;
	dr_repair_result  res;
	int               view_sector;
	int               have_res;
};

static void drop_cache(struct srv *s)
{
	dr_repair_free(&s->res);
	if (s->view)
		dr_view_free(s->view);
	s->view = NULL;
	s->view_sector = -1;
	s->have_res = 0;
}

static dr_view *ensure_view(struct srv *s, int sector)
{
	if (s->view && s->view_sector == sector)
		return s->view;
	drop_cache(s);
	s->view = dr_view_open(s->ctx, sector, &s->opt);
	s->view_sector = sector;
	return s->view;
}

struct jscan { dr_ctx *c; };
static void j_scan(void *a, FILE *f)
{
	dr_json_scan(((struct jscan *)a)->c, f);
}

struct jview { dr_ctx *c; dr_view *v; };
static void j_view(void *a, FILE *f)
{
	struct jview *p = a;
	dr_json_view(p->c, p->v, f);
}

struct jcand { dr_view *v; dr_repair_result *r; int off, lim; };
static void j_cand(void *a, FILE *f)
{
	struct jcand *p = a;
	dr_json_candidates(p->v, p->r, p->off, p->lim, f);
}

static void handle(struct srv *s, int fd, const char *method,
                   const char *path, const char *query)
{
	if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
		send_response(fd, "200 OK", "text/html; charset=utf-8",
		              dr_web_index, strlen(dr_web_index));
		return;
	}

	if (!strcmp(path, "/api/scan")) {
		struct jscan a = { s->ctx };
		send_json(fd, j_scan, &a);
		return;
	}

	if (!strcmp(path, "/api/view")) {
		int sector = qs_int(query, "sector", dr_first_bad(s->ctx));
		dr_view *v;
		struct jview a;

		if (sector < 0)
			sector = 0;
		s->opt.base_perr      = qs_dbl(query, "base", s->opt.base_perr);
		s->opt.jitter         = qs_dbl(query, "jitter", s->opt.jitter);
		s->opt.good_threshold = qs_dbl(query, "threshold",
		                               s->opt.good_threshold);
		drop_cache(s);
		v = ensure_view(s, sector);
		if (!v) {
			send_response(fd, "404 Not Found", "application/json",
			              "{\"error\":\"no such sector\"}", 26);
			return;
		}
		a.c = s->ctx;
		a.v = v;
		send_json(fd, j_view, &a);
		return;
	}

	if (!strcmp(path, "/api/repair")) {
		int sector = qs_int(query, "sector", s->view_sector);
		dr_view *v;
		struct jcand a;

		s->opt.max_weight     = qs_int(query, "weight", s->opt.max_weight);
		s->opt.max_pool       = qs_int(query, "pool", s->opt.max_pool);
		s->opt.good_threshold = qs_dbl(query, "threshold",
		                               s->opt.good_threshold);
		s->opt.jitter         = qs_dbl(query, "jitter", s->opt.jitter);

		if (sector < 0)
			sector = dr_first_bad(s->ctx);
		v = ensure_view(s, sector);
		if (!v) {
			send_response(fd, "404 Not Found", "application/json",
			              "{\"error\":\"no such sector\"}", 26);
			return;
		}
		if (!s->have_res) {
			dr_repair_search(v, &s->opt, &s->res);
			s->have_res = 1;
		}
		a.v = v;
		a.r = &s->res;
		a.off = qs_int(query, "offset", 0);
		a.lim = qs_int(query, "limit", 64);
		send_json(fd, j_cand, &a);
		return;
	}

	if (!strcmp(path, "/api/apply") && !strcmp(method, "POST")) {
		int sector = qs_int(query, "sector", s->view_sector);
		int rank = qs_int(query, "rank", 0);
		dr_view *v = ensure_view(s, sector);
		char body[256];
		int ok;

		if (!v || !s->have_res || rank < 0 || rank >= s->res.count) {
			send_response(fd, "400 Bad Request", "application/json",
			              "{\"error\":\"no such candidate\"}", 29);
			return;
		}
		if (dr_apply(s->ctx, v, &s->res.list[rank]) < 0) {
			send_response(fd, "500 Internal Error", "application/json",
			              "{\"error\":\"apply failed\"}", 24);
			return;
		}
		ok = dr_verify(s->ctx, sector);
		drop_cache(s);
		snprintf(body, sizeof(body),
		         "{\"applied\":%d,\"verified\":%s}", rank,
		         ok == 1 ? "true" : "false");
		send_response(fd, "200 OK", "application/json", body, strlen(body));
		return;
	}

	if (!strcmp(path, "/api/export") && !strcmp(method, "POST")) {
		char p[512], fmt[64], body[700];

		/* This endpoint writes a file wherever it is told to, so it
		 * only exists for a viewer on this machine. */
		if (!s->local_only) {
			send_response(fd, "403 Forbidden", "application/json",
			              "{\"error\":\"export is loopback-only\"}", 36);
			return;
		}

		qs_str(query, "path", p, sizeof(p));
		qs_str(query, "format", fmt, sizeof(fmt));
		if (!p[0]) {
			send_response(fd, "400 Bad Request", "application/json",
			              "{\"error\":\"path required\"}", 25);
			return;
		}
		if (!fmt[0])
			snprintf(fmt, sizeof(fmt), "hfe");

		if (dr_export(s->ctx, p, fmt) < 0)
			snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
			         dr_last_error(s->ctx));
		else
			snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\"}", p);
		send_response(fd, "200 OK", "application/json", body, strlen(body));
		return;
	}

	send_response(fd, "404 Not Found", "text/plain", "not found", 9);
}

int dr_serve(dr_ctx *c, const char *bind_addr, int port, const dr_options *o)
{
	struct srv s;
	struct sockaddr_in sa;
	int sock, one = 1;

	memset(&s, 0, sizeof(s));
	s.ctx = c;
	s.view_sector = -1;
	if (o)
		s.opt = *o;
	else
		dr_options_default(&s.opt);

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1)
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	s.local_only = ((ntohl(sa.sin_addr.s_addr) >> 24) == 127);

	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "bind %s:%d: %s\n", bind_addr, port,
		        strerror(errno));
		close(sock);
		return 1;
	}
	if (listen(sock, 8) < 0) {
		perror("listen");
		close(sock);
		return 1;
	}

	fprintf(stderr, "DisketteRecover viewer on http://%s:%d/  (ctrl-c to stop)\n",
	        bind_addr, port);
	if (!s.local_only)
		fprintf(stderr, "warning: not bound to loopback - the export "
		                "endpoint is disabled\n");

	for (;;) {
		char req[8192];
		char method[16], target[2048];
		char *sp, *query;
		ssize_t n;
		int fd = accept(sock, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		n = read(fd, req, sizeof(req) - 1);
		if (n <= 0) {
			close(fd);
			continue;
		}
		req[n] = 0;

		if (sscanf(req, "%15s %2047s", method, target) != 2) {
			close(fd);
			continue;
		}

		sp = strchr(target, '?');
		if (sp) {
			*sp = 0;
			query = sp + 1;
		} else {
			query = NULL;
		}

		handle(&s, fd, method, target, query);
		close(fd);
	}

	drop_cache(&s);
	close(sock);
	return 0;
}
