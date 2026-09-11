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

#define DBG(...) do { if (getenv("DR_DEBUG")) \
	fprintf(stderr, "dr_pattern: " __VA_ARGS__); } while (0)

#define MAX_PERIOD  256
#define ALPHA       0.08     /* add-alpha smoothing for the byte model */
#define BYTE_NATS   5.545    /* ln(256): the surprise of one wrong byte */

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
		 * A counter that does not count is a repeat, and fit_periodic
		 * already describes repeats - better, because it commits to
		 * every byte instead of abstaining wherever the records
		 * disagree. Letting the degenerate parameterisation compete
		 * hands the field to whichever model declines to look at the
		 * damage, which is exactly backwards.
		 */
		if (!step)
			continue;

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
/* ---- the fit, cached on the view ---------------------------------- */
void *dr_fit_get(dr_view *v)
{
	dr_fit *f;

	if (!v || !v->msg || v->data_len <= 0)
		return NULL;
	if (v->fit)
		return v->fit;

	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	if (fit_best(v->msg + v->data_offset, v->data_len, f) < 0 ||
	    !fit_trusted(f)) {
		fit_free(f);
		free(f);
		return NULL;
	}
	v->fit = f;
	return f;
}

int dr_fit_outliers(void *fitp, const uint8_t *data, int n)
{
	dr_fit *f = fitp;
	int i, out = 0;

	if (!f)
		return -1;
	for (i = 0; i < n; i++)
		if (f->known[i] && f->pred[i] != data[i])
			out++;
	return out;
}

void dr_fit_release(void *fitp)
{
	dr_fit *f = fitp;

	if (!f)
		return;
	fit_free(f);
	free(f);
}

/*
 * Put every engine's candidates on one scale.
 *
 * The engines reason in different currencies - a re-binning scores flux
 * timings, a bit-flip search scores per-bit confidence, the data model
 * scores bytes - and their raw numbers are not comparable. Ranked only
 * within an engine, a fifteen-bit re-reading that rewrites nine bytes
 * can be declared a certainty while a one-bit dropout repair sits
 * unexamined in another engine's list.
 *
 * So the score is recomputed here from scratch, out of terms that mean
 * the same thing wherever the candidate came from:
 *
 *   data      how much likelier the resulting bytes are under a model
 *             of this disk's data
 *   flux      the per-bit confidence the timings assign to each bit the
 *             candidate moves
 *   physics   restoring a dropped reversal beats inventing one
 *
 * A reading that moves fifteen bits pays the flux term fifteen times.
 */
/* The candidate's flips in ascending bit order, as indices into its own
 * arrays - the burst prior needs the distance to the previous one. */
static void order_bits(const dr_candidate *cd, int *ord)
{
	int i, j;

	for (i = 0; i < cd->weight; i++)
		ord[i] = i;
	for (i = 1; i < cd->weight; i++) {
		int key = ord[i];

		for (j = i - 1; j >= 0 && cd->bits[ord[j]] > cd->bits[key]; j--)
			ord[j + 1] = ord[j];
		ord[j + 1] = key;
	}
}

