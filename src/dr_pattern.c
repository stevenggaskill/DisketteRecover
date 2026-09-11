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

/* ------------------------------------------------------------------ */
/* Fitting a model to the data field                                   */
/* ------------------------------------------------------------------ */
/*
 * Both models below produce the same thing: a predicted byte for every
 * position it has an opinion about. The repair engine then works on the
 * positions where the prediction and the read disagree.
 *
 * The distinction that matters is between a model that is wrong and one
 * that has no opinion. A model which mistakes a legitimate carry for an
 * error manufactures outliers, and every manufactured outlier is another
 * free byte for the CRC to match by luck. Thirty free bytes against
 * sixteen bits of CRC will always produce a "valid" reading, and it will
 * always be nonsense. So a model has to explain the regular parts of the
 * field exactly, or admit it cannot.
 */
typedef struct {
	uint8_t *pred;
	uint8_t *known;
	int      outliers;
	int      explained;
	dr_fit_kind kind;
	int      period;
	int      rec, phase, big_endian;
	uint64_t step, v0;
	char     desc[160];
} dr_fit;

static void fit_free(dr_fit *f)
{
	free(f->pred);
	free(f->known);
	f->pred = f->known = NULL;
}

static int fit_alloc(dr_fit *f, int n)
{
	memset(f, 0, sizeof(*f));
	f->pred = calloc((size_t)n, 1);
	f->known = calloc((size_t)n, 1);
	return (f->pred && f->known) ? 0 : -1;
}

/*
 * Score a fit by what it would cost to describe the field with it: every
 * byte it gets right is free, every byte it gets wrong costs a byte to
 * correct, and every byte it declines to model costs one too.
 *
 * Ranking by "fewest outliers" alone is a trap - it rewards a model for
 * abstaining. A longer record size that quietly says nothing about the
 * damaged bytes shows no outliers at all and looks like the better fit,
 * right up until there is nothing left to correct.
 */
static long fit_score(const dr_fit *f, int n)
{
	int unknown = n - f->explained - f->outliers;

	return (long)f->explained - 8L * f->outliers - (long)unknown;
}

static void fit_count(dr_fit *f, const uint8_t *d, int n)
{
	int i;

	f->outliers = f->explained = 0;
	for (i = 0; i < n; i++) {
		if (!f->known[i])
			continue;
		if (f->pred[i] == d[i])
			f->explained++;
		else
			f->outliers++;
	}
}

/* A fit is only usable if what it claims to model, it models almost
 * perfectly - 90% is not "mostly right", it is fifty invented outliers
 * in a 512-byte sector. A narrow fit that is trustworthy beats a broad
 * one that is not, so trust is the first comparison and description
 * length only the tie-break. */
static int fit_trusted(const dr_fit *f)
{
	int seen = f->explained + f->outliers;

	return seen > 0 && f->explained * 10 >= seen * 9;
}

/* ---- the repeat a field almost obeys ------------------------------ */
static const int periods[] = {
	1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128, 256
};
#define NPERIODS ((int)(sizeof(periods) / sizeof(periods[0])))

static int fit_periodic(const uint8_t *data, int n, dr_fit *best)
{
	dr_fit cur;
	int pi, i, r, have = 0;

	memset(best, 0, sizeof(*best));

	for (pi = 0; pi < NPERIODS; pi++) {
		int p = periods[pi];
		int modal[MAX_PERIOD];

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

		if (fit_alloc(&cur, n) < 0)
			return -1;
		for (i = 0; i < n; i++) {
			cur.pred[i] = (uint8_t)modal[i % p];
			cur.known[i] = 1;
		}
		cur.kind = DR_FIT_PERIODIC;
		cur.period = p;
		fit_count(&cur, data, n);
		snprintf(cur.desc, sizeof(cur.desc),
		         "period %d repeat", p);

		if (!have || fit_score(&cur, n) > fit_score(best, n)) {
			if (have)
				fit_free(best);
			*best = cur;
			have = 1;
		} else {
			fit_free(&cur);
		}
		if (have && !best->outliers)
			break;
	}
	return have ? 0 : -1;
}

