/*
 * DisketteRecover - command line front end.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "dr_internal.h"

extern const char dr_web_plot[];

static const char *usage_text =
"DisketteRecover - CRC-guided flux-level floppy repair (built on libhxcfe)\n"
"\n"
"usage: disketterecover <command> [options] <image>\n"
"\n"
"commands:\n"
"  scan     <image>              list every sector and flag CRC failures\n"
"  inspect  <image>              zoomed view of one sector's cells+bytes\n"
"  repair   <image>              search for the most likely CRC-valid fix\n"
"  damage   <image>              flip data bits on purpose (test images)\n"
"  serve    <image>              browser UI for inspect + repair\n"
"  convert  <image>              write the image out in another format\n"
"  plot     <image>              write an interactive scatter of the flux\n"
"                                transition widths to --out FILE.html\n"
"  formats                       list libhxcfe export formats\n"
"\n"
"inspect options:\n"
"  --bytes A:B       zoom in on message bytes A..B - shows the cells, the\n"
"                    flux interval each reversal came from, and the bits\n"
"                    they decode to\n"
"\n"
"selection:\n"
"  --sector N        sector index from `scan` (default: first bad CRC)\n"
"  --track T --side S --id R     select by physical address instead\n"
"\n"
"engine options:\n"
"  --mode M          auto | pattern | rebin | bits   (default auto)\n"
"                      pattern: restore the repeat the data almost obeys\n"
"                      rebin  : re-read the flux under another legal\n"
"                               binning of the transitions (MFM + flux)\n"
"                      bits   : search bit flips (works without flux)\n"
"                    auto tries pattern, then rebin, then bits\n"
"  --max-outliers N  pattern engine: bytes allowed off-pattern (24)\n"
"  --dropout-bias N  nats favouring a lost 1 over a gained 1 (1.6)\n"
"  --burst-gain G    how much likelier an error is right after another\n"
"                    one - errors clump, so a flip beside a flip is one\n"
"                    event, not two (110; 0 turns the burst prior off)\n"
"  --burst-len B     bits over which that lift decays (60)\n"
"  --restore-only    only consider putting dropped reversals back;\n"
"                    every verified error so far has been a 1 read as 0\n"
"  --bin-budget N    nats of timing cost a re-bin may spend (12)\n"
"  --max-ambiguous N refuse to search past this many open intervals (40)\n"
"  --max-explore N   cap on re-binning assignments tested (2000000)\n"
"\n"
"model options:\n"
"  --threshold P     p_err below this is 'assumed good' (default 1e-3)\n"
"  --base-perr P     prior error rate with no timing evidence (2e-4)\n"
"  --jitter S        flux jitter sigma in cell periods (0.16)\n"
"  --max-weight N    deepest error weight to search (3, max 6)\n"
"  --max-pool N      how many low-confidence bits to consider (96)\n"
"  --max-results N   cap on returned candidates (256)\n"
"\n"
"repair options:\n"
"  --all             work through every sector with a CRC error\n"
"  --apply K         apply candidate K (0 = most likely) and verify\n"
"  --auto            apply candidate 0 only when it clearly wins\n"
"  --out FILE        write the patched image\n"
"  --format NAME     export format for --out (default: hfe)\n"
"\n"
"damage options:\n"
"  --bits a,b,c      message bit indices to flip\n"
"  --drop-only       only clear bits that read 1, so the damage is a\n"
"                    lost reversal - what real media actually does\n"
"  --out FILE        required\n"
"\n"
"other:\n"
"  --set NAME=VALUE  override a libhxcfe setting before loading, e.g.\n"
"                    --set FLUXSTREAM_PLL_MAX_ERROR_NS=900 (repeatable)\n"
"  --json            machine readable output\n"
"  --port N          port for `serve` (default 842)\n"
"  --bind ADDR       bind address for `serve` (default 127.0.0.1)\n"
"  -v / -vv          libhxcfe chatter\n";

typedef struct {
	const char *cmd;
	const char *image;
	int    sector;
	int    track, side, id;
	int    json;
	int    verbose;
	int    apply;
	int    autoapply;
	int    all;
	int    droponly;
	int    port;
	const char *bind;
	const char *out;
	const char *format;
	const char *bits;
	const char *bytes;
	char *const *sets;
	int    nsets;
	dr_options opt;
} args;

static int parse_args(int argc, char **argv, args *a)
{
	int i;
	static char *setbuf[32];

	memset(a, 0, sizeof(*a));
	a->sets = setbuf;
	dr_options_default(&a->opt);
	a->sector = -1;
	a->track = a->side = a->id = -1;
	a->apply = -1;
	a->port = 842;
	a->bind = "127.0.0.1";
	a->format = "HXC_HFE";

	if (argc < 2)
		return -1;
	a->cmd = argv[1];

	for (i = 2; i < argc; i++) {
		const char *o = argv[i];
		#define NEXT() (++i < argc ? argv[i] : "")

		if (!strcmp(o, "--sector"))        a->sector = atoi(NEXT());
		else if (!strcmp(o, "--track"))    a->track = atoi(NEXT());
		else if (!strcmp(o, "--side"))     a->side = atoi(NEXT());
		else if (!strcmp(o, "--id"))       a->id = atoi(NEXT());
		else if (!strcmp(o, "--threshold"))a->opt.good_threshold = atof(NEXT());
		else if (!strcmp(o, "--base-perr"))a->opt.base_perr = atof(NEXT());
		else if (!strcmp(o, "--jitter"))   a->opt.jitter = atof(NEXT());
		else if (!strcmp(o, "--max-weight"))a->opt.max_weight = atoi(NEXT());
		else if (!strcmp(o, "--max-pool")) a->opt.max_pool = atoi(NEXT());
		else if (!strcmp(o, "--max-results"))a->opt.max_results = atoi(NEXT());
		else if (!strcmp(o, "--bin-budget")) a->opt.bin_budget = atof(NEXT());
		else if (!strcmp(o, "--max-explore")) a->opt.max_explore = atol(NEXT());
		else if (!strcmp(o, "--max-ambiguous")) a->opt.max_ambiguous = atoi(NEXT());
		else if (!strcmp(o, "--max-outliers")) a->opt.max_outliers = atoi(NEXT());
		else if (!strcmp(o, "--dropout-bias")) a->opt.dropout_bias = atof(NEXT());
		else if (!strcmp(o, "--burst-gain")) a->opt.burst_gain = atof(NEXT());
		else if (!strcmp(o, "--burst-len"))  a->opt.burst_len  = atof(NEXT());
		else if (!strcmp(o, "--restore-only")) a->opt.restore_only = 1;
		else if (!strcmp(o, "--drop-only")) a->droponly = 1;
		else if (!strcmp(o, "--mode")) {
			const char *m = NEXT();
			if (!strcmp(m, "bits"))         a->opt.mode = DR_MODE_BITS;
			else if (!strcmp(m, "rebin"))   a->opt.mode = DR_MODE_REBIN;
			else if (!strcmp(m, "pattern")) a->opt.mode = DR_MODE_PATTERN;
			else                            a->opt.mode = DR_MODE_AUTO;
		}
		else if (!strcmp(o, "--apply"))    a->apply = atoi(NEXT());
		else if (!strcmp(o, "--auto"))     a->autoapply = 1;
		else if (!strcmp(o, "--all"))      a->all = 1;
		else if (!strcmp(o, "--out"))      a->out = NEXT();
		else if (!strcmp(o, "--format"))   a->format = NEXT();
		else if (!strcmp(o, "--bits"))     a->bits = NEXT();
		else if (!strcmp(o, "--bytes"))    a->bytes = NEXT();
		else if (!strcmp(o, "--port"))     a->port = atoi(NEXT());
		else if (!strcmp(o, "--bind"))     a->bind = NEXT();
		else if (!strcmp(o, "--set")) {
			if (a->nsets < 32)
				setbuf[a->nsets++] = (char *)NEXT();
			else
				(void)NEXT();
		}
		else if (!strcmp(o, "--json"))     a->json = 1;
		else if (!strcmp(o, "-v"))         a->verbose = 1;
		else if (!strcmp(o, "-vv"))        a->verbose = 2;
		else if (o[0] == '-') {
			fprintf(stderr, "unknown option '%s'\n", o);
			return -1;
		} else if (!a->image)
			a->image = o;
		else {
			fprintf(stderr, "unexpected argument '%s'\n", o);
			return -1;
		}
		#undef NEXT
	}
	return 0;
}

/* Resolve --sector / --track/--side/--id / first bad. */
static int pick_sector(dr_ctx *c, const args *a)
{
	int i, n;
	const dr_sector *s = dr_sectors(c, &n);

	if (a->sector >= 0) {
		if (a->sector >= n) {
			fprintf(stderr, "sector index %d out of range (%d sectors)\n",
			        a->sector, n);
			return -1;
		}
		return a->sector;
	}

	if (a->track >= 0 || a->id >= 0) {
		for (i = 0; i < n; i++) {
			if (a->track >= 0 && s[i].track != a->track) continue;
			if (a->side  >= 0 && s[i].side  != a->side)  continue;
			if (a->id    >= 0 && s[i].sector_id != a->id) continue;
			return i;
		}
		fprintf(stderr, "no sector matches the given address\n");
		return -1;
	}

	i = dr_first_bad(c);
	if (i < 0) {
		if (n > 0)
			fprintf(stderr, "no CRC error found; pass --sector to pick one\n");
		else
			fprintf(stderr, "no sectors decoded from this image\n");
		return -1;
	}
	return i;
}

