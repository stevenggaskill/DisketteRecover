/*
 * DisketteRecover - re-reading a sector's flux under a different, but
 * equally legal, binning of its transitions.
 *
 * MFM only ever writes three interval lengths - 2, 3 and 4 cell periods
 * - and the decoder's whole job is to sort each measured interval into
 * one of them. That is where a marginal disk actually fails, and it
 * fails in three ways:
 *
 *   mis-bin   the PLL puts a reversal in the wrong bin, and steals the
 *             difference from the next one, so a true (4,2) reads back
 *             as (3,3);
 *   dropout   a reversal was too weak to detect, so one measured
 *             interval covers two real ones;
 *   spurious  noise added a reversal, so two measured intervals cover
 *             one real one.
 *
 * All three leave the total cell count intact, and that conservation is
 * the redundancy worth exploiting. The byte grid downstream still framed
 * correctly - the sync was found, the sector ended where it should - so
 * the cell count across a disturbed stretch is known even when the
 * individual intervals are not. Any re-reading has to preserve it.
 *
 * So the search here is not over bit flips. It is over re-readings of
 * the flux, subject to
 *
 *   - every interval is 2, 3 or 4 cells (the MFM run-length rule), and
 *   - each disturbed stretch spans exactly the cells it did before,
 *
 * ordered by how well they explain the measured timings, and accepted
 * when the resulting bytes satisfy the sector's CRC. Intervals whose
 * timing pins them to one bin are held fixed, so the branching happens
 * only where the disk is genuinely ambiguous.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dr_internal.h"

#define MIN_BIN     2
#define MAX_BIN     4
#define MAX_DEV     10       /* cells a run may drift mid-way          */
#define DEV_SPAN    (2 * MAX_DEV + 1)
#define BIG         1e18
/* Real flux residuals are near-Gaussian out to about 4 sigma and then
 * break into a separate population of genuine mis-reads, so that is
 * where the line goes: 4.5 sigma, i.e. 0.5 * 4.5^2 nats. */
#define DIRTY_COST  10.1
#define DILATE      2        /* intervals of context around a defect    */
#define RUN_CANDS   4096     /* worked out per stretch before trimming  */
#define MAX_RUNS    32       /* disturbed stretches the merge can combine */