int dr_rescore_data(dr_ctx *c, dr_view *v, const dr_options *opt,
                    dr_repair_result *r)
{
	struct dmodel local;
	const uint8_t *cur;
	double base_score = 0.0;
	void *fit;
	int ord[DR_MAX_WEIGHT];
	int i, k, use_disk = 0, before = 0;

	if (!v || !r || !r->count || v->data_len <= 0)
		return 0;

	cur = v->msg + v->data_offset;
	fit = dr_fit_get(v);
	if (fit)
		before = dr_fit_outliers(fit, cur, v->data_len);

	/* Prefer a model of the whole disk; fall back to this sector's own
	 * statistics when there is nothing else to learn from. */
	if (c) {
		if (!c->model)
			c->model = dr_model_build(c);
		if (c->model && c->model->samples >= 65536)
			use_disk = 1;
	}

	if (!fit) {
		if (use_disk)
			base_score = dr_model_score(c->model, cur, v->data_len);
		else if (dmodel_build(&local, cur, v->data_len) < 0)
			return -1;
	}

	for (i = 0; i < r->count; i++) {
		dr_candidate *cd = &r->list[i];
		uint8_t *msg = dr_candidate_message(v, cd);
		double flux = 0.0;

		if (!msg)
			continue;

		/*
		 * Prefer the strongest data model available. A byte n-gram
		 * cannot represent a counter - ask it whether restoring a
		 * table of incrementing records is an improvement and it will
		 * say no, because it never learned the increment. Where a
		 * structural fit holds, the honest measure of a reading is
		 * how many bytes it leaves off-pattern.
		 */
		if (fit) {
			int after = dr_fit_outliers(fit, msg + v->data_offset,
			                            v->data_len);
			cd->data_prior = (double)(before - after) * BYTE_NATS;
		} else if (use_disk) {
			cd->data_prior = dr_model_score(c->model,
			        msg + v->data_offset, v->data_len) - base_score;
		} else {
			cd->data_prior = dmodel_delta(&local, cur,
			        msg + v->data_offset, v->data_len);
		}

		cd->restores = cd->removes = 0;
		order_bits(cd, ord);
		for (k = 0; k < cd->weight; k++) {
			double q;
			int b = cd->bits[ord[k]];

			if (cd->before[ord[k]])
				cd->removes++;     /* reading says 1, we say 0 */
			else
				cd->restores++;    /* reading says 0, we say 1 */

			/* What the timings think of moving this particular
			 * bit. A confident cell is expensive to overrule. */
			q = (b >= 0 && b < v->msg_bits) ? v->bit_perr[b] : 1e-4;

			/*
			 * ...and what the bit before it says. Errors on this
			 * medium are not memoryless: every reading we have
			 * been able to confirm puts its errors in clumps.
			 * Over the confirmed fixes on both disks, 21 of the
			 * 22 gaps between consecutive errors are under 160
			 * bits and half are under 16, where an independent
			 * error rate of 2e-4 per bit would put the typical
			 * gap in the thousands. The ground-truth sectors on
			 * their own show the same thing, so it is not an
			 * artefact of the engine that found them.
			 *
			 * That is physics, not coincidence - one weak spot
			 * in the oxide, one off-track excursion, one speed
			 * wobble takes out a neighbourhood of reversals, not
			 * a bit. So a flip standing next to another flip is
			 * charged as the continuation of one event rather
			 * than as a second independent miracle.
			 */
			if (k > 0 && opt->burst_gain > 0.0 &&
			    opt->burst_len > 0.0) {
				double d = (double)(b - cd->bits[ord[k - 1]]);
				double lift = 1.0 + opt->burst_gain *
				        exp(-d / opt->burst_len);
				q *= lift;
			}
			if (q <= 0.0) q = 1e-9;
			if (q >= 0.5) q = 0.499999;
			flux += log(q) - log(1.0 - q);
		}

		/* Media loses transitions far more readily than it invents
		 * them - a weak pulse falls under the detector's threshold,
		 * whereas noise has to clear it. */
		cd->log_likelihood = cd->data_prior + flux
		        + opt->dropout_bias * (cd->restores - cd->removes);
		free(msg);
	}

	if (!fit && !use_disk)
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

/* Are two candidates the same set of bit flips? */
static int same_flips(const dr_candidate *a, const dr_candidate *b)
{
	int i;

	if (a->weight != b->weight)
		return 0;
	for (i = 0; i < a->weight; i++)
		if (a->bits[i] != b->bits[i])
			return 0;
	return 1;
}

int dr_repair_auto(dr_ctx *c, dr_view *v, const dr_options *opt,
                   dr_repair_result *out)
{
	dr_options defopt;
	dr_repair_result part[4];
	dr_mode modes[4] = { DR_MODE_PATTERN, DR_MODE_REVS, DR_MODE_REBIN,
	                     DR_MODE_BITS };
	dr_candidate *all = NULL;
	int nall = 0, i, j, m;

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}
	memset(out, 0, sizeof(*out));
	memset(part, 0, sizeof(part));

	for (m = 0; m < 4; m++) {
		if (modes[m] == DR_MODE_PATTERN)
			dr_pattern_search(v, opt, &part[m]);
		else if (modes[m] == DR_MODE_REVS)
			dr_revs_search(v, opt, &part[m]);
		else if (modes[m] == DR_MODE_REBIN)
			dr_rebin_search(v, opt, &part[m]);
		else
			dr_repair_search(v, opt, &part[m]);

		for (i = 0; i < part[m].count; i++)
			part[m].list[i].origin = modes[m];

		/* Keep the running commentary of whichever engine had most
		 * to say about the damage. */
		if (part[m].note[0] && !out->note[0])
			snprintf(out->note, sizeof(out->note), "%s",
			         part[m].note);
		if (part[m].ambiguous > out->ambiguous)
			out->ambiguous = part[m].ambiguous;
		if (part[m].uncertain_bits > out->uncertain_bits)
			out->uncertain_bits = part[m].uncertain_bits;
		out->explored += part[m].explored;
		out->truncated |= part[m].truncated;
		if (part[m].floor_cost > out->floor_cost) {
			out->floor_cost = part[m].floor_cost;
			out->current_cost = part[m].current_cost;
		}
		if (modes[m] == DR_MODE_PATTERN) {
			out->period = part[m].period;
			out->outliers = part[m].outliers;
			out->coverage = part[m].coverage;
			out->slip = part[m].slip;
			out->slip_byte = part[m].slip_byte;
			out->crc_fixed = part[m].crc_fixed;
		}
		if (modes[m] == DR_MODE_BITS)
			out->npool = part[m].npool;
		if (modes[m] == DR_MODE_REVS) {
			out->contested = part[m].contested;
			out->majority = part[m].majority;
			out->crc_contested = part[m].crc_contested;
		}
		out->searched_weight += part[m].searched_weight;
		nall += part[m].count;
	}

	if (nall) {
		all = malloc((size_t)nall * sizeof(*all));
		if (!all) {
			for (m = 0; m < 4; m++)
				dr_repair_free(&part[m]);
			return -1;
		}
		nall = 0;
		for (m = 0; m < 4; m++)
			for (i = 0; i < part[m].count; i++) {
				int dup = 0;
				for (j = 0; j < nall && !dup; j++)
					dup = same_flips(&all[j],
					                 &part[m].list[i]);
				if (!dup)
					all[nall++] = part[m].list[i];
			}
	}
	for (m = 0; m < 4; m++)
		dr_repair_free(&part[m]);

	out->list = all;
	out->count = nall;
	if (nall > opt->max_results) {
		out->count = opt->max_results;
		out->truncated = 1;
	}

	dr_rescore_data(c, v, opt, out);
	return 0;
}