/* ------------------------------------------------------------------ */
static void print_scan(dr_ctx *c)
{
	int i, n, bad = 0;
	const dr_sector *s = dr_sectors(c, &n);

	printf("image     : %s\n", dr_path(c));
	printf("geometry  : %d tracks x %d sides, %d sectors decoded\n",
	       dr_tracks(c), dr_sides(c), n);
	printf("\n idx  trk sd  ord   id  size  enc      hdr-crc  data-crc  data-cell\n");
	printf(" ---- --- --  ---  ---  ----  -------  -------  --------  ---------\n");

	for (i = 0; i < n; i++) {
		int isbad = (s[i].header_crc == DR_CRC_BAD ||
		             s[i].data_crc == DR_CRC_BAD);
		if (isbad)
			bad++;
		printf(" %4d %3d  %d  %3d  %3d  %4d  %-7s  %-7s  %-8s  %9d%s\n",
		       i, s[i].track, s[i].side, s[i].order, s[i].sector_id,
		       s[i].sector_size,
		       s[i].encoding == DR_ENC_ISO_MFM ? "ISO/MFM" : "ISO/FM",
		       s[i].header_crc == DR_CRC_OK ? "ok" : "BAD",
		       s[i].data_crc == DR_CRC_OK ? "ok" :
		           (s[i].data_crc == DR_CRC_BAD ? "BAD" : "-"),
		       s[i].data_cell, isbad ? "  <--" : "");
	}

	printf("\n%d sector(s) with a CRC error", bad);
	i = dr_first_bad(c);
	if (i >= 0)
		printf("; first is index %d (track %d side %d sector %d)",
		       i, s[i].track, s[i].side, s[i].sector_id);
	printf("\n");
}