#define DBG(...) do { if (getenv("DR_DEBUG")) \
	fprintf(stderr, "dr_rebin: " __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Interval extraction                                                 */
/* ------------------------------------------------------------------ */
/*
 * The cell period, sampled once where the sector starts and held for the
 * whole of it.
 *
 * That is not always true - a stretch rewritten in a different drive runs
 * at that drive's speed, and a few percent is enough to push a 4T past
 * the top of its bin, which is how a region ends up decoded with 5- and
 * 6-cell gaps that MFM cannot produce at all. libhxcfe's per-byte rate
 * does not rescue that: it is smoothed over a 24-byte window and two
 * filter passes, so following it interval by interval destroys the fit
 * rather than sharpening it (measured - the model stops fitting at all).
 * Recovering a local rate well enough to help means fitting it from the
 * flux directly, and until that exists a sector with a speed splice is
 * reported rather than repaired.
 */
double dr_cell_period(const dr_view *v)
{
	dr_flux_map *fx = (dr_flux_map *)v->flux;
	HXCFE_SIDE *side = (HXCFE_SIDE *)v->side;
	double bitrate;

	if (!fx || !fx->valid || !side)
		return 0.0;

	bitrate = side->timingbuffer
	        ? (double)side->timingbuffer[dr_wrap(side->tracklen,
	                                             v->base_cell) / 8]
	        : (double)side->bitrate;
	if (bitrate < 1000.0)
		bitrate = (double)side->bitrate;
	if (bitrate < 1000.0)
		return 0.0;
	return (double)fx->tick_freq / (2.0 * bitrate);
}

int dr_intervals_collect(dr_view *v, dr_interval **out, double *period_out)
{
	dr_flux_map *fx = (dr_flux_map *)v->flux;
	HXCFE_SIDE *side = (HXCFE_SIDE *)v->side;
	dr_interval *iv;
	dr_revmap *rm;
	double period0 = 0.0;
	int i, n = 0, prev = -1;

	*out = NULL;
	if (!fx || !fx->valid)
		return -1;

	period0 = dr_cell_period(v);
	if (period0 <= 0.0)
		return -1;

	iv = malloc((size_t)v->ncells * sizeof(*iv));
	if (!iv)
		return -1;

	/*
	 * Where the dump holds more than one pass over the track, an
	 * interval's duration is the mean of the passes that read it the
	 * same way. Reversals are magnetised into the oxide, so the passes
	 * normally agree to a fraction of a tick and averaging simply
	 * divides the read noise by the root of their number; where they
	 * disagree, that disagreement is itself the measurement worth
	 * having, and dr_revs.c has counted it.
	 *
	 * The consensus is indexed by the dump's own pulse number, so a
	 * decode that gained or lost a cell mid-sector still lines up.
	 */
	rm = (dr_revmap *)v->revs;

	for (i = 0; i < v->ncells; i++) {
		uint32_t p;

		if (!v->cells[i].state)
			continue;
		if (prev < 0) {
			prev = i;
			continue;
		}

		p = fx->pulse_of_cell[dr_wrap(side->tracklen, v->base_cell + i)];
		iv[n].cell  = i;
		iv[n].gap   = i - prev;
		iv[n].pulse = p;
		iv[n].ticks = (p != 0xFFFFFFFFu && p < fx->nb_pulses)
		                      ? fx->stream[p] : 0;
		if (rm && iv[n].ticks && p >= rm->p_first &&
		    (int)(p - rm->p_first) < rm->n)
			iv[n].ticks = rm->ticks[p - rm->p_first];
		iv[n].meas  = iv[n].ticks ? (double)iv[n].ticks / period0 : -1.0;
		iv[n].adj   = iv[n].meas;
		n++;
		prev = i;
	}

	if (n < 32) {
		free(iv);
		return -1;
	}

	*out = iv;
	if (period_out)
		*period_out = period0;
	return n;
}

/* ------------------------------------------------------------------ */
/* Timing model                                                        */
/* ------------------------------------------------------------------ */
/* Solve a small symmetric system by Gaussian elimination with partial
 * pivoting. Returns 0 on success. */
static int solve(double A[4][5], int n, double *x)
{
	int i, j, k;

	for (i = 0; i < n; i++) {
		int piv = i;
		double t;

		for (j = i + 1; j < n; j++)
			if (fabs(A[j][i]) > fabs(A[piv][i]))
				piv = j;
		if (fabs(A[piv][i]) < 1e-12)
			return -1;
		if (piv != i)
			for (k = i; k <= n; k++) {
				t = A[i][k]; A[i][k] = A[piv][k]; A[piv][k] = t;
			}
		for (j = i + 1; j < n; j++) {
			double f = A[j][i] / A[i][i];
			for (k = i; k <= n; k++)
				A[j][k] -= f * A[i][k];
		}
	}
	for (i = n - 1; i >= 0; i--) {
		double sum = A[i][n];
		for (j = i + 1; j < n; j++)
			sum -= A[i][j] * x[j];
		x[i] = sum / A[i][i];
	}
	return 0;
}

/* Regressors for interval i: 1, its own bin, and its two neighbours. */
static void design(const dr_interval *iv, int n, int i, double *x)
{
	x[0] = 1.0;
	x[1] = (double)iv[i].gap;
	x[2] = (i > 0) ? (double)iv[i - 1].gap : 3.0;
	x[3] = (i + 1 < n) ? (double)iv[i + 1].gap : 3.0;
}

static int usable(const dr_interval *iv, int i)
{
	return iv[i].meas > 0.0 && iv[i].gap >= MIN_BIN && iv[i].gap <= MAX_BIN;
}

/*
 * Fit  meas ~ a + b*k + c*k_prev + d*k_next.
 *
 * The neighbour terms are not a curiosity: adjacent reversals repel each
 * other, so on real media a 2T following a 4T measures ~2.20 cells while
 * a 2T following a 2T measures ~1.87. Fitting only the offset and gain
 * buries that 0.3-cell swing in the residual, which then makes a sixth
 * of a perfectly good track look ambiguous.
 */
/* Least squares for one block, returning 0 and filling sol/sd on
 * success. `keep` is the residual window; a single robust pass is enough
 * because the global fit has already set the scale. */
static int fit_block(const dr_interval *iv, int n, int lo, int hi,
                     const dr_timing *g, double *sol, double *sd, int *used)
{
	double A[4][5], x[4], ss = 0.0;
	int pass, i, j, k, cnt = 0;
	double keep = 4.0 * g->sigma;

	sol[0] = g->a; sol[1] = g->b; sol[2] = g->c; sol[3] = g->d;

	for (pass = 0; pass < 2; pass++) {
		memset(A, 0, sizeof(A));
		cnt = 0;
		for (i = lo; i < hi && i < n; i++) {
			double pred, r;

			if (i < 1 || i + 1 >= n || !usable(iv, i))
				continue;
			design(iv, n, i, x);
			pred = sol[0] + sol[1] * x[1] + sol[2] * x[2]
			     + sol[3] * x[3];
			r = iv[i].meas - pred;
			if (fabs(r) > keep)
				continue;
			for (j = 0; j < 4; j++) {
				for (k = 0; k < 4; k++)
					A[j][k] += x[j] * x[k];
				A[j][4] += x[j] * iv[i].meas;
			}
			cnt++;
		}
		if (cnt < 64)
			return -1;
		if (solve(A, 4, sol) < 0)
			return -1;
	}

	ss = 0.0;
	cnt = 0;
	for (i = lo; i < hi && i < n; i++) {
		double pred, r;

		if (i < 1 || i + 1 >= n || !usable(iv, i))
			continue;
		design(iv, n, i, x);
		pred = sol[0] + sol[1] * x[1] + sol[2] * x[2] + sol[3] * x[3];
		r = iv[i].meas - pred;
		if (fabs(r) > keep)
			continue;
		ss += r * r;
		cnt++;
	}
	if (cnt < 64)
		return -1;
	*sd = sqrt(ss / cnt);
	*used = cnt;
	return 0;
}

/* The spread of the block residuals around the *global* fit, which is
 * what a local fit has to beat to be worth having. */
static double global_sd(const dr_interval *iv, int n, int lo, int hi,
                        const dr_timing *g)
{
	double x[4], ss = 0.0;
	int i, cnt = 0;

	for (i = lo; i < hi && i < n; i++) {
		double r;

		if (i < 1 || i + 1 >= n || !usable(iv, i))
			continue;
		design(iv, n, i, x);
		r = iv[i].meas - (g->a + g->b * x[1] + g->c * x[2] +
		                  g->d * x[3]);
		ss += r * r;
		cnt++;
	}
	return cnt ? sqrt(ss / cnt) : g->sigma;
}

static void fit_blocks(const dr_interval *iv, int n, dr_timing *t)
{
	int width, i;

	t->nint = n;
	t->nblk = 0;
	t->nlocal = 0;
	t->bmin = t->bmax = t->b;

	width = n / 8;
	if (width < 192)
		width = 192;
	if (width > n)
		return;
	if (n / (width / 2) >= DR_TIMING_BLOCKS)
		width = 2 * n / (DR_TIMING_BLOCKS - 1);
	t->width = width;

	for (i = 0; i * (width / 2) < n && t->nblk < DR_TIMING_BLOCKS; i++) {
		int lo = i * (width / 2);
		int hi = lo + width;
		int b = t->nblk++;
		double sol[4], sd = 0.0, gsd;
		int used = 0;

		t->la[b] = t->a; t->lb[b] = t->b;
		t->lc[b] = t->c; t->ld[b] = t->d;
		t->lsigma[b] = t->sigma;
		t->lok[b] = 0;

		if (hi > n)
			hi = n;
		gsd = global_sd(iv, n, lo, hi, t);

		if (fit_block(iv, n, lo, hi, t, sol, &sd, &used) < 0) {
			/* Too little to say anything local about. */
			if (gsd > t->sigma)
				t->lsigma[b] = gsd;
			continue;
		}

		/*
		 * A local gain outside this range is not a channel that
		 * drifted, it is a stretch with no readable structure left -
		 * the fit has latched onto noise. Keep the global centres so
		 * the bins stay where the rest of the track says they are,
		 * and keep the large residual, so the region reads as
		 * exactly what it is: not to be trusted.
		 */
		if (sol[1] < 0.75 || sol[1] > 1.25 || sd >= gsd) {
			t->lsigma[b] = (sd > gsd ? sd : gsd);
			if (t->lsigma[b] < t->sigma)
				t->lsigma[b] = t->sigma;
			continue;
		}

		t->la[b] = sol[0]; t->lb[b] = sol[1];
		t->lc[b] = sol[2]; t->ld[b] = sol[3];
		t->lsigma[b] = sd < 0.015 ? 0.015 : sd;
		t->lok[b] = 1;
		t->nlocal++;
		if (sol[1] < t->bmin) t->bmin = sol[1];
		if (sol[1] > t->bmax) t->bmax = sol[1];
	}
}

void dr_timing_fit(const dr_interval *iv, int n, dr_timing *t)
{
	int pass, i, j, k, used = 0;
	double keep = 0.40;

	t->a = 0.0; t->b = 1.0; t->c = 0.0; t->d = 0.0;
	t->sigma = 0.12;
	t->n = 0;
	t->valid = 0;

	for (pass = 0; pass < 5; pass++) {
		double A[4][5], sol[4], x[4], ss = 0.0;
		int nreg = (pass == 0) ? 2 : 4;   /* neighbours once we are close */

		memset(A, 0, sizeof(A));
		used = 0;

		for (i = 0; i < n; i++) {
			double pred, r;

			if (!usable(iv, i))
				continue;
			design(iv, n, i, x);
			pred = t->a + t->b * x[1] + t->c * x[2] + t->d * x[3];
			r = iv[i].meas - pred;
			if (fabs(r) > keep)
				continue;

			for (j = 0; j < nreg; j++) {
				for (k = 0; k < nreg; k++)
					A[j][k] += x[j] * x[k];
				A[j][nreg] += x[j] * iv[i].meas;
			}
			used++;
		}

		if (used < 32)
			return;

		if (solve(A, nreg, sol) < 0)
			return;

		t->a = sol[0];
		t->b = sol[1];
		t->c = (nreg > 2) ? sol[2] : 0.0;
		t->d = (nreg > 3) ? sol[3] : 0.0;

		used = 0;
		for (i = 0; i < n; i++) {
			double pred, r;

			if (!usable(iv, i))
				continue;
			design(iv, n, i, x);
			pred = t->a + t->b * x[1] + t->c * x[2] + t->d * x[3];
			r = iv[i].meas - pred;
			if (fabs(r) > keep)
				continue;
			ss += r * r;
			used++;
		}
		if (!used)
			return;

		t->sigma = sqrt(ss / used);
		if (t->sigma < 0.015)
			t->sigma = 0.015;
		t->n = used;
		keep = 3.5 * t->sigma;
		if (keep < 0.10)
			keep = 0.10;
	}

	/*
	 * Interval errors are not independent, and assuming they are is a
	 * real mistake rather than a rounding one.
	 *
	 * What the medium and the head actually perturb is the *position*
	 * of a reversal. An interval is the gap between two of them, so an
	 * error e_j in one position lands in two consecutive intervals with
	 * opposite sign: r_j = e_j - e_{j-1}. That makes adjacent interval
	 * residuals correlated at -1/2, and it means a long interval
	 * followed by an equally short one is a single displaced reversal,
	 * not two independent 3-sigma surprises. Scoring them independently
	 * charges twice for one event and blinds the search to the
	 * compensating pairs that are the commonest failure of all.
	 *
	 * Measure the correlation, and recover the position noise from it:
	 * var(r) = 2*sigma_pos^2 + sigma_indep^2 and cov(r_j, r_j+1) =
	 * -sigma_pos^2, so sigma_pos^2 = -rho * var(r).
	 */
	{
		double m = 0.0, v = 0.0, c = 0.0;
		int used2 = 0, k, prev_ok = 0;
		double prev_r = 0.0;

		for (i = 0; i < n; i++) {
			double x[4], r;

			if (!usable(iv, i)) { prev_ok = 0; continue; }
			design(iv, n, i, x);
			r = iv[i].meas - (t->a + t->b * x[1] + t->c * x[2] +
			                  t->d * x[3]);
			if (fabs(r) > 3.5 * t->sigma) { prev_ok = 0; continue; }
			m += r;
			v += r * r;
			if (prev_ok) {
				c += prev_r * r;
				used2++;
			}
			prev_r = r;
			prev_ok = 1;
			k = 0; (void)k;
		}
		if (v > 0.0 && used2 > 32) {
			t->rho = c / v;
			if (t->rho > -0.05) t->rho = -0.05;
			if (t->rho < -0.75) t->rho = -0.75;
		} else {
			t->rho = -0.5;
		}
		/*
		 * Residuals are measured against an anchor transition that
		 * carries its own error, so what the search actually sees is
		 * e_j - e_anchor, with twice the variance of a single
		 * position. Dividing by sigma_pos alone halves the
		 * denominator, doubles every cost, and turns a marginal
		 * preference into a claimed certainty.
		 */
		t->sigma_pos = t->sigma * sqrt(-2.0 * t->rho);
		if (t->sigma_pos < 0.01)
			t->sigma_pos = 0.01;
		(void)m;
	}

	if (t->b > 0.6 && t->b < 1.6 && t->n >= 32)
		t->valid = 1;

	if (t->valid)
		fit_blocks(iv, n, t);
}

/*
 * The model at interval j, interpolated between the two block fits whose
 * centres straddle it. Blending rather than stepping matters: a step in
 * the bin centres halfway through a sector would put a seam of spurious
 * ambiguity at every block boundary.
 */
void dr_timing_at(const dr_timing *t, int j, double *a, double *b,
                  double *c, double *d, double *sigma)
{
	double u;
	int half, i0, i1;

	*a = t->a; *b = t->b; *c = t->c; *d = t->d; *sigma = t->sigma;
	if (j < 0 || t->nblk <= 0 || t->width <= 0)
		return;

	half = t->width / 2;
	if (half <= 0)
		return;

	/* Block i covers [i*half, i*half + width), centre i*half + half. */
	i0 = (j - half) / half;
	if (i0 < 0)
		i0 = 0;
	if (i0 > t->nblk - 1)
		i0 = t->nblk - 1;
	i1 = i0 + 1 < t->nblk ? i0 + 1 : i0;

	u = (double)(j - (i0 * half + half)) / (double)half;
	if (u < 0.0) u = 0.0;
	if (u > 1.0) u = 1.0;

	*a = t->la[i0] * (1.0 - u) + t->la[i1] * u;
	*b = t->lb[i0] * (1.0 - u) + t->lb[i1] * u;
	*c = t->lc[i0] * (1.0 - u) + t->lc[i1] * u;
	*d = t->ld[i0] * (1.0 - u) + t->ld[i1] * u;
	/* Uncertainty takes the worse of the two: a boundary next to a
	 * ruined block is not half safe. */
	*sigma = t->lsigma[i0] > t->lsigma[i1] ? t->lsigma[i0]
	                                       : t->lsigma[i1];
}

double dr_timing_adjust(const dr_timing *t, int j, double meas,
                        int prev_gap, int next_gap)
{
	double a, b, c, d, sg;

	if (meas <= 0.0)
		return meas;
	if (prev_gap < MIN_BIN || prev_gap > MAX_BIN) prev_gap = 3;
	if (next_gap < MIN_BIN || next_gap > MAX_BIN) next_gap = 3;
	dr_timing_at(t, j, &a, &b, &c, &d, &sg);
	return meas - c * (double)prev_gap - d * (double)next_gap;
}

double dr_bin_cost(const dr_timing *t, int j, double meas, int k)
{
	double a, b, c, d, sg, r;

	if (meas <= 0.0)
		return 0.0;
	dr_timing_at(t, j, &a, &b, &c, &d, &sg);
	if (sg < 1e-6)
		sg = 1e-6;
	r = (meas - (a + b * (double)k)) / sg;
	return 0.5 * r * r;
}

/* ------------------------------------------------------------------ */
/* Per-run search                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
	int    nbits;
	int    bits[DR_MAX_WEIGHT];
	double cost;
	int    rebins;
} runcand;

typedef struct {
	int      first, last;      /* measured interval index range        */
	runcand *cands;
	int      n;
} runset;

