/*
 * DisketteRecover - Occam's razor for sector data.
 *
 * Sector data is very often not random. A freshly formatted MS-DOS disk
 * is 512 bytes of 0xF6; an unused directory block is 0x00 or 0xE5; a
 * padded file tail repeats; a bitmap alternates. So when 506 of a
 * sector's 512 bytes read 0xF6 and the other six read 0x76 or 0xF2 -
 * each of which is 0xF6 with a single bit missing - the overwhelmingly
 * likely truth is that the sector is all 0xF6 and six transitions were
 * dropped. No flux timing is needed to see that, and no amount of flux
 * timing is as decisive.
 *
 * This engine finds the repeat the field almost obeys, lists the bytes
 * that break it, and asks the CRC whether restoring them is right. It
 * enumerates by how much of the pattern it has to leave broken -
 * restoring everything first, then leaving one byte out, then two -
 * because the fewer exceptions a reading needs, the likelier it is.
 *
 * The same data model then re-scores whatever the other engines find, so
 * a candidate that turns a run of 0xF6 into noise is ranked where it
 * belongs even when its CRC is perfectly valid.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dr_internal.h"

#define MAX_PERIOD  256
#define ALPHA       0.08     /* add-alpha smoothing for the byte model */

struct dr_model {
	uint16_t *c2;        /* 256^3 counts, saturating                  */
	uint32_t *t2;        /* 256^2 totals                              */
	uint32_t *c1;        /* 256^2                                     */
	uint32_t *t1;        /* 256                                       */
	uint32_t  c0[256];
	double    t0;
	long      samples;
	int       have2;
};

static const int periods[] = {
	1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128, 256
};
#define NPERIODS ((int)(sizeof(periods) / sizeof(periods[0])))