/* ------------------------------------------------------------------ */
static const char *bar(double p)
{
	if (p >= 0.30) return "#####";
	if (p >= 0.10) return "#### ";
	if (p >= 0.02) return "###  ";
	if (p >= 1e-3) return "##   ";
	if (p >= 1e-5) return "#    ";
	return "     ";
}

static void print_view(dr_view *v, int top)
{
	int i, k, n;
	int *ord;

	printf("sector    : index %d - track %d side %d, id %d, %d bytes\n",
	       v->sector_index, v->sect.track, v->sect.side, v->sect.sector_id,
	       v->sect.sector_size);
	printf("encoding  : %s, %d cells/byte, bitrate %d\n",
	       v->encoding == DR_ENC_ISO_MFM ? "ISO/IBM MFM" : "ISO/IBM FM",
	       v->stride, v->sect.bitrate);
	printf("field     : %s field, %d bytes of CRC-covered message "
	       "starting at cell %d\n", v->field, v->msg_len, v->base_cell);
	printf("crc       : stored %04X, computed %04X, syndrome %04X -> %s\n",
	       v->stored_crc, v->computed_crc, v->syndrome,
	       v->syndrome ? "INVALID" : "valid");
	printf("evidence  : %s%s\n", v->model,
	       v->flux_available ? "" : "  (no flux stream in this image)");

	/* --- decoded bytes with a confidence bar ---------------------- */
	printf("\ndecoded message (worst-bit confidence bar per byte)\n");
	for (i = 0; i < v->msg_len; i += 16) {
		printf("  %04X ", i);
		for (k = 0; k < 16 && i + k < v->msg_len; k++)
			printf("%02X ", v->msg[i + k]);
		for (; k < 16; k++)
			printf("   ");
		printf(" |");
		for (k = 0; k < 16 && i + k < v->msg_len; k++) {
			unsigned char ch = v->msg[i + k];
			putchar((ch >= 32 && ch < 127) ? ch : '.');
		}
		printf("|\n");
	}

	/* --- least trusted bits --------------------------------------- */
	if (top <= 0)
		return;
	n = v->msg_bits - v->first_bit;
	if (n <= 0)
		return;
	ord = malloc((size_t)n * sizeof(int));
	if (!ord)
		return;
	for (i = 0; i < n; i++)
		ord[i] = v->first_bit + i;
	for (i = 1; i < n; i++) {          /* insertion sort, n is small */
		int key = ord[i], j = i - 1;
		while (j >= 0 && v->bit_perr[ord[j]] < v->bit_perr[key]) {
			ord[j + 1] = ord[j];
			j--;
		}
		ord[j + 1] = key;
	}

	printf("\nleast trusted bits in this field (the search starts here)\n");
	printf("  bit    byte  role  bitpos  cell    state  interval      bin   "
	       "margin  p_err   evidence\n");
	for (i = 0; i < top && i < n; i++) {
		int p = ord[i];
		int byte = p >> 3, bit = p & 7;
		int cc, dc;
		dr_cell *dcell;

		dr_bit_cells(v->encoding, byte * v->stride, bit, &cc, &dc);
		dcell = &v->cells[dc];

		printf("  %-6d %-5d %-5s %-7d %-7d %-6d ", p, byte,
		       v->bytes[byte].role, bit, dcell->cell, dcell->state);
		if (dcell->interval_ticks >= 0)
			printf("%6.3fT/%-5d ", dcell->interval_cells,
			       dcell->interval_ticks);
		else
			printf("%-13s ", "-");
		if (dcell->bin >= 0)
			printf("%-5d ", dcell->bin);
		else
			printf("%-5s ", "-");
		if (dcell->margin >= 0.0f)
			printf("%-7.3f ", dcell->margin);
		else
			printf("%-7s ", "-");
		printf("%-7.2g %s %s\n", v->bit_perr[p],
		       dcell->evidence == DR_EV_FLUX ? "flux" :
		       dcell->evidence == DR_EV_WEAKBIT ? "weak" :
		       dcell->evidence == DR_EV_VIOLATION ? "cell-violation" : "prior",
		       bar(v->bit_perr[p]));
	}
	free(ord);
}