/* A* node: one decision, chained back to its parent. */
typedef struct {
	int    parent;
	int8_t kind;    /* 0 single, 1 split, 2 merge                     */
	int8_t k1, k2;
	int    j;       /* measured interval this decision consumed       */
} rnode;

typedef struct {
	double g, f;
	int    node, j, s;
} rstate;

typedef struct { rstate *v; int n, cap; } rheap;

static int rpush(rheap *h, rstate s)
{
	int i;

	if (h->n == h->cap) {
		int nc = h->cap ? h->cap * 2 : 1024;
		rstate *nv = realloc(h->v, (size_t)nc * sizeof(*nv));
		if (!nv)
			return -1;
		h->v = nv;
		h->cap = nc;
	}
	i = h->n++;
	h->v[i] = s;
	while (i > 0) {
		int p = (i - 1) / 2;
		if (h->v[p].f <= h->v[i].f)
			break;
		s = h->v[p]; h->v[p] = h->v[i]; h->v[i] = s;
		i = p;
	}
	return 0;
}

static int rpop(rheap *h, rstate *out)
{
	int i = 0;

	if (!h->n)
		return -1;
	*out = h->v[0];
	h->v[0] = h->v[--h->n];
	for (;;) {
		int l = 2 * i + 1, r = l + 1, m = i;
		rstate t;

		if (l < h->n && h->v[l].f < h->v[m].f) m = l;
		if (r < h->n && h->v[r].f < h->v[m].f) m = r;
		if (m == i)
			break;
		t = h->v[m]; h->v[m] = h->v[i]; h->v[i] = t;
		i = m;
	}
	return 0;
}