/* ---- a counter: fixed-size records advancing by a fixed step ------ */
static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y) ? -1 : (x > y);
}

static uint64_t mode_u64(uint64_t *v, int n, int *count)
{
	int i, run = 1, bestn = 0;
	uint64_t best = 0;

	if (n <= 0) {
		*count = 0;
		return 0;
	}
	qsort(v, (size_t)n, sizeof(*v), cmp_u64);
	for (i = 1; i <= n; i++) {
		if (i < n && v[i] == v[i - 1]) {
			run++;
			continue;
		}
		if (run > bestn) {
			bestn = run;
			best = v[i - 1];
		}
		run = 1;
	}
	*count = bestn;
	return best;
}

static uint64_t rec_value(const uint8_t *d, int off, int rec, int be)
{
	uint64_t v = 0;
	int k;

	if (be)
		for (k = 0; k < rec; k++)
			v = (v << 8) | d[off + k];
	else
		for (k = rec - 1; k >= 0; k--)
			v = (v << 8) | d[off + k];
	return v;
}

static void rec_store(uint8_t *d, int off, int rec, int be, uint64_t v)
{
	int k;

	if (be)
		for (k = rec - 1; k >= 0; k--) {
			d[off + k] = (uint8_t)(v & 0xFF);
			v >>= 8;
		}
	else
		for (k = 0; k < rec; k++) {
			d[off + k] = (uint8_t)(v & 0xFF);
			v >>= 8;
		}
}

/*
 * Tables of counters are everywhere on a disk - index tables, timing
 * lists, sector maps, directory offsets. Modelling the record as one
 * integer rather than as independent byte columns is what makes carries
 * come out right: a column-wise model sees the carry that ticks a high
 * byte over as an error and "corrects" it, which is exactly how a search
 * talks itself into a thirty-byte answer.
 *
 * Record alignment matters as much as record size. A table of 24-bit
 * counters that starts one byte into the field looks, column by column,
 * like a bizarre permutation; lined up on its real boundary it is plain
 * little-endian with a constant step.
 */