/* ------------------------------------------------------------------ */
/*
 * How far the best reading beats the next one.
 *
 * A runner-up whose relative likelihood has underflowed to zero is not a
 * close call - it is the most decisive result the ranking can produce,
 * and reporting it as a margin of zero gets the conclusion exactly
 * backwards.
 */
static double top_margin(const dr_repair_result *r)
{
	if (r->count <= 1)
		return HUGE_VAL;            /* nothing else to weigh it against */
	if (r->list[1].rel_likelihood <= 0.0)
		return HUGE_VAL;
	return 1.0 / r->list[1].rel_likelihood;
}

static void print_margin(const dr_repair_result *r, const char *what)
{
	double m = top_margin(r);

	if (r->count <= 1)
		return;
	if (m == HUGE_VAL)
		printf("margin    : the top %s is overwhelmingly more likely "
		       "than the next; clear winner\n", what);
	else
		printf("margin    : the top %s is %.3g x more likely than the "
		       "next; %s\n", what, m,
		       m >= 100.0 ? "clear winner"
		                  : "NOT a clear winner - inspect before applying");
}

/*
 * What a CRC-valid reading is actually worth.
 *
 * A 16-bit CRC lets one reading in 65536 through by chance, so the
 * question is never "does it pass" but "how many readings were even on
 * offer". A model that narrows the field to a few hundred candidates
 * makes a single survivor decisive; a search that leaves thirty bytes
 * free will always find one, and it will mean nothing.
 */
static void print_budget(dr_repair_result *r)
{
	double chance;

	if (r->explored > 0) {
		chance = (double)r->explored / 65536.0;
		printf("budget    : %ld reading(s) were possible; a 16-bit CRC "
		       "passes ~%.3g of them\n", r->explored, chance);
		if (r->count == 1 && chance < 0.2)
			printf("            by chance, so the single survivor is "
			       "~%.1f%% likely to be right\n",
			       100.0 * (1.0 - chance));
		else if (chance > 2.0)
			printf("            by chance - too many to trust any "
			       "single survivor\n");
	}
	if (r->uncertain_bits > 16)
		printf("            %d message bit(s) are in doubt; the CRC "
		       "pins down 16\n", r->uncertain_bits);
}

/*
 * The zoomed view: for each byte, the sixteen cells it occupies, the bit
 * they decode to, and the flux interval that put each reversal where it
 * is. This is the picture the whole tool is built around - everything
 * else is a search over what these numbers could have meant.
 */
static void print_bytes(dr_view *v, int b0, int b1)
{
	int b, i, k;

	if (b0 < 0) b0 = 0;
	if (b1 >= v->msg_len) b1 = v->msg_len - 1;

	printf("\nzoom: message bytes %d..%d   ('|' = flux reversal, "
	       "'.' = none)\n", b0, b1);
	printf("      each byte spans %d cells; a data bit is a reversal in "
	       "its data cell\n      with none in its clock cell\n\n",
	       v->stride);

	for (b = b0; b <= b1; b++) {
		int base = b * v->stride;
		char cells[80], bits[80];
		int first_iv = 1;

		if (base + v->stride > v->ncells)
			break;

		memset(cells, ' ', sizeof(cells));
		memset(bits, ' ', sizeof(bits));

		for (i = 0; i < v->stride; i++)
			cells[i] = v->cells[base + i].state ? '|' : '.';
		for (k = 0; k < 8; k++) {
			int cc, dc, bit;

			dr_bit_cells(v->encoding, 0, k, &cc, &dc);
			bit = (v->encoding == DR_ENC_ISO_FM)
			        ? v->cells[base + dc].state
			        : (!v->cells[base + cc].state &&
			           v->cells[base + dc].state);
			bits[dc] = (char)('0' + bit);
		}
		cells[v->stride] = 0;
		bits[v->stride] = 0;

		printf(" %4d %-5s %02X %c  %s\n", b, v->bytes[b].role,
		       v->msg[b],
		       (v->msg[b] >= 32 && v->msg[b] < 127) ? v->msg[b] : '.',
		       cells);
		printf("                   %s", bits);

		for (i = 0; i < v->stride; i++) {
			dr_cell *c = &v->cells[base + i];

			if (!c->state || c->interval_ticks < 0)
				continue;
			if (first_iv) {
				printf("   ");
				first_iv = 0;
			} else {
				printf(", ");
			}
			printf("%.2fT->%dT", c->interval_cells, c->bin);
			if (c->best_bin > 0 && c->best_bin != c->bin)
				printf(" (fits %dT, p=%.2f)",
				       c->best_bin, c->p_bin);
		}
		printf("\n");
	}
}