struct ctx {
	dr_view      *v;
	dr_interval  *iv;
	int           niv;
	dr_timing     t;
	double        p_drop, p_spur;
	int           stride;

	/* Running sums so a transition's position residual is a function of
	 * the DP state alone: how far the reading has drifted from the
	 * measurements by the time it reaches interval j having spent
	 * `dev` extra cells. */
	double       *cum_meas;   /* measured cells before interval j     */
	double       *cum_pll;    /* cells the decoder assigned, same     */
};

/*
 * The cost of standing at measured transition j after spending `dev`
 * cells more than the decoder did. Because the noise is on positions,
 * the penalty is on where the reading has drifted to - not on how wide
 * any single interval came out. A pair that runs long then short costs
 * almost nothing; a reading that drifts and stays drifted pays for every
 * transition it stays wrong.
 */
static double pos_cost(const struct ctx *X, int j, int dev)
{
	double r = X->cum_meas[j] - (X->cum_pll[j] + (double)dev);
	double z = r / X->t.sigma_pos;

	return 0.5 * z * z;
}

/* Turn a chain of decisions into transition positions, decode the bytes
 * they touch, and record which message bits moved. */
static int run_decode(struct ctx *X, const rnode *arena, int leaf,
                      int first, int last, runcand *out)
{
	dr_view *v = X->v;
	int order[256], nord = 0, node;
	int pos, c0, c1, blo, bhi, b, k, n = 0, nre = 0;
	uint8_t *cells;
	int newpos[512], nnew = 0;

	for (node = leaf; node >= 0; node = arena[node].parent) {
		if (nord >= (int)(sizeof(order) / sizeof(order[0])))
			return -1;
		order[nord++] = node;
	}

	c0 = X->iv[first].cell - X->iv[first].gap;   /* anchor before run */
	c1 = X->iv[last].cell;                       /* anchor after run  */
	pos = c0;

	/* The chain was walked backwards, so replay it forwards. */
	for (k = nord - 1; k >= 0; k--) {
		const rnode *nd = &arena[order[k]];

		if (nnew + 2 >= (int)(sizeof(newpos) / sizeof(newpos[0])))
			return -1;

		switch (nd->kind) {
		case 0:
			pos += nd->k1;
			newpos[nnew++] = pos;
			if (nd->k1 != X->iv[nd->j].gap)
				nre++;
			break;
		case 1:                                  /* dropout */
			pos += nd->k1;
			newpos[nnew++] = pos;
			pos += nd->k2;
			newpos[nnew++] = pos;
			nre++;
			break;
		case 2:                                  /* spurious */
			pos += nd->k1;
			newpos[nnew++] = pos;
			nre++;
			break;
		}
	}

	if (pos != c1)
		return -1;                               /* should not happen */

	blo = c0 / X->stride - 1;
	bhi = c1 / X->stride + 2;
	if (blo < 0) blo = 0;
	if (bhi > v->msg_len) bhi = v->msg_len;
	if (bhi <= blo)
		return -1;

	{
		int cs = blo * X->stride, ce = bhi * X->stride;
		int span = ce - cs;

		cells = malloc((size_t)span);
		if (!cells)
			return -1;

		/* Start from the current reading, then replace the run. */
		for (b = 0; b < span; b++)
			cells[b] = (uint8_t)(cs + b < v->ncells
			                     ? v->cells[cs + b].state : 0);
		for (b = c0 + 1; b <= c1; b++)
			if (b >= cs && b < ce)
				cells[b - cs] = 0;
		for (k = 0; k < nnew; k++)
			if (newpos[k] >= cs && newpos[k] < ce)
				cells[newpos[k] - cs] = 1;

		for (b = blo; b < bhi; b++) {
			uint8_t val = 0, diff;

			for (k = 0; k < 8; k++) {
				int cc, dc, o = (b - blo) * X->stride;

				dr_bit_cells(v->encoding, o, k, &cc, &dc);
				val = (uint8_t)(val << 1);
				if (v->encoding == DR_ENC_ISO_FM) {
					if (cells[dc])
						val |= 1;
				} else if (!cells[cc] && cells[dc]) {
					val |= 1;
				}
			}

			diff = (uint8_t)(val ^ v->msg[b]);
			for (k = 0; k < 8 && diff; k++) {
				if (!(diff & (0x80 >> k)))
					continue;
				if (n >= DR_MAX_WEIGHT) {
					free(cells);
					return -1;
				}
				out->bits[n++] = b * 8 + k;
			}
		}
		free(cells);
	}

	out->nbits = n;
	out->rebins = nre;
	return 0;
}