static int fit_counter(const uint8_t *data, int n, dr_fit *best)
{
	dr_fit cur;
	uint64_t *diffs, *v0s, *win;
	int rec, phase, be, have = 0;
	const int W = 8;               /* records voting on the local origin */

	memset(best, 0, sizeof(*best));

	diffs = malloc((size_t)(n + 1) * sizeof(uint64_t));
	v0s   = malloc((size_t)(n + 1) * sizeof(uint64_t));
	win   = malloc((size_t)(2 * W + 2) * sizeof(uint64_t));
	if (!diffs || !v0s || !win) {
		free(diffs); free(v0s); free(win);
		return -1;
	}

	for (rec = 1; rec <= 8; rec++) {
	for (phase = 0; phase < rec; phase++) {
	for (be = 0; be < 2; be++) {
		int nrec = (n - phase) / rec;
		uint64_t mask, step;
		int i, r, nstep, minstep, known = 0;

		if (nrec < 16)
			continue;
		if (rec == 1 && be)
			continue;                /* no byte order to choose */
		mask = (rec >= 8) ? ~(uint64_t)0
		                  : (((uint64_t)1 << (8 * rec)) - 1);

		for (i = 0; i + 1 < nrec; i++)
			diffs[i] = (rec_value(data, phase + (i + 1) * rec, rec, be) -
			            rec_value(data, phase + i * rec, rec, be)) & mask;
		step = mode_u64(diffs, nrec - 1, &nstep);

		/*
		 * A table often fills only part of a sector, so the step need
		 * not carry a majority of the whole field - but it does have
		 * to carry a real run of records, not a coincidence.
		 */
		minstep = nrec / 8;
		if (minstep < 6)
			minstep = 6;
		if (nstep < minstep)
			continue;

		for (r = 0; r < nrec; r++)
			v0s[r] = (rec_value(data, phase + r * rec, rec, be) -
			          (uint64_t)r * step) & mask;

		if (fit_alloc(&cur, n) < 0) {
			free(diffs); free(v0s); free(win);
			return -1;
		}

		/*
		 * Decide the origin locally. Where a run of records agrees on
		 * one origin, the model owns that stretch and any record that
		 * disagrees inside it is damage. Where the neighbourhood
		 * cannot agree - past the end of the table, or in data that
		 * was never a counter - the model says nothing at all, which
		 * is the whole point: a byte it has no opinion about is not a
		 * byte the CRC gets to play with.
		 */
		for (r = 0; r < nrec; r++) {
			int lo = r - W, hi = r + W, nw = 0, sup;
			uint64_t v0;

			if (lo < 0) lo = 0;
			if (hi >= nrec) hi = nrec - 1;
			for (i = lo; i <= hi; i++)
				win[nw++] = v0s[i];
			v0 = mode_u64(win, nw, &sup);
			if (sup * 5 < nw * 3)          /* under 60% agreement */
				continue;

			rec_store(cur.pred, phase + r * rec, rec, be,
			          (v0 + (uint64_t)r * step) & mask);
			memset(cur.known + phase + r * rec, 1, (size_t)rec);
			known += rec;
		}

		/* A model that only recognises a corner of the field is not
		 * worth preferring over one that covers it. */
		if (known * 8 < n) {
			fit_free(&cur);
			continue;
		}

		cur.kind = DR_FIT_COUNTER;
		cur.rec = rec;
		cur.phase = phase;
		cur.big_endian = be;
		cur.step = step;
		cur.v0 = v0s[0];
		fit_count(&cur, data, n);
		snprintf(cur.desc, sizeof(cur.desc),
		         "%d-byte %s records at offset %d counting by 0x%llX, "
		         "over %d of %d bytes",
		         rec, be ? "big-endian" : "little-endian", phase,
		         (unsigned long long)step, known, n);

		if (!have || (fit_trusted(&cur) && !fit_trusted(best)) ||
		    (fit_trusted(&cur) == fit_trusted(best) &&
		     (fit_score(&cur, n) > fit_score(best, n) ||
		      (fit_score(&cur, n) == fit_score(best, n) &&
		       cur.rec < best->rec)))) {
			if (have)
				fit_free(best);
			*best = cur;
			have = 1;
		} else {
			fit_free(&cur);
		}
	}}}

	free(diffs);
	free(v0s);
	free(win);
	return have ? 0 : -1;
}

/* Pick whichever model describes the field best, trusted fits first. */
static int fit_best(const uint8_t *data, int n, dr_fit *out)
{
	dr_fit a, b;
	int ha, hb;

	ha = (fit_periodic(data, n, &a) == 0);
	hb = (fit_counter(data, n, &b) == 0);

	if (ha && hb) {
		int ta = fit_trusted(&a), tb = fit_trusted(&b);

		if (ta != tb) {
			if (tb) { fit_free(&a); *out = b; }
			else    { fit_free(&b); *out = a; }
			return 0;
		}
		if (fit_score(&b, n) > fit_score(&a, n)) {
			fit_free(&a);
			*out = b;
		} else {
			fit_free(&b);
			*out = a;
		}
		return 0;
	}
	if (ha) { *out = a; return 0; }
	if (hb) { *out = b; return 0; }
	return -1;
}