static void print_pattern(dr_view *v, dr_repair_result *r, int limit)
{
	int i, k;

	printf("\ndata      : %s\n", r->note);
	printf("search    : restoring the repeat - %d byte(s) break it; "
	       "%ld reading(s) tested\n", r->outliers, r->explored);

	if (!r->count) {
		printf("result    : restoring the pattern does not satisfy "
		       "the CRC.\n");
		return;
	}

	printf("result    : %d CRC-valid reading(s) from the data model\n",
	       r->count);
	print_margin(r, "reading");

	print_budget(r);

	printf("\n rank  bytes  bits  restore  remove  data evidence  "
	       "changed bytes\n");
	printf(" ----  -----  ----  -------  ------  -------------  "
	       "----------------------------\n");

	for (i = 0; i < r->count && i < limit; i++) {
		dr_candidate *cd = &r->list[i];
		uint8_t *m = dr_candidate_message(v, cd);
		int shown = 0, lastbyte = -1, nbytes = 0;

		for (k = 0; k < cd->weight; k++)
			if ((cd->bits[k] >> 3) != lastbyte) {
				lastbyte = cd->bits[k] >> 3;
				nbytes++;
			}
		lastbyte = -1;

		printf(" %4d  %5d  %4d  %7d  %6d  %8.1f nats  ", i, nbytes,
		       cd->weight, cd->restores, cd->removes, cd->data_prior);
		for (k = 0; k < cd->weight && m; k++) {
			int b = cd->bits[k] >> 3;
			if (b == lastbyte)
				continue;
			lastbyte = b;
			if (shown == 5) { printf(", ..."); break; }
			printf("%s%d:%02X->%02X", shown ? ", " : "",
			       b, v->msg[b], m[b]);
			shown++;
		}
		printf("\n");
		free(m);
	}
}

/* Re-binning results move whole bursts, so list the bytes that changed
 * rather than every individual bit. */
static void print_rebin(dr_view *v, dr_repair_result *r, int limit)
{
	int i, k;

	printf("\nsearch    : re-binning the flux - %d interval(s) left open by "
	       "the timings,\n"
	       "            %d that the timings re-read on their own; "
	       "%ld assignment(s) tested\n",
	       r->ambiguous, r->pinned_moves, r->explored);
	if (r->note[0])
		printf("            %s\n", r->note);

	if (!r->count) {
		printf("result    : no legal re-binning satisfies the CRC.\n");
		if (r->truncated)
			printf("            the search hit its cap - try a larger "
			       "--max-explore or --bin-budget.\n");
		return;
	}

	printf("result    : %d CRC-valid re-reading(s)%s\n", r->count,
	       r->truncated ? " (search capped)" : "");
	printf("            likeliest reading costs %.1f nats and %s the CRC\n",
	       r->floor_cost, r->floor_valid ? "satisfies" : "does NOT satisfy");
	if (r->count)
		printf("            best CRC-valid reading costs %.1f nats "
		       "(%.1f more than the likeliest)\n",
		       r->list[0].flux_cost,
		       r->list[0].flux_cost - r->floor_cost);
	print_margin(r, "re-reading");

	print_budget(r);
	if (r->current_cost > 0.0)
		printf("            re-binning explains the timings far better "
		       "than the decoder did:\n"
		       "            %.0f nats -> %.0f nats over the disturbed "
		       "intervals\n", r->current_cost, r->floor_cost);

	printf("\n rank  bits  re-bins  rel.likelihood  changed bytes\n");
	printf(" ----  ----  -------  --------------  "
	       "------------------------------------\n");

	for (i = 0; i < r->count && i < limit; i++) {
		dr_candidate *cd = &r->list[i];
		uint8_t *m = dr_candidate_message(v, cd);
		int shown = 0, lastbyte = -1;

		printf(" %4d  %4d  %7d  %14.6g  ", i, cd->weight, cd->rebins,
		       cd->rel_likelihood);
		for (k = 0; k < cd->weight && m; k++) {
			int b = cd->bits[k] >> 3;
			if (b == lastbyte)
				continue;
			lastbyte = b;
			if (shown == 6) {
				printf(", ...");
				break;
			}
			printf("%s%d:%02X->%02X", shown ? ", " : "",
			       b, v->msg[b], m[b]);
			shown++;
		}
		printf("\n");
		free(m);
	}
}

static void print_candidates(dr_view *v, dr_repair_result *r, int limit)
{
	int i, k;

	printf("\nsearch    : %d low-confidence bit(s) in the pool, "
	       "explored up to weight %d\n", r->npool, r->searched_weight);

	if (!r->count) {
		printf("result    : no CRC-valid correction found within the "
		       "search limits.\n"
		       "            try --max-weight 4 or a larger --max-pool / "
		       "higher --threshold.\n");
		return;
	}

	printf("result    : %d CRC-valid candidate(s) at weight %d%s\n",
	       r->count, r->list[0].weight,
	       r->truncated ? " (list truncated)" : "");

	/* A 16-bit CRC only pins the data down to 1 in 65536, and a field
	 * this long offers thousands of places to flip, so alternative
	 * readings are normal. Say how much the top one actually wins by. */
	print_margin(r, "candidate");
	if (!v->flux_available)
		printf("note      : no flux timing in this image, so every bit "
		       "carries the same prior.\n"
		       "            Candidates of equal weight cannot be told "
		       "apart; the lowest weight\n"
		       "            is not necessarily the true error. Re-dump "
		       "as flux (SCP/KryoFlux/A2R)\n"
		       "            to get a real ranking.\n");
	printf("\n rank  w  rel.likelihood  flips\n");
	printf(" ----  -  --------------  ---------------------------------\n");

	for (i = 0; i < r->count && i < limit; i++) {
		dr_candidate *cd = &r->list[i];
		printf(" %4d  %d  %14.6g  ", i, cd->weight, cd->rel_likelihood);
		for (k = 0; k < cd->weight; k++) {
			int p = cd->bits[k];
			printf("%sbyte %d bit %d: %d->%d (p=%.3g)",
			       k ? ", " : "", p >> 3, p & 7,
			       cd->before[k], !cd->before[k], v->bit_perr[p]);
		}
		printf("\n");
	}
}