static int cmp_runcand(const void *a, const void *b)
{
	const runcand *x = a, *y = b;

	if (x->cost < y->cost) return -1;
	if (x->cost > y->cost) return 1;
	return 0;
}

/* Enumerate the best re-readings of one disturbed stretch. */
static int run_search(struct ctx *X, int first, int last,
                      long budget_nodes, runset *rs)
{
	int M = last - first + 1;
	double *h = NULL;
	rnode *arena = NULL;
	rheap H;
	rstate s;
	int arena_n = 0, arena_cap = 0;
	int j, d, k, rc = -1;
	long nodes = 0;
	double floor_cost;

	memset(&H, 0, sizeof(H));
	rs->first = first;
	rs->last = last;
	rs->cands = NULL;
	rs->n = 0;

	h = malloc((size_t)(M + 2) * DEV_SPAN * sizeof(double));
	if (!h)
		return -1;
	for (j = 0; j <= M + 1; j++)
		for (d = 0; d < DEV_SPAN; d++)
			h[(size_t)j * DEV_SPAN + d] = BIG;
	h[(size_t)M * DEV_SPAN + MAX_DEV] = 0.0;     /* done, no drift */

	/*
	 * Suffix bounds. A move's cost is the position residual at the
	 * measured transition it lands on, so it depends only on where the
	 * move leaves us - which is exactly the DP state.
	 */
	for (j = M - 1; j >= 0; j--) {
		int idx = first + j;

		for (d = 0; d < DEV_SPAN; d++) {
			double best = BIG, c;
			int nd;

			for (k = MIN_BIN; k <= MAX_BIN; k++) {
				nd = d + (k - X->iv[idx].gap);
				if (nd < 0 || nd >= DEV_SPAN)
					continue;
				c = pos_cost(X, first + j + 1, nd - MAX_DEV) +
				    h[(size_t)(j + 1) * DEV_SPAN + nd];
				if (c < best) best = c;
			}

			{	/* dropout: this measurement hides two intervals.
				 * The reversal we insert was never measured, so
				 * it contributes no residual of its own. */
				int k1, k2;
				for (k1 = MIN_BIN; k1 <= MAX_BIN; k1++)
				for (k2 = MIN_BIN; k2 <= MAX_BIN; k2++) {
					nd = d + (k1 + k2 - X->iv[idx].gap);
					if (nd < 0 || nd >= DEV_SPAN)
						continue;
					c = pos_cost(X, first + j + 1,
					             nd - MAX_DEV) + X->p_drop +
					    h[(size_t)(j + 1) * DEV_SPAN + nd];
					if (c < best) best = c;
				}
			}

			if (j + 1 < M) {      /* spurious: two make up one.
					       * The extra measured reversal
					       * should not exist, so its
					       * residual is not charged. */
				int pll2 = X->iv[idx].gap + X->iv[idx + 1].gap;

				for (k = MIN_BIN; k <= MAX_BIN; k++) {
					nd = d + (k - pll2);
					if (nd < 0 || nd >= DEV_SPAN)
						continue;
					c = pos_cost(X, first + j + 2,
					             nd - MAX_DEV) + X->p_spur +
					    h[(size_t)(j + 2) * DEV_SPAN + nd];
					if (c < best) best = c;
				}
			}

			h[(size_t)j * DEV_SPAN + d] = best;
		}
	}

	floor_cost = h[MAX_DEV];
	if (floor_cost >= BIG) {
		free(h);
		return 0;                     /* no legal re-reading at all */
	}

	arena_cap = 4096;
	arena = malloc((size_t)arena_cap * sizeof(*arena));
	if (!arena)
		goto out;

	s.g = 0.0; s.f = floor_cost; s.node = -1; s.j = 0; s.s = MAX_DEV;
	if (rpush(&H, s) < 0)
		goto out;

	while (rpop(&H, &s) == 0) {
		int idx;

		if (nodes++ > budget_nodes)
			break;
		if (rs->n >= RUN_CANDS)
			break;

		if (s.j == M) {
			runcand rc2;

			memset(&rc2, 0, sizeof(rc2));
			/* The unchanged reading has to stay in the list: a
			 * joint assignment may well want to leave this
			 * stretch exactly as the decoder read it. */
			if (run_decode(X, arena, s.node, first, last, &rc2) == 0) {
				rc2.cost = s.g;
				if (rs->n % 256 == 0) {
					runcand *nl = realloc(rs->cands,
					        (size_t)(rs->n + 256) * sizeof(*nl));
					if (!nl)
						goto out;
					rs->cands = nl;
				}
				rs->cands[rs->n++] = rc2;
			}
			continue;
		}

		idx = first + s.j;

		#define PUSH(kind_, a_, b_, nj_, ns_, g_)                      \
		do {                                                           \
			double hh = h[(size_t)(nj_) * DEV_SPAN + (ns_)];        \
			rstate t_;                                             \
			if (hh >= BIG) break;                                  \
			if (arena_n == arena_cap) {                            \
				rnode *na;                                     \
				arena_cap *= 2;                                \
				na = realloc(arena,                            \
				     (size_t)arena_cap * sizeof(*na));         \
				if (!na) goto out;                             \
				arena = na;                                    \
			}                                                      \
			arena[arena_n].parent = s.node;                        \
			arena[arena_n].kind = (kind_);                         \
			arena[arena_n].k1 = (int8_t)(a_);                      \
			arena[arena_n].k2 = (int8_t)(b_);                      \
			arena[arena_n].j = idx;                                \
			t_.g = (g_);                                           \
			t_.f = (g_) + hh;                                      \
			t_.node = arena_n++;                                   \
			t_.j = (nj_);                                          \
			t_.s = (ns_);                                          \
			if (rpush(&H, t_) < 0) goto out;                       \
		} while (0)

		for (k = MIN_BIN; k <= MAX_BIN; k++) {
			int ns = s.s + (k - X->iv[idx].gap);
			if (ns < 0 || ns >= DEV_SPAN)
				continue;
			PUSH(0, k, 0, s.j + 1, ns,
			     s.g + pos_cost(X, first + s.j + 1, ns - MAX_DEV));
		}
		{
			int k1, k2;
			for (k1 = MIN_BIN; k1 <= MAX_BIN; k1++)
			for (k2 = MIN_BIN; k2 <= MAX_BIN; k2++) {
				int ns = s.s + (k1 + k2 - X->iv[idx].gap);
				if (ns < 0 || ns >= DEV_SPAN)
					continue;
				PUSH(1, k1, k2, s.j + 1, ns,
				     s.g + pos_cost(X, first + s.j + 1,
				                    ns - MAX_DEV) + X->p_drop);
			}
		}
		if (s.j + 1 < M) {
			int pll2 = X->iv[idx].gap + X->iv[idx + 1].gap;

			for (k = MIN_BIN; k <= MAX_BIN; k++) {
				int ns = s.s + (k - pll2);
				if (ns < 0 || ns >= DEV_SPAN)
					continue;
				PUSH(2, k, 0, s.j + 2, ns,
				     s.g + pos_cost(X, first + s.j + 2,
				                    ns - MAX_DEV) + X->p_spur);
			}
		}
		#undef PUSH
	}

	if (rs->n > 1)
		qsort(rs->cands, (size_t)rs->n, sizeof(runcand), cmp_runcand);
	rc = 0;

out:
	free(arena);
	free(H.v);
	free(h);
	return rc;
}