int dr_pattern_analyse(const dr_view *v, dr_pattern_info *info)
{
	const uint8_t *data;
	dr_fit f;
	int n, i, hist[256];

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

	if (fit_best(data, n, &f) < 0) {
		info->kind = DR_FIT_NONE;
		snprintf(info->desc, sizeof(info->desc), "no usable regularity");
		return 0;
	}

	info->kind = f.kind;
	info->period = f.period;
	info->rec = f.rec;
	info->phase = f.phase;
	info->big_endian = f.big_endian;
	info->step = f.step;
	info->v0 = f.v0;
	info->outliers = f.outliers;
	info->explained = f.explained;
	info->coverage = (f.explained + f.outliers)
	        ? (double)f.explained / (double)(f.explained + f.outliers) : 0.0;
	snprintf(info->desc, sizeof(info->desc), "%s", f.desc);
	fit_free(&f);
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
	dr_fit f;
	struct dmodel m;
	uint16_t *masks = NULL;
	int *outpos = NULL;
	uint8_t *fixed = NULL;
	dr_candidate *found = NULL;
	int nfound = 0, fcap = 0;
	int n, i, j, leave, nout = 0, rc = 0, have_fit = 0, have_model = 0;
	long explored = 0;
	double coverage;
	const uint8_t *data;

	memset(out, 0, sizeof(*out));
	out->pattern = 1;

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}
	if (!v || !v->msg || v->data_len <= 0)
		return -1;

	data = v->msg + v->data_offset;
	n = v->data_len;

	if (fit_best(data, n, &f) < 0) {
		snprintf(out->note, sizeof(out->note),
		         "no usable regularity in this data - nothing for "
		         "Occam to work with");
		return 0;
	}
	have_fit = 1;

	coverage = (f.explained + f.outliers)
	        ? (double)f.explained / (double)(f.explained + f.outliers) : 0.0;
	out->period = f.kind == DR_FIT_PERIODIC ? f.period : f.rec;
	out->outliers = f.outliers;
	out->coverage = coverage;

	if (!f.outliers) {
		if (f.explained >= n)
			snprintf(out->note, sizeof(out->note),
			         "%s explains the field exactly - the CRC error "
			         "is not in the data", f.desc);
		else
			snprintf(out->note, sizeof(out->note),
			         "%s, and the part it covers is intact - the "
			         "damage is in the %d byte(s) it cannot model",
			         f.desc, n - f.explained);
		goto done;
	}
	/*
	 * A model that only half fits is worse than none: its disagreements
	 * are its own failures, not the disk's, and every one of them is a
	 * byte the CRC can then match by luck.
	 */
	if (coverage < 0.90) {
		snprintf(out->note, sizeof(out->note),
		         "best fit (%s) explains only %.0f%% of the field - "
		         "too loose to trust", f.desc, coverage * 100.0);
		goto done;
	}
	if (f.outliers > opt->max_outliers) {
		snprintf(out->note, sizeof(out->note),
		         "%s leaves %d byte(s) unexplained - too many to "
		         "enumerate (raise --max-outliers)", f.desc, f.outliers);
		goto done;
	}

	masks = malloc((size_t)v->msg_bits * sizeof(uint16_t));
	outpos = malloc((size_t)f.outliers * sizeof(int));
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
	have_model = 1;

	for (i = 0; i < n; i++)
		if (f.known[i] && f.pred[i] != data[i])
			outpos[nout++] = i;

	/* How much is genuinely in doubt, against the 16 bits the CRC has? */
	for (i = 0; i < nout; i++) {
		uint8_t diff = (uint8_t)(data[outpos[i]] ^ f.pred[outpos[i]]);
		for (j = 0; j < 8; j++)
			if (diff & (0x80 >> j))
				out->uncertain_bits++;
	}

	/*
	 * Enumerate by how many outliers we decline to correct. Restoring
	 * the whole pattern is the simplest explanation, so it is tried
	 * first; each byte left broken makes the reading less likely.
	 */
	for (leave = 0; leave <= nout && !nfound; leave++) {
		unsigned long combo, limit;

		if (nout > 20)
			break;
		limit = 1UL << nout;

		for (combo = 0; combo < limit; combo++) {
			uint16_t syn = v->syndrome;
			int bits[DR_MAX_WEIGHT], nb = 0, ok = 1;

			if (__builtin_popcountl(combo) != leave)
				continue;
			explored++;

			memcpy(fixed, data, (size_t)n);
			for (i = 0; i < nout && ok; i++) {
				int pos = outpos[i];
				uint8_t want, diff;

				if (combo & (1UL << i))
					continue;          /* left broken */
				want = f.pred[pos];
				fixed[pos] = want;
				diff = (uint8_t)(want ^ data[pos]);
				for (j = 0; j < 8; j++) {
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
		         "%s - explains %.1f%% of the field", f.desc,
		         coverage * 100.0);

	out->list = found;
	out->count = nfound;
	found = NULL;

done:
	if (have_model)
		dmodel_free(&m);
	if (have_fit)
		fit_free(&f);
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