/* Run the engine cascade for one sector and report in one line.
 * Returns 1 if the sector was repaired and verified. */
static int repair_one(dr_ctx *c, int idx, args *a, int apply)
{
	dr_view *v;
	dr_repair_result r;
	dr_mode engine;
	const dr_sector *sl;
	int n, rc = 0;
	double margin = 0.0;

	sl = dr_sectors(c, &n);
	v = dr_view_open(c, idx, &a->opt);
	if (!v) {
		printf(" %3d/%d s%-3d  %-8s  %s\n", sl[idx].track, sl[idx].side,
		       sl[idx].sector_id, "-", "no view (unsupported encoding)");
		return 0;
	}

	if (a->opt.mode == DR_MODE_AUTO) {
		dr_repair_auto(c, v, &a->opt, &r);
		engine = r.count ? r.list[0].origin : DR_MODE_BITS;
	} else {
		engine = a->opt.mode;
		memset(&r, 0, sizeof(r));
		if (engine == DR_MODE_PATTERN)
			dr_pattern_search(v, &a->opt, &r);
		else if (engine == DR_MODE_REBIN)
			dr_rebin_search(v, &a->opt, &r);
		else
			dr_repair_search(v, &a->opt, &r);
		dr_rescore_data(c, v, &a->opt, &r);
	}

	margin = top_margin(&r);

	printf(" %3d/%d s%-3d  %-8s  ",
	       sl[idx].track, sl[idx].side, sl[idx].sector_id,
	       engine == DR_MODE_PATTERN ? "pattern" :
	       engine == DR_MODE_REBIN ? "re-bin" : "bits");

	if (!r.count) {
		printf("no CRC-valid reading found");
		if (r.uncertain_bits > 16)
			printf(" (%d bits in doubt vs 16 of CRC)",
			       r.uncertain_bits);
		printf("\n");
	} else {
		int unique = (r.count == 1);

		printf("%d reading(s), ", r.count);
		if (unique)
			printf("unique");
		else if (margin == HUGE_VAL)
			printf("top overwhelms the next");
		else
			printf("top %.3g x next", margin);

		if (apply && (unique || margin >= 100.0)) {
			if (dr_apply(c, v, &r.list[0]) == 0 &&
			    dr_verify(c, idx) == 1) {
				printf("  -> APPLIED, verifies clean");
				rc = 1;
			} else {
				printf("  -> apply failed");
			}
		} else if (apply) {
			printf("  -> not applied (ambiguous)");
		}
		printf("\n");
	}

	dr_repair_free(&r);
	dr_view_free(v);
	return rc;
}