/* ------------------------------------------------------------------ */
int dr_rebin_search(dr_view *v, const dr_options *opt, dr_repair_result *out)
{
	dr_options defopt;
	struct ctx X;
	runset *runs = NULL;
	int nruns = 0;
	uint8_t *dirty = NULL;
	uint16_t *masks = NULL;
	dr_candidate *found = NULL;
	int nfound = 0, fcap = 0;
	int i, j, rc = 0;
	long explored = 0;
	double period = 0.0;

	memset(&X, 0, sizeof(X));
	memset(out, 0, sizeof(*out));
	out->rebin = 1;

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}
	if (!v || !v->msg)
		return -1;

	if (v->encoding != DR_ENC_ISO_MFM) {
		snprintf(out->note, sizeof(out->note),
		         "re-binning currently understands MFM only");
		return 0;
	}
	if (!v->flux_available) {
		snprintf(out->note, sizeof(out->note),
		         "no flux timings in this image - nothing to re-bin");
		return 0;
	}

	X.v = v;
	X.stride = v->stride;
	X.p_drop = 6.0;
	X.p_spur = 6.0;
	X.niv = dr_intervals_collect(v, &X.iv, &period);
	if (X.niv < 0) {
		snprintf(out->note, sizeof(out->note),
		         "could not line the cells up with the pulse stream");
		return 0;
	}

	X.cum_meas = malloc((size_t)(X.niv + 2) * sizeof(double));
	X.cum_pll  = malloc((size_t)(X.niv + 2) * sizeof(double));
	if (!X.cum_meas || !X.cum_pll) {
		rc = -1;
		goto done;
	}

	dr_timing_fit(X.iv, X.niv, &X.t);
	for (i = 0; i < X.niv; i++)
		X.iv[i].adj = dr_timing_adjust(&X.t, i, X.iv[i].meas,
		        i > 0 ? X.iv[i - 1].gap : 3,
		        i + 1 < X.niv ? X.iv[i + 1].gap : 3);
	if (!X.t.valid) {
		snprintf(out->note, sizeof(out->note),
		         "could not fit a timing model to this track");
		goto done;
	}

	/* Positions, in cells, as measured and as the decoder read them. */
	X.cum_meas[0] = 0.0;
	X.cum_pll[0] = 0.0;
	for (i = 0; i < X.niv; i++) {
		double m = (X.iv[i].adj > 0.0) ? X.iv[i].adj : (double)X.iv[i].gap;

		X.cum_meas[i + 1] = X.cum_meas[i] + m;
		X.cum_pll[i + 1] = X.cum_pll[i] + (double)X.iv[i].gap;
	}
	X.cum_meas[X.niv + 1] = X.cum_meas[X.niv];
	X.cum_pll[X.niv + 1] = X.cum_pll[X.niv];

	masks = malloc((size_t)v->msg_bits * sizeof(uint16_t));
	dirty = calloc((size_t)X.niv, 1);
	if (!masks || !dirty) {
		rc = -1;
		goto done;
	}
	dr_crc16_bit_masks(v->msg_bits, masks);

	/* ---- which intervals does the timing not vouch for? ---------- */
	for (i = 0; i < X.niv; i++) {
		if (X.iv[i].meas <= 0.0)
			continue;
		if (X.iv[i].gap < MIN_BIN || X.iv[i].gap > MAX_BIN ||
		    dr_bin_cost(&X.t, i, X.iv[i].adj, X.iv[i].gap) > DIRTY_COST)
			dirty[i] = 1;
	}
	if (X.niv > 0) {
		uint8_t *d2 = malloc((size_t)X.niv);
		if (!d2) { rc = -1; goto done; }
		memcpy(d2, dirty, (size_t)X.niv);
		for (i = 0; i < X.niv; i++) {
			if (!d2[i])
				continue;
			for (j = i - DILATE; j <= i + DILATE; j++)
				if (j >= 0 && j < X.niv)
					dirty[j] = 1;
		}
		free(d2);
	}

	/* ---- group them into disturbed stretches --------------------- */
	for (i = 0; i < X.niv; ) {
		int first, last;

		if (!dirty[i]) { i++; continue; }
		first = i;
		while (i < X.niv && dirty[i])
			i++;
		last = i - 1;

		/* A stretch must sit between two transitions we trust. */
		if (first == 0 || last >= X.niv - 1)
			continue;

		runs = realloc(runs, (size_t)(nruns + 1) * sizeof(*runs));
		if (!runs) { rc = -1; goto done; }
		memset(&runs[nruns], 0, sizeof(runs[nruns]));
		runs[nruns].first = first;
		runs[nruns].last = last;
		nruns++;
	}

	out->ambiguous = 0;
	for (i = 0; i < nruns; i++)
		out->ambiguous += runs[i].last - runs[i].first + 1;

	DBG("%d disturbed stretch(es), %d intervals, sigma %.4f\n",
	    nruns, out->ambiguous, X.t.sigma);

	if (!nruns) {
		snprintf(out->note, sizeof(out->note),
		         "the timings vouch for every interval in this sector - "
		         "the damage is not a mis-read");
		goto done;
	}
	if (out->ambiguous > opt->max_ambiguous * 8) {
		snprintf(out->note, sizeof(out->note),
		         "%d disturbed interval(s) across %d stretch(es) - too "
		         "much to search (raise --max-ambiguous)",
		         out->ambiguous, nruns);
		goto done;
	}

	for (i = 0; i < X.niv; i++)
		if (dirty[i])
			out->current_cost += dr_bin_cost(&X.t, i, X.iv[i].adj,
			                                 X.iv[i].gap);

	/* ---- enumerate re-readings of each stretch ------------------- */
	for (i = 0; i < nruns; i++) {
		if (run_search(&X, runs[i].first, runs[i].last,
		               opt->max_explore / (nruns ? nruns : 1),
		               &runs[i]) < 0) {
			rc = -1;
			goto done;
		}
		DBG("stretch %d: intervals %d..%d (cells %d..%d, bytes %d..%d) "
		    "-> %d re-reading(s), cheapest %.1f nats (%d re-bins, "
		    "%d bits), current %.1f nats\n",
		    i, runs[i].first, runs[i].last,
		    X.iv[runs[i].first].cell, X.iv[runs[i].last].cell,
		    X.iv[runs[i].first].cell / X.stride,
		    X.iv[runs[i].last].cell / X.stride,
		    runs[i].n,
		    runs[i].n ? runs[i].cands[0].cost : -1.0,
		    runs[i].n ? runs[i].cands[0].rebins : -1,
		    runs[i].n ? runs[i].cands[0].nbits : -1,
		    ({ double cc = 0; int q; for (q = runs[i].first;
		       q <= runs[i].last; q++)
		         cc += dr_bin_cost(&X.t, q, X.iv[q].adj, X.iv[q].gap);
		       cc; }));
		out->pinned_moves += runs[i].n ? runs[i].cands[0].rebins : 0;
	}

	/* ---- combine the stretches and test the CRC ------------------ */
	/*
	 * Each disturbed stretch has its own list of re-readings, cheapest
	 * first. The joint reading picks one from each, and its cost is the
	 * sum - so the joint readings are enumerated in cost order by a
	 * k-way merge: start with the cheapest from every stretch, and
	 * repeatedly step one stretch down its own list.
	 *
	 * Successors only ever advance a stretch at or after the last one
	 * advanced, which reaches every combination exactly once and saves
	 * keeping a visited set.
	 */
	{
		typedef struct {
			double   g;
			int      last;
			uint16_t idx[MAX_RUNS];
		} jstate;
		jstate *heap = NULL;
		int hn = 0, hcap = 0;

		#define JPUSH(st_)                                             \
		do {                                                           \
			int i_;                                                \
			jstate tmp_;                                           \
			if (hn == hcap) {                                      \
				int nc_ = hcap ? hcap * 2 : 1024;              \
				jstate *nh_ = realloc(heap,                    \
				        (size_t)nc_ * sizeof(*nh_));           \
				if (!nh_) { free(heap); rc = -1; goto done; }  \
				heap = nh_; hcap = nc_;                        \
			}                                                      \
			i_ = hn++;                                             \
			heap[i_] = (st_);                                      \
			while (i_ > 0) {                                       \
				int p_ = (i_ - 1) / 2;                         \
				if (heap[p_].g <= heap[i_].g) break;           \
				tmp_ = heap[p_]; heap[p_] = heap[i_];          \
				heap[i_] = tmp_; i_ = p_;                      \
			}                                                      \
		} while (0)

		if (nruns > MAX_RUNS) {
			snprintf(out->note, sizeof(out->note),
			         "%d disturbed stretches - more than the merge "
			         "can combine", nruns);
			goto done;
		}
		for (i = 0; i < nruns; i++) {
			if (!runs[i].n) {
				snprintf(out->note, sizeof(out->note),
				         "one disturbed stretch has no legal "
				         "re-reading at all");
				goto done;
			}
			if (runs[i].n > opt->rebin_width)
				runs[i].n = opt->rebin_width;
		}

		{
			jstate st;
			memset(&st, 0, sizeof(st));
			st.last = 0;
			st.g = 0.0;
			for (i = 0; i < nruns; i++)
				st.g += runs[i].cands[0].cost;
			out->floor_cost = st.g;
			JPUSH(st);
		}

		while (hn && explored < opt->max_explore &&
		       nfound < opt->max_results) {
			jstate cur = heap[0];
			uint16_t syn;
			int bits[DR_MAX_WEIGHT], nb = 0, ok = 1, re = 0;

			/* pop */
			heap[0] = heap[--hn];
			{
				int i_ = 0;
				while (1) {
					int l = 2 * i_ + 1, r = l + 1, m = i_;
					jstate t_;
					if (l < hn && heap[l].g < heap[m].g) m = l;
					if (r < hn && heap[r].g < heap[m].g) m = r;
					if (m == i_) break;
					t_ = heap[m]; heap[m] = heap[i_];
					heap[i_] = t_; i_ = m;
				}
			}

			explored++;
			syn = v->syndrome;
			for (i = 0; i < nruns && ok; i++) {
				runcand *rcp = &runs[i].cands[cur.idx[i]];

				re += rcp->rebins;
				for (j = 0; j < rcp->nbits; j++) {
					if (nb >= DR_MAX_WEIGHT) { ok = 0; break; }
					bits[nb++] = rcp->bits[j];
					syn ^= masks[rcp->bits[j]];
				}
			}

			if (explored == 1 && ok && !syn)
				out->floor_valid = 1;

			if (ok && nb && !syn) {
				if (nfound == fcap) {
					int nc = fcap ? fcap * 2 : 32;
					dr_candidate *nl = realloc(found,
					        (size_t)nc * sizeof(*nl));
					if (!nl) { free(heap); rc = -1; goto done; }
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
					cd->rebins = re;
					cd->flux_cost = cur.g;
					cd->log_likelihood = -cur.g;
				}
			}

			for (i = cur.last; i < nruns; i++) {
				jstate nx = cur;

				if (cur.idx[i] + 1 >= runs[i].n)
					continue;
				nx.g = cur.g - runs[i].cands[cur.idx[i]].cost
				             + runs[i].cands[cur.idx[i] + 1].cost;
				nx.idx[i] = (uint16_t)(cur.idx[i] + 1);
				nx.last = i;
				JPUSH(nx);
			}
		}

		if (hn)
			out->truncated = 1;
		free(heap);
		#undef JPUSH
	}

	/* How many message bits are genuinely still in doubt? That is what
	 * decides whether a 16-bit CRC can settle this sector at all. */
	{
		uint8_t *seen = calloc((size_t)v->msg_bits, 1);

		if (seen) {
			for (i = 0; i < nruns; i++)
				for (j = 0; j < runs[i].n; j++) {
					int q;
					for (q = 0; q < runs[i].cands[j].nbits; q++) {
						int bit = runs[i].cands[j].bits[q];
						if (bit >= 0 && bit < v->msg_bits)
							seen[bit] = 1;
					}
				}
			for (i = 0; i < v->msg_bits; i++)
				out->uncertain_bits += seen[i];
			free(seen);
		}
	}

	out->explored = explored;

	if (nfound > 1) {
		/* cheapest first */
		for (i = 1; i < nfound; i++) {
			dr_candidate key = found[i];
			j = i - 1;
			while (j >= 0 && found[j].flux_cost > key.flux_cost) {
				found[j + 1] = found[j];
				j--;
			}
			found[j + 1] = key;
		}
	}
	if (nfound > 0) {
		double best = found[0].log_likelihood;
		for (i = 0; i < nfound; i++)
			found[i].rel_likelihood =
			        exp(found[i].log_likelihood - best);
	}

	if (!out->note[0])
		snprintf(out->note, sizeof(out->note),
		         "timing model: cell = %.3f + %.3f x bin, sigma %.3f "
		         "(%d intervals)", X.t.a, X.t.b, X.t.sigma, X.t.n);

	out->list = found;
	out->count = nfound;
	found = NULL;

done:
	v->fit_a = X.t.a;
	v->fit_b = X.t.b;
	v->fit_sigma = X.t.sigma;
	v->fit_n = X.t.n;
	if (period > 0.0)
		v->period = period;

	for (i = 0; i < nruns; i++)
		free(runs[i].cands);
	free(runs);
	free(found);
	free(dirty);
	free(masks);
	free(X.cum_meas);
	free(X.cum_pll);
	free(X.iv);
	return rc;
}