/* ------------------------------------------------------------------ */
/* Find the repeat the data field almost obeys                         */
/* ------------------------------------------------------------------ */
int dr_pattern_analyse(const dr_view *v, dr_pattern_info *info)
{
	const uint8_t *data;
	int n, pi, i, best_p = 1, best_out = -1;
	int hist[256];

	memset(info, 0, sizeof(*info));
	if (!v || !v->msg || v->data_len <= 0)
		return -1;

	data = v->msg + v->data_offset;
	n = v->data_len;

	memset(hist, 0, sizeof(hist));
	for (i = 0; i < n; i++)
		hist[data[i]]++;
	for (i = 0; i < 256; i++) {
		if (!hist[i])
			continue;
		info->distinct++;
		if (hist[i] > info->top_count) {
			info->top_count = hist[i];
			info->top_value = i;
		}
	}

	/* Shortest period that explains the most bytes. Longer periods can
	 * only fit better, so they have to beat the incumbent outright. */
	for (pi = 0; pi < NPERIODS; pi++) {
		int p = periods[pi];
		int modal[MAX_PERIOD], out = 0, r;

		if (p > n / 4)
			break;

		for (r = 0; r < p; r++) {
			int c[256], j, bestv = 0, bestc = -1;

			memset(c, 0, sizeof(c));
			for (j = r; j < n; j += p)
				c[data[j]]++;
			for (j = 0; j < 256; j++)
				if (c[j] > bestc) {
					bestc = c[j];
					bestv = j;
				}
			modal[r] = bestv;
		}

		for (i = 0; i < n; i++)
			if (data[i] != modal[i % p])
				out++;

		if (best_out < 0 || out < best_out) {
			best_out = out;
			best_p = p;
			memcpy(info->modal, modal, (size_t)p * sizeof(int));
		}
		if (!out)
			break;
	}

	info->period = best_p;
	info->outliers = best_out < 0 ? n : best_out;
	info->coverage = 1.0 - (double)info->outliers / (double)n;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Order-1 byte model built from the field's own statistics            */
/* ------------------------------------------------------------------ */
struct dmodel {
	int   *ctx;          /* 256*256 counts                            */
	int    tot[256];
	int    n;
};

static void dmodel_free(struct dmodel *m)
{
	free(m->ctx);
	m->ctx = NULL;
}

static int dmodel_build(struct dmodel *m, const uint8_t *data, int n)
{
	int i, prev = 0;

	memset(m, 0, sizeof(*m));
	m->ctx = calloc(256 * 256, sizeof(int));
	if (!m->ctx)
		return -1;

	for (i = 0; i < n; i++) {
		m->ctx[prev * 256 + data[i]]++;
		m->tot[prev]++;
		prev = data[i];
	}
	m->n = n;
	return 0;
}

static double dmodel_logp(const struct dmodel *m, int prev, int cur)
{
	double num = (double)m->ctx[prev * 256 + cur] + ALPHA;
	double den = (double)m->tot[prev] + 256.0 * ALPHA;

	return log(num / den);
}

/* log P(candidate data) - log P(current data), evaluated only where the
 * two differ (and at the byte after each difference, whose context
 * moved). */
static double dmodel_delta(const struct dmodel *m, const uint8_t *cur,
                           const uint8_t *cand, int n)
{
	double d = 0.0;
	int i;

	for (i = 0; i < n; i++) {
		int pc, pn;

		if (cur[i] == cand[i] && (i == 0 || cur[i - 1] == cand[i - 1]))
			continue;
		pc = i ? cur[i - 1] : 0;
		pn = i ? cand[i - 1] : 0;
		d += dmodel_logp(m, pn, cand[i]) - dmodel_logp(m, pc, cur[i]);
	}
	return d;
}

/* ------------------------------------------------------------------ */
/* Shared re-scoring hook used by every engine                         */
/* ------------------------------------------------------------------ */
int dr_rescore_data(dr_ctx *c, dr_view *v, const dr_options *opt,
                    dr_repair_result *r)
{
	struct dmodel local;
	const uint8_t *cur;
	uint8_t *base = NULL;
	double base_score = 0.0;
	int i, k, use_disk = 0;

	if (!v || !r || !r->count || v->data_len <= 0)
		return 0;

	cur = v->msg + v->data_offset;

	/* Prefer a model of the whole disk; fall back to this sector's own
	 * statistics when there is nothing else to learn from. */
	if (c) {
		if (!c->model)
			c->model = dr_model_build(c);
		if (c->model && c->model->samples >= 65536)
			use_disk = 1;
	}

	if (use_disk) {
		base_score = dr_model_score(c->model, cur, v->data_len);
	} else if (dmodel_build(&local, cur, v->data_len) < 0) {
		return -1;
	}

	base = malloc((size_t)v->data_len);
	if (!base) {
		if (!use_disk)
			dmodel_free(&local);
		return -1;
	}

	for (i = 0; i < r->count; i++) {
		dr_candidate *cd = &r->list[i];
		uint8_t *msg = dr_candidate_message(v, cd);

		if (!msg)
			continue;

		if (use_disk)
			cd->data_prior = dr_model_score(c->model,
			        msg + v->data_offset, v->data_len) - base_score;
		else
			cd->data_prior = dmodel_delta(&local, cur,
			        msg + v->data_offset, v->data_len);

		cd->restores = cd->removes = 0;
		for (k = 0; k < cd->weight; k++) {
			if (cd->before[k])
				cd->removes++;     /* reading says 1, we say 0 */
			else
				cd->restores++;    /* reading says 0, we say 1 */
		}

		/* Media loses transitions far more readily than it invents
		 * them - a weak pulse falls under the detector's threshold,
		 * whereas noise has to clear it. So putting a 1 back is the
		 * commoner repair. Mild, and only a tie-breaker. */
		cd->log_likelihood += cd->data_prior
		        + opt->dropout_bias * (cd->restores - cd->removes);
		free(msg);
	}

	free(base);
	if (!use_disk)
		dmodel_free(&local);

	/* re-sort and re-normalise */
	for (i = 1; i < r->count; i++) {
		dr_candidate key = r->list[i];
		k = i - 1;
		while (k >= 0 && r->list[k].log_likelihood < key.log_likelihood) {
			r->list[k + 1] = r->list[k];
			k--;
		}
		r->list[k + 1] = key;
	}
	{
		double best = r->list[0].log_likelihood;
		for (i = 0; i < r->count; i++)
			r->list[i].rel_likelihood =
			        exp(r->list[i].log_likelihood - best);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* The pattern engine                                                  */
/* ------------------------------------------------------------------ */
struct pcand {
	int      leave;        /* outliers left uncorrected               */
	uint16_t mask;         /* which ones, as a bitmask over outliers  */
};

int dr_pattern_search(dr_view *v, const dr_options *opt, dr_repair_result *out)
{
	dr_options defopt;
	dr_pattern_info info;
	struct dmodel m;
	uint16_t *masks = NULL;
	int *outpos = NULL;
	uint8_t *fixed = NULL;
	dr_candidate *found = NULL;
	int nfound = 0, fcap = 0;
	int n, i, j, leave, rc = 0;
	long explored = 0;
	const uint8_t *data;

	memset(out, 0, sizeof(*out));
	out->pattern = 1;

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}
	if (!v || !v->msg || v->data_len <= 0)
		return -1;

	if (dr_pattern_analyse(v, &info) < 0)
		return -1;

	out->period = info.period;
	out->outliers = info.outliers;
	out->coverage = info.coverage;

	data = v->msg + v->data_offset;
	n = v->data_len;

	if (!info.outliers) {
		snprintf(out->note, sizeof(out->note),
		         "the data field already repeats exactly - the CRC "
		         "error is not in the data");
		return 0;
	}
	if (info.coverage < 0.5) {
		snprintf(out->note, sizeof(out->note),
		         "no usable repeat in this data (best period %d "
		         "explains only %.0f%% of it) - nothing for Occam to "
		         "work with", info.period, info.coverage * 100.0);
		return 0;
	}
	if (info.outliers > opt->max_outliers) {
		snprintf(out->note, sizeof(out->note),
		         "period %d leaves %d bytes off-pattern - too many to "
		         "enumerate (raise --max-outliers)",
		         info.period, info.outliers);
		return 0;
	}

	masks = malloc((size_t)v->msg_bits * sizeof(uint16_t));
	outpos = malloc((size_t)info.outliers * sizeof(int));
	fixed = malloc((size_t)n);
	if (!masks || !outpos || !fixed) {
		rc = -1;
		goto done;
	}
	dr_crc16_bit_masks(v->msg_bits, masks);
	if (dmodel_build(&m, data, n) < 0) {
		rc = -1;
		goto done;
	}

	j = 0;
	for (i = 0; i < n; i++)
		if (data[i] != info.modal[i % info.period])
			outpos[j++] = i;

	/*
	 * Enumerate by how many outliers we decline to correct. Restoring
	 * the whole pattern is the simplest explanation, so it is tried
	 * first; each byte we have to leave broken makes the reading less
	 * likely, so those come later.
	 */
	for (leave = 0; leave <= info.outliers && !nfound; leave++) {
		unsigned long combo, limit;

		if (info.outliers > 20)
			break;
		limit = 1UL << info.outliers;

		for (combo = 0; combo < limit; combo++) {
			uint16_t syn = v->syndrome;
			int bits[DR_MAX_WEIGHT], nb = 0, ok = 1;

			if (__builtin_popcountl(combo) != leave)
				continue;
			explored++;

			memcpy(fixed, data, (size_t)n);
			for (i = 0; i < info.outliers; i++) {
				int pos = outpos[i];
				uint8_t want, diff;

				if (combo & (1UL << i))
					continue;          /* left broken */
				want = (uint8_t)info.modal[pos % info.period];
				fixed[pos] = want;
				diff = (uint8_t)(want ^ data[pos]);
				for (j = 0; j < 8 && ok; j++) {
					int bit;
					if (!(diff & (0x80 >> j)))
						continue;
					if (nb >= DR_MAX_WEIGHT) { ok = 0; break; }
					bit = (v->data_offset + pos) * 8 + j;
					bits[nb++] = bit;
					syn ^= masks[bit];
				}
			}

			if (!ok || !nb || syn)
				continue;

			if (nfound == fcap) {
				int nc = fcap ? fcap * 2 : 16;
				dr_candidate *nl = realloc(found,
				        (size_t)nc * sizeof(*nl));
				if (!nl) { rc = -1; goto done; }
				found = nl;
				fcap = nc;
			}
			{
				dr_candidate *cd = &found[nfound++];

				memset(cd, 0, sizeof(*cd));
				cd->weight = nb;
				for (j = 0; j < nb; j++) {
					cd->bits[j] = bits[j];
					cd->before[j] = (uint8_t)
					  ((v->msg[bits[j] >> 3] >>
					    (7 - (bits[j] & 7))) & 1);
				}
				cd->data_prior = dmodel_delta(&m, data, fixed, n);
				cd->log_likelihood = cd->data_prior;
			}
		}
	}

	out->explored = explored;
	dmodel_free(&m);

	if (nfound > 0) {
		double best;

		for (i = 1; i < nfound; i++) {
			dr_candidate key = found[i];
			j = i - 1;
			while (j >= 0 &&
			       found[j].log_likelihood < key.log_likelihood) {
				found[j + 1] = found[j];
				j--;
			}
			found[j + 1] = key;
		}
		best = found[0].log_likelihood;
		for (i = 0; i < nfound; i++)
			found[i].rel_likelihood =
			        exp(found[i].log_likelihood - best);
	}

	if (!out->note[0])
		snprintf(out->note, sizeof(out->note),
		         "period %d explains %.1f%% of the field "
		         "(%d distinct value(s), commonest 0x%02X x%d)",
		         info.period, info.coverage * 100.0, info.distinct,
		         info.top_value, info.top_count);

	out->list = found;
	out->count = nfound;
	found = NULL;

done:
	free(found);
	free(fixed);
	free(outpos);
	free(masks);
	return rc;
}

/* ================================================================== */
/* A byte model of the whole disk                                      */
/* ================================================================== */
/*
 * Occam's razor needs to know what "ordinary" looks like, and one
 * sector is not enough to learn it: 512 samples against 65536 order-1
 * contexts leaves almost everything on the smoothing floor, which is
 * why a text sector's candidates all scored within a factor of two of
 * each other.
 *
 * A whole disk is 1.4 MB, which supports a genuine order-2 model. That
 * is the difference between "these bytes are unusual" and "these bytes
 * are not English", and on a document disk it is decisive.
 */

#define BACK2 6.0
#define BACK1 8.0
#define BACK0 12.0

static void model_add(dr_model *m, const uint8_t *d, int n)
{
	int i, p1 = 0, p2 = 0;

	for (i = 0; i < n; i++) {
		int c = d[i];

		if (m->have2) {
			size_t k = ((size_t)p2 << 16) | ((size_t)p1 << 8) |
			           (size_t)c;
			if (m->c2[k] < 0xFFFF)
				m->c2[k]++;
			m->t2[((size_t)p2 << 8) | (size_t)p1]++;
		}
		m->c1[((size_t)p1 << 8) | (size_t)c]++;
		m->t1[p1]++;
		m->c0[c]++;
		m->t0 += 1.0;
		p2 = p1;
		p1 = c;
	}
	m->samples += n;
}

dr_model *dr_model_build(dr_ctx *c)
{
	dr_model *m;
	HXCFE_SECTORACCESS *sa;
	int t, s, nt, ns;

	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;

	m->c1 = calloc(256 * 256, sizeof(uint32_t));
	m->t1 = calloc(256, sizeof(uint32_t));
	if (!m->c1 || !m->t1) {
		dr_model_free(m);
		return NULL;
	}

	/* 32 MB for the order-2 table; skip it rather than fail. */
	m->c2 = calloc((size_t)256 * 256 * 256, sizeof(uint16_t));
	m->t2 = calloc((size_t)256 * 256, sizeof(uint32_t));
	m->have2 = (m->c2 && m->t2);

	sa = c->sacc;
	nt = hxcfe_getNumberOfTrack(c->hxcfe, c->floppy);
	ns = hxcfe_getNumberOfSide(c->hxcfe, c->floppy);

	for (t = 0; t < nt; t++) {
		for (s = 0; s < ns; s++) {
			HXCFE_SECTCFG **list;
			int32_t n = 0;
			int i;

			list = hxcfe_getAllTrackSectors(sa, t, s,
			                                ISOIBM_MFM_ENCODING, &n);
			if (!list)
				continue;
			for (i = 0; i < n; i++) {
				HXCFE_SECTCFG *sc = list[i];

				/* Learn only from sectors that read cleanly -
				 * a damaged one would teach the model its own
				 * corruption. */
				if (sc && sc->input_data && !sc->use_alternate_data_crc &&
				    !sc->use_alternate_header_crc &&
				    sc->sectorsize > 0)
					model_add(m, sc->input_data, sc->sectorsize);
				hxcfe_freeSectorConfig(sa, sc);
			}
			free(list);
		}
	}

	hxcfe_clearTrackCache(sa);
	hxcfe_resetSearchTrackPosition(sa);
	return m;
}

void dr_model_free(dr_model *m)
{
	if (!m)
		return;
	free(m->c2);
	free(m->t2);
	free(m->c1);
	free(m->t1);
	free(m);
}

double dr_model_logp(const dr_model *m, int p2, int p1, int cur)
{
	double p0, p1v, p2v;

	if (!m || m->t0 < 256.0)
		return log(1.0 / 256.0);

	p0 = ((double)m->c0[cur] + BACK0 / 256.0) / (m->t0 + BACK0);

	{
		double n = (double)m->c1[((size_t)p1 << 8) | (size_t)cur];
		double t = (double)m->t1[p1];
		p1v = (n + BACK1 * p0) / (t + BACK1);
	}

	if (!m->have2)
		return log(p1v);

	{
		double n = (double)m->c2[((size_t)p2 << 16) |
		                         ((size_t)p1 << 8) | (size_t)cur];
		double t = (double)m->t2[((size_t)p2 << 8) | (size_t)p1];
		p2v = (n + BACK2 * p1v) / (t + BACK2);
	}
	return log(p2v);
}

double dr_model_score(const dr_model *m, const uint8_t *d, int n)
{
	double s = 0.0;
	int i, p1 = 0, p2 = 0;

	for (i = 0; i < n; i++) {
		s += dr_model_logp(m, p2, p1, d[i]);
		p2 = p1;
		p1 = d[i];
	}
	return s;
}