static int cmd_repair_all(dr_ctx *c, args *a)
{
	struct { int track, side, id; } *todo = NULL;
	int n, i, ntodo = 0, fixed = 0, apply;
	const dr_sector *sl = dr_sectors(c, &n);

	apply = (a->apply >= 0 || a->autoapply);

	for (i = 0; i < n; i++)
		if (sl[i].header_crc == DR_CRC_BAD ||
		    sl[i].data_crc == DR_CRC_BAD) {
			todo = realloc(todo, (size_t)(ntodo + 1) * sizeof(*todo));
			if (!todo)
				return 1;
			todo[ntodo].track = sl[i].track;
			todo[ntodo].side = sl[i].side;
			todo[ntodo].id = sl[i].sector_id;
			ntodo++;
		}

	if (!ntodo) {
		printf("no CRC errors on this disk.\n");
		free(todo);
		return 0;
	}

	printf("%d sector(s) with a CRC error%s\n\n", ntodo,
	       apply ? "; applying unambiguous repairs" : "");
	printf(" trk/s sect  engine    result\n");
	printf(" ----- ----  --------  "
	       "---------------------------------------------------\n");

	for (i = 0; i < ntodo; i++) {
		int idx = -1, j;

		/* Applying re-scans, so locate the sector by address. */
		sl = dr_sectors(c, &n);
		for (j = 0; j < n; j++)
			if (sl[j].track == todo[i].track &&
			    sl[j].side == todo[i].side &&
			    sl[j].sector_id == todo[i].id) {
				idx = j;
				break;
			}
		if (idx < 0)
			continue;

		fixed += repair_one(c, idx, a, apply);
		fflush(stdout);
	}

	printf("\n%d of %d repaired and verified\n", fixed, ntodo);
	free(todo);

	if (a->out && fixed) {
		if (dr_export(c, a->out, a->format) < 0) {
			fprintf(stderr, "%s\n", dr_last_error(c));
			return 1;
		}
		printf("wrote %s (%s)\n", a->out, a->format);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
static int cmd_formats(void)
{
	HXCFE *h = hxcfe_init();
	HXCFE_IMGLDR *l;
	int i, n;

	if (!h)
		return 1;
	l = hxcfe_imgInitLoader(h);
	if (!l) {
		hxcfe_deinit(h);
		return 1;
	}

	n = hxcfe_imgGetNumberOfLoader(l);
	printf("%-24s %-10s %s\n", "NAME", "EXT", "DESCRIPTION");
	for (i = 0; i < n; i++) {
		if (!(hxcfe_imgGetLoaderAccess(l, i) & 2))
			continue;
		printf("%-24s %-10s %s\n",
		       hxcfe_imgGetLoaderName(l, i),
		       hxcfe_imgGetLoaderExt(l, i),
		       hxcfe_imgGetLoaderDesc(l, i));
	}
	hxcfe_imgDeInitLoader(l);
	hxcfe_deinit(h);
	return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	args a;
	dr_ctx *c;
	int rc = 0, idx;

	if (parse_args(argc, argv, &a) < 0) {
		fputs(usage_text, stderr);
		return 2;
	}

	if (!strcmp(a.cmd, "formats"))
		return cmd_formats();

	if (!strcmp(a.cmd, "help") || !strcmp(a.cmd, "--help")) {
		fputs(usage_text, stdout);
		return 0;
	}

	if (!a.image) {
		fputs(usage_text, stderr);
		return 2;
	}

	c = dr_open_ex(a.image, a.verbose, a.sets, a.nsets);
	if (!c)
		return 1;
	if (!c->floppy) {
		fprintf(stderr, "error: %s\n", dr_last_error(c));
		dr_close(c);
		return 1;
	}

	if (dr_scan(c) < 0) {
		fprintf(stderr, "error: %s\n", dr_last_error(c));
		dr_close(c);
		return 1;
	}

	if (!strcmp(a.cmd, "scan")) {
		if (a.json)
			dr_json_scan(c, stdout), printf("\n");
		else
			print_scan(c);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "plot")) {
		dr_view *pv;
		FILE *f;
		const char *marker = "/*__DR_DATA__*/null";
		const char *split;

		idx = pick_sector(c, &a);
		if (idx < 0) {
			dr_close(c);
			return 1;
		}
		if (!a.out) {
			fprintf(stderr, "plot needs --out FILE.html\n");
			dr_close(c);
			return 2;
		}
		pv = dr_view_open(c, idx, &a.opt);
		if (!pv) {
			fprintf(stderr, "could not build a view for sector %d\n", idx);
			dr_close(c);
			return 1;
		}
		f = fopen(a.out, "wb");
		if (!f) {
			perror(a.out);
			dr_view_free(pv);
			dr_close(c);
			return 1;
		}
		split = strstr(dr_web_plot, marker);
		if (!split) {
			fprintf(stderr, "plot template is missing its data slot\n");
			fclose(f);
			dr_view_free(pv);
			dr_close(c);
			return 1;
		}
		fwrite(dr_web_plot, 1, (size_t)(split - dr_web_plot), f);
		dr_json_view(c, pv, f);
		fputs(split + strlen(marker), f);
		fclose(f);
		printf("wrote %s - sector index %d, track %d side %d id %d\n",
		       a.out, idx, pv->sect.track, pv->sect.side,
		       pv->sect.sector_id);
		dr_view_free(pv);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "convert")) {
		if (!a.out) {
			fprintf(stderr, "convert needs --out\n");
			dr_close(c);
			return 2;
		}
		if (dr_export(c, a.out, a.format) < 0) {
			fprintf(stderr, "%s\n", dr_last_error(c));
			dr_close(c);
			return 1;
		}
		printf("wrote %s (%s)\n", a.out, a.format);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "serve")) {
		rc = dr_serve(c, a.bind, a.port, &a.opt);
		dr_close(c);
		return rc;
	}

	if (!strcmp(a.cmd, "repair") && a.all) {
		rc = cmd_repair_all(c, &a);
		dr_close(c);
		return rc;
	}

	idx = pick_sector(c, &a);
	if (idx < 0) {
		dr_close(c);
		return 1;
	}

	if (!strcmp(a.cmd, "damage")) {
		int bits[DR_MAX_WEIGHT * 4], nb = 0;
		char *dup, *tok;

		if (!a.bits || !a.out) {
			fprintf(stderr, "damage needs --bits and --out\n");
			dr_close(c);
			return 2;
		}
		dup = strdup(a.bits);
		for (tok = strtok(dup, ","); tok && nb < (int)(sizeof(bits)/sizeof(bits[0]));
		     tok = strtok(NULL, ","))
			bits[nb++] = atoi(tok);
		free(dup);

		nb = dr_damage(c, idx, bits, nb, a.droponly);
		if (nb < 0) {
			fprintf(stderr, "damage failed: %s\n", dr_last_error(c));
			dr_close(c);
			return 1;
		}
		if (!nb) {
			fprintf(stderr, "no requested bit could be dropped "
			                "(they all read 0 already)\n");
			dr_close(c);
			return 1;
		}
		if (dr_export(c, a.out, a.format) < 0) {
			fprintf(stderr, "%s\n", dr_last_error(c));
			dr_close(c);
			return 1;
		}
		printf("flipped %d bit(s) in sector index %d; wrote %s\n",
		       nb, idx, a.out);
		dr_close(c);
		return 0;
	}

	{
		dr_view *v = dr_view_open(c, idx, &a.opt);

		if (!v) {
			fprintf(stderr, "could not build a view for sector %d "
			                "(unsupported encoding?)\n", idx);
			dr_close(c);
			return 1;
		}

		if (!strcmp(a.cmd, "inspect")) {
			if (a.json)
				dr_json_view(c, v, stdout), printf("\n");
			else if (a.bytes) {
				int b0 = 0, b1 = 0;
				char *colon;
				char *dup = strdup(a.bytes);

				colon = strchr(dup, ':');
				if (colon) {
					*colon = 0;
					b0 = atoi(dup);
					b1 = atoi(colon + 1);
				} else {
					b0 = b1 = atoi(dup);
				}
				free(dup);
				print_view(v, 0);
				print_bytes(v, b0, b1);
			} else
				print_view(v, 24);
			dr_view_free(v);
			dr_close(c);
			return 0;
		}

		if (!strcmp(a.cmd, "repair")) {
			dr_repair_result r;
			dr_mode engine;

			if (!a.json)
				print_view(v, 12);

			/*
			 * Every engine that applies gets a say, and their
			 * candidates are ranked together on one scale. Taking
			 * whichever engine answered first is how a fifteen-bit
			 * re-reading gets applied while a one-bit dropout
			 * repair sits unexamined in another engine's list.
			 */
			if (a.opt.mode == DR_MODE_AUTO) {
				if (dr_repair_auto(c, v, &a.opt, &r) < 0) {
					fprintf(stderr, "search failed\n");
					dr_view_free(v);
					dr_close(c);
					return 1;
				}
				engine = r.count ? r.list[0].origin : DR_MODE_BITS;
			} else {
				int rs;

				engine = a.opt.mode;
				memset(&r, 0, sizeof(r));
				if (engine == DR_MODE_PATTERN)
					rs = dr_pattern_search(v, &a.opt, &r);
				else if (engine == DR_MODE_REBIN)
					rs = dr_rebin_search(v, &a.opt, &r);
				else
					rs = dr_repair_search(v, &a.opt, &r);
				if (rs < 0) {
					fprintf(stderr, "search failed\n");
					dr_view_free(v);
					dr_close(c);
					return 1;
				}
				dr_rescore_data(c, v, &a.opt, &r);
			}

			if (a.json) {
				dr_json_candidates(v, &r, 0, 0, stdout);
				printf("\n");
			} else if (engine == DR_MODE_PATTERN) {
				print_pattern(v, &r, 16);
			} else if (engine == DR_MODE_REBIN) {
				print_rebin(v, &r, 16);
			} else {
				print_candidates(v, &r, 16);
			}

			/* --auto only fires when the reading is not in doubt:
			 * a lone candidate, or one that beats the runner-up
			 * by two orders of magnitude. */
			if (a.apply < 0 && a.autoapply && r.count >= 1) {
				if (r.count == 1 ||
				    r.list[1].rel_likelihood <= 0.01)
					a.apply = 0;
				else if (!a.json)
					printf("\n--auto declined: the top "
					       "candidate is not a clear "
					       "winner.\n");
			}

			if (a.apply >= 0) {
				if (a.apply >= r.count) {
					fprintf(stderr, "candidate %d does not exist\n",
					        a.apply);
					rc = 1;
				} else if (dr_apply(c, v, &r.list[a.apply]) < 0) {
					fprintf(stderr, "apply failed\n");
					rc = 1;
				} else {
					int ok = dr_verify(c, idx);
					if (!a.json)
						printf("\napplied candidate %d; "
						       "libhxcfe re-decode says the "
						       "sector is now %s\n",
						       a.apply,
						       ok == 1 ? "CLEAN" :
						       ok == 0 ? "still bad" : "gone");
					if (ok != 1)
						rc = 1;
				}
			}

			if (a.out && rc == 0) {
				if (dr_export(c, a.out, a.format) < 0) {
					fprintf(stderr, "%s\n", dr_last_error(c));
					rc = 1;
				} else if (!a.json) {
					printf("wrote %s (%s)\n", a.out, a.format);
				}
			}

			dr_repair_free(&r);
			dr_view_free(v);
			dr_close(c);
			return rc;
		}

		dr_view_free(v);
	}

	fprintf(stderr, "unknown command '%s'\n", a.cmd);
	dr_close(c);
	return 2;
}