/* ------------------------------------------------------------------ */
/* The pattern engine                                                  */
/* ------------------------------------------------------------------ */
struct pcand {
	int      leave;        /* outliers left uncorrected               */
	uint16_t mask;         /* which ones, as a bitmask over outliers  */
};

/*
 * Look for a cell-phase correction that brings a re-framed tail back
 * into line with the model.
 *
 * Scored against the prediction rather than against a re-fit, because a
 * re-fit per trial costs a hundred times more and the prediction already
 * extrapolates across the whole field - that is what a model is for.
 * Only positions inside the span the model cannot explain are worth
 * trying: a slip outside it would have broken the part that fits.
 *
 * Returns the number of bytes the best correction recovers, 0 if none is
 * worth having.
 */
#define SLIP_MAX 6           /* cells a decoder can plausibly lose      */

static int agree(const uint8_t *msg, const dr_fit *f, int off, int n)
{
	int i, k = 0;

	for (i = 0; i < n; i++)
		if (f->known[i] && f->pred[i] == msg[off + i])
			k++;
	return k;
}

static int find_slip(const dr_view *v, const dr_fit *f, uint8_t *best,
                     int *at_out, int *slip_out)
{
	uint8_t *try = NULL;
	int lo = -1, hi = -1, i, d, b;
	int base, bestk = -1, gain;

	*at_out = 0;
	*slip_out = 0;

	for (i = 0; i < v->data_len; i++)
		if (f->known[i] && f->pred[i] != (v->msg + v->data_offset)[i]) {
			if (lo < 0)
				lo = i;
			hi = i;
		}
	if (lo < 0 || hi - lo < 8)
		return 0;              /* nothing re-framed, just damaged */

	base = agree(v->msg, f, v->data_offset, v->data_len);

	try = malloc((size_t)v->msg_len);
	if (!try)
		return 0;

	for (b = lo; b <= hi; b++) {
		int at = (v->data_offset + b) * v->stride;

		for (d = -SLIP_MAX; d <= SLIP_MAX; d++) {
			int k;

			if (!d)
				continue;
			dr_decode_slipped(v, at, d, try);
			k = agree(try, f, v->data_offset, v->data_len);
			if (k > bestk) {
				bestk = k;
				*at_out = at;
				*slip_out = d;
				memcpy(best, try, (size_t)v->msg_len);
			}
		}
	}
	free(try);

	/*
	 * Insist on a real improvement. A slip that recovers a handful of
	 * bytes is a coincidence; one that recovers a tail is the diagnosis.
	 */
	gain = bestk - base;
	DBG("slip: damaged bytes %d..%d, base agree %d, best %d (%+d cells at "
	    "byte %d), gain %d\n", lo, hi, base, bestk, *slip_out,
	    *at_out / v->stride, gain);
	if (gain < 16 || gain < v->data_len / 32) {
		*slip_out = 0;
		return 0;
	}
	return gain;
}

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
	const uint8_t *msg0;
	uint8_t *slipped = NULL, *cmsg = NULL;
	uint16_t base_syn;
	int crc_may_be_damaged = 0;
	int slip_at = 0, slip = 0, max_leave = 12, crc_fixed = 0;
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

	/*
	 * Did the decoder lose its place rather than lose a bit?
	 *
	 * If a run of intervals was given one cell too many or too few,
	 * the byte boundary moves and every byte after it decodes as
	 * something else entirely - not corrupted, just re-framed. A model
	 * that fits the head of a field and then collapses is the
	 * signature, and no amount of bit-flipping repairs it, because the
	 * bits are not wrong.
	 *
	 * The fitted prediction is what makes this cheap to test: it
	 * extrapolates over the whole field whether or not the field
	 * agrees, so a re-framed tail can simply be scored against it.
	 */
	{
		dr_fit rep;

		/*
		 * Scored against the *repeat*, not against whichever model
		 * won overall. A slip is exactly what breaks a repeat - the
		 * bytes are right, the boundary is not - and a repeat
		 * extrapolates over damage instead of abstaining on it,
		 * which is what a search for the damage needs.
		 */
		if (fit_periodic(data, n, &rep) == 0) {
			if (rep.outliers > 0) {
				slipped = malloc((size_t)v->msg_len);
				if (!slipped) {
					fit_free(&rep);
					rc = -1;
					goto done;
				}
				find_slip(v, &rep, slipped, &slip_at, &slip);
			}
			fit_free(&rep);
		}
		if (slip) {
			dr_fit g;
			int ok = (fit_best(slipped + v->data_offset, n, &g) == 0);

			DBG("slip: unslipped fit '%s' explains %d (%d out); "
			    "slipped fit '%s' explains %d (%d out)\n",
			    f.desc, f.explained, f.outliers,
			    ok ? g.desc : "-", ok ? g.explained : -1,
			    ok ? g.outliers : -1);
			if (ok && g.explained > f.explained) {
				fit_free(&f);
				f = g;
				data = slipped + v->data_offset;
			} else {
				slip = 0;
			}
		}
		if (!slip) {
			free(slipped);
			slipped = NULL;
		}
	}

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
	/*
	 * The cap is on the *enumeration*, not on the model. Restoring
	 * every outlier is a single reading however many there are, and it
	 * is the one Occam nominates, so it is always tried; only the
	 * combinations that leave some of them broken have to be bounded.
	 */
	if (f.outliers > opt->max_outliers)
		max_leave = 0;

	/*
	 * Everything below is relative to the reading the model actually
	 * fits - which is the re-framed one if a phase correction won.
	 */
	msg0 = slip ? slipped : v->msg;
	base_syn = dr_crc16(msg0, v->msg_len);

	/*
	 * The stored CRC is only in doubt if the damage reaches it. A
	 * sector whose last bytes fit the model has a CRC that was read
	 * cleanly, and there is no excuse for touching it.
	 */
	for (i = v->data_len - 8; i < v->data_len; i++)
		if (i >= 0 && f.known[i] && f.pred[i] != data[i])
			crc_may_be_damaged = 1;
	if (slip && slip_at < (v->msg_len - 2) * v->stride)
		crc_may_be_damaged = 1;

	masks = malloc((size_t)v->msg_bits * sizeof(uint16_t));
	outpos = malloc((size_t)f.outliers * sizeof(int));
	fixed = malloc((size_t)n);
	cmsg = malloc((size_t)v->msg_len);
	if (!masks || !outpos || !fixed || !cmsg) {
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
		int idx[24], t;

		if (leave > max_leave)
			break;
		if (leave && (nout > 24 || leave > 12))
			break;
		if (leave > nout)
			break;

		/* Enumerate the C(nout, leave) subsets directly. Walking all
		 * 2^nout masks and filtering by popcount looks equivalent and
		 * is not: it does twenty million iterations to find the few
		 * thousand of the right size. */
		for (i = 0; i < leave; i++)
			idx[i] = i;

		for (;;) {
			uint16_t syn = base_syn;
			int bits[DR_MAX_WEIGHT], nb = 0, ok = 1, ncrc;

			explored++;

			memcpy(fixed, data, (size_t)n);
			for (i = 0; i < nout && ok; i++) {
				int pos = outpos[i];
				uint8_t want, diff;
				int skip = 0;

				for (t = 0; t < leave; t++)
					if (idx[t] == i) { skip = 1; break; }
				if (skip)
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

			/*
			 * A damaged CRC is still a measurement.
			 *
			 * The two CRC bytes sit at the end of the sector with
			 * nothing protecting them, so damage that reaches the
			 * tail corrupts them too - and then demanding an exact
			 * match rejects the true reading and accepts whatever
			 * else happens to match the corrupted target. When a
			 * trustworthy model determines the data, the honest
			 * question is how many of the 16 stored bits agree,
			 * not whether all of them do. One wrong bit still
			 * leaves a 1-in-3855 coincidence; two leaves 1 in 478,
			 * which is where this stops.
			 */
			ncrc = 0;
			if (ok && nb && syn && crc_may_be_damaged) {
				uint16_t want, have;
				uint8_t d[2];

				/*
				 * Ask what CRC the corrected data implies and
				 * compare it with the two bytes as read,
				 * rather than trying to steer the syndrome to
				 * zero with bit masks - the masks of the
				 * stored CRC's own bits are not unit vectors,
				 * and treating them as if they were is a way
				 * to be confidently wrong.
				 */
				memcpy(cmsg, msg0, (size_t)v->msg_len);
				memcpy(cmsg + v->data_offset, fixed, (size_t)n);
				want = dr_crc16(cmsg, v->msg_len - 2);
				have = (uint16_t)((cmsg[v->msg_len - 2] << 8) |
				                   cmsg[v->msg_len - 1]);
				d[0] = (uint8_t)((want ^ have) >> 8);
				d[1] = (uint8_t)((want ^ have) & 0xFF);

				for (i = 0; i < 2; i++)
					for (j = 0; j < 8; j++)
						if (d[i] & (0x80 >> j))
							ncrc++;
				if (ncrc > opt->crc_budget ||
				    nb + ncrc > DR_MAX_WEIGHT) {
					ok = 0;
				} else {
					for (i = 0; i < 2; i++)
						for (j = 0; j < 8; j++)
							if (d[i] & (0x80 >> j))
								bits[nb++] =
								  (v->msg_len
								   - 2 + i) * 8
								  + j;
					syn = 0;
				}
			}

			if (ok && nb && !syn) {
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
					cd->slip_at = slip_at;
					cd->slip = slip;
					for (j = 0; j < nb; j++) {
						cd->bits[j] = bits[j];
						cd->before[j] = (uint8_t)
						  ((msg0[bits[j] >> 3] >>
						    (7 - (bits[j] & 7))) & 1);
					}
					if (ncrc > crc_fixed)
						crc_fixed = ncrc;
					cd->data_prior = dmodel_delta(&m, data,
					                              fixed, n);
					cd->log_likelihood = cd->data_prior;
				}
			}

			if (!leave)
				break;
			i = leave - 1;
			while (i >= 0 && idx[i] == nout - (leave - i))
				i--;
			if (i < 0)
				break;
			idx[i]++;
			for (++i; i < leave; i++)
				idx[i] = idx[i - 1] + 1;
		}
	}

	out->explored = explored;
	out->slip = slip;
	out->slip_byte = slip_at / v->stride;
	out->crc_fixed = crc_fixed;
	if (slip && nfound)
		snprintf(out->note, sizeof(out->note),
		         "%.100s, once the decoder's %d-cell slip at byte %d "
		         "is taken out%s", f.desc, slip < 0 ? -slip : slip,
		         slip_at / v->stride,
		         crc_fixed ? " - and the stored CRC is damaged too"
		                   : "");
	else if (crc_fixed)
		snprintf(out->note, sizeof(out->note),
		         "%.180s - and the stored CRC is damaged too", f.desc);

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
	free(cmsg);
	free(slipped);
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
