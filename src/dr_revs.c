/*
 * DisketteRecover - combine the revolutions a flux dump actually contains.
 *
 * A KryoFlux or SuperCard Pro dump holds the same track read several
 * times round - five, in the dumps this was written against. libhxcfe
 * decodes each one, scores them by how many sectors came out with a good
 * CRC, and hands back the winner; everything the other passes saw is
 * thrown away. For a sector that no pass got right, that is exactly the
 * evidence worth keeping.
 *
 * What the extra passes are good for is not majority voting on the
 * decoded bytes. Measured on these disks, two revolutions of the same
 * sector agree on the length of every flux interval to within half a
 * tick out of ~24 - the reversals really are in the same place each time,
 * because they are magnetised into the oxide. Voting on bytes would
 * throw that away and vote on the PLL's opinion instead, which is the
 * part that is unreliable: on a damaged sector the same flux decodes to
 * a different byte string on every pass.
 *
 * So the combining happens here, on the flux, before anything is
 * decoded:
 *
 *   - align each revolution to the reference with a banded alignment
 *     that can insert and delete reversals, because that is exactly the
 *     failure being measured;
 *   - average the intervals that all the passes agree on, which divides
 *     the read noise by the root of the number of passes;
 *   - and count, per interval, how many passes saw a reversal there at
 *     all. Five out of five is a reversal in the oxide. Two out of five
 *     is the amplifier firing on noise, which is what an unmagnetised
 *     patch of media looks like from the outside.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dr_internal.h"

#define DBG(...) do { if (getenv("DR_DEBUG")) \
	fprintf(stderr, "dr_revs: " __VA_ARGS__); } while (0)

#define MAX_REVS    16
#define PROBE       256      /* intervals used to locate the sector     */
#define SEARCH      1024     /* pulses either side of the estimate      */
#define BAND        12       /* how far the alignment may wander        */
#define BIG         1e17

/* Cost of an inserted or deleted reversal, in units of the squared
 * timing residual it has to beat. A reversal that one pass sees and
 * another does not is a real event and should not be explained away by
 * timing noise, but it is far from free: at 25 it takes a 5-sigma
 * mismatch to buy one. */
#define INDEL_COST  25.0

struct rev {
	uint32_t start;      /* first pulse of this revolution            */
	uint32_t end;        /* one past the last                         */
};

/* ------------------------------------------------------------------ */
/* Locating the sector in another revolution                           */
/* ------------------------------------------------------------------ */
/* Mean absolute difference between the reference probe and the
 * candidate starting at `at`, or a large number if it does not fit. */
static double probe_cost(const uint32_t *stream, uint32_t np,
                         const uint32_t *ref, int m, uint32_t at)
{
	double s = 0.0;
	int i;

	if (at + (uint32_t)m > np)
		return BIG;
	for (i = 0; i < m; i++)
		s += fabs((double)stream[at + i] - (double)ref[i]);
	return s / m;
}

static int32_t find_sector(const uint32_t *stream, uint32_t np,
                           const uint32_t *ref, int m,
                           uint32_t est, uint32_t lo, uint32_t hi,
                           double *cost_out)
{
	double best = BIG;
	int32_t at = -1;
	uint32_t s, from, to;

	from = (est > lo + SEARCH) ? est - SEARCH : lo;
	to   = (est + SEARCH < hi) ? est + SEARCH : hi;

	for (s = from; s <= to; s++) {
		double c = probe_cost(stream, np, ref, m, s);
		if (c < best) {
			best = c;
			at = (int32_t)s;
		}
	}
	if (cost_out)
		*cost_out = best;
	return at;
}

/* ------------------------------------------------------------------ */
/* Alignment                                                           */
/* ------------------------------------------------------------------ */
/*
 * Align the reference intervals against one revolution's, allowing a
 * reference interval to correspond to two of theirs (the reference
 * missed a reversal that pass saw) or two reference intervals to one of
 * theirs (the reference has one that pass missed).
 *
 * A banded dynamic program: the two sequences describe the same piece of
 * track, so the alignment never strays far from the diagonal, and the
 * band keeps this linear in the sector length.
 */
enum { OP_MATCH = 0, OP_SPLIT = 1, OP_MERGE = 2 };

struct align_out {
	int8_t  *op;         /* [n] what happened to reference interval i */
	int32_t *peer;       /* [n] the candidate interval it matched     */
	int      matched;
	int      splits;
	int      merges;
	double   resid;      /* mean |difference| over matched intervals  */
};

static int align_rev(const uint32_t *ref, int n,
                     const uint32_t *cand, int m,
                     double sigma, struct align_out *out)
{
	const int W = 2 * BAND + 1;
	double *dp = NULL;
	int8_t *bp = NULL;
	int i, o, rc = -1;
	double inv = 1.0 / (sigma * sigma);
	double sum = 0.0;
	int cnt = 0;

	out->op = calloc((size_t)n, sizeof(*out->op));
	out->peer = malloc((size_t)n * sizeof(*out->peer));
	dp = malloc((size_t)(n + 2) * W * sizeof(*dp));
	bp = malloc((size_t)(n + 2) * W * sizeof(*bp));
	if (!out->op || !out->peer || !dp || !bp)
		goto done;

	for (i = 0; i < (n + 2) * W; i++)
		dp[i] = BIG;
	memset(bp, 0, (size_t)(n + 2) * W);
	dp[0 * W + BAND] = 0.0;

	for (i = 0; i < n; i++) {
		for (o = 0; o < W; o++) {
			double c = dp[i * W + o], d, nc;
			int j = i + o - BAND;

			if (c >= BIG)
				continue;
			if (j < 0 || j > m)
				continue;

			if (j < m) {                       /* match */
				d = (double)ref[i] - (double)cand[j];
				nc = c + d * d * inv;
				if (nc < dp[(i + 1) * W + o]) {
					dp[(i + 1) * W + o] = nc;
					bp[(i + 1) * W + o] = OP_MATCH;
				}
			}
			if (j + 1 < m && o + 1 < W) {      /* ref missed one */
				d = (double)ref[i] - (double)cand[j]
				    - (double)cand[j + 1];
				nc = c + d * d * inv + INDEL_COST;
				if (nc < dp[(i + 1) * W + o + 1]) {
					dp[(i + 1) * W + o + 1] = nc;
					bp[(i + 1) * W + o + 1] = OP_SPLIT;
				}
			}
			if (i + 1 < n && j < m && o > 0) { /* pass missed one */
				d = (double)ref[i] + (double)ref[i + 1]
				    - (double)cand[j];
				nc = c + d * d * inv + INDEL_COST;
				if (nc < dp[(i + 2) * W + o - 1]) {
					dp[(i + 2) * W + o - 1] = nc;
					bp[(i + 2) * W + o - 1] = OP_MERGE;
				}
			}
		}
	}

	/* Finish wherever the candidate ran out at the same time we did. */
	{
		double best = BIG;
		int bo = -1;

		for (o = 0; o < W; o++) {
			int j = n + o - BAND;
			if (j >= 0 && j <= m && dp[n * W + o] < best) {
				best = dp[n * W + o];
				bo = o;
			}
		}
		if (bo < 0)
			goto done;

		i = n;
		o = bo;
		while (i > 0) {
			int8_t k = bp[i * W + o];
			int j = i + o - BAND;

			if (k == OP_MATCH) {
				out->op[i - 1] = OP_MATCH;
				out->peer[i - 1] = j - 1;
				i--;
			} else if (k == OP_SPLIT) {
				out->op[i - 1] = OP_SPLIT;
				out->peer[i - 1] = j - 2;
				i--;
				o--;
			} else {
				out->op[i - 1] = OP_MERGE;
				out->op[i - 2] = OP_MERGE;
				out->peer[i - 1] = j - 1;
				out->peer[i - 2] = -1;
				i -= 2;
				o++;
			}
		}
	}

	out->matched = out->splits = out->merges = 0;
	for (i = 0; i < n; i++) {
		if (out->op[i] == OP_MATCH) {
			int j = out->peer[i];
			out->matched++;
			if (j >= 0 && j < m) {
				sum += fabs((double)ref[i] - (double)cand[j]);
				cnt++;
			}
		} else if (out->op[i] == OP_SPLIT) {
			out->splits++;
		} else if (out->peer[i] >= 0) {
			out->merges++;
		}
	}
	out->resid = cnt ? sum / cnt : 0.0;
	rc = 0;
done:
	free(dp);
	free(bp);
	if (rc < 0) {
		free(out->op);
		free(out->peer);
		out->op = NULL;
		out->peer = NULL;
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */
void dr_revs_free(dr_revmap *r)
{
	if (!r)
		return;
	free(r->ticks);
	free(r->votes);
	free(r->same);
	free(r->extra);
	free(r->extra_at);
	free(r);
}

dr_revmap *dr_revs_build(dr_view *v)
{
	HXCFE_SIDE *side = (HXCFE_SIDE *)v->side;
	dr_flux_map *fx = (dr_flux_map *)v->flux;
	HXCFE_TRKSTREAM *std;
	struct rev rev[MAX_REVS];
	dr_revmap *out = NULL;
	uint32_t *ref = NULL;
	double *acc = NULL;
	uint32_t p_first = 0;
	double period;
	int nrev = 0, n = 0, i, r, ref_rev = -1, probe;

	if (!side || !fx || !fx->valid || !side->stream_dump)
		return NULL;
	period = dr_cell_period(v);
	if (period <= 1.0)
		return NULL;
	std = side->stream_dump;
	if (std->nb_of_index < 3 || !std->channels[0].stream)
		return NULL;      /* fewer than two complete revolutions */

	/* ---- revolution table ---------------------------------------- */
	for (i = 0; i + 1 < (int)std->nb_of_index && nrev < MAX_REVS; i++) {
		uint32_t a = std->index_evt_tab[i].dump_offset;
		uint32_t b = std->index_evt_tab[i + 1].dump_offset;

		if (b <= a || b > std->channels[0].nb_of_pulses)
			continue;
		rev[nrev].start = a;
		rev[nrev].end = b;
		nrev++;
	}
	if (nrev < 2)
		return NULL;

	/*
	 * ---- the reference: the pulses this sector was read from ------
	 *
	 * Taken as a contiguous run of the dump rather than by walking the
	 * cells, because the cell-to-pulse map is allowed to re-synchronise
	 * where the decoder gained or lost a reversal - and a sector whose
	 * decode slipped is precisely the one being rescued. The pulses
	 * themselves never slip: they are what was measured.
	 */
	{
		uint32_t lo = 0xFFFFFFFFu, hi = 0;

		for (i = 0; i < v->ncells; i++) {
			uint32_t p;

			if (!v->cells[i].state)
				continue;
			p = fx->pulse_of_cell[dr_wrap(side->tracklen,
			                              v->base_cell + i)];
			if (p == 0xFFFFFFFFu || p >= fx->nb_pulses)
				continue;
			if (p < lo) lo = p;
			if (p > hi) hi = p;
		}
		if (lo > hi || hi - lo < 512)
			return NULL;
		p_first = lo;
		n = (int)(hi - lo) + 1;
		ref = malloc((size_t)n * sizeof(*ref));
		if (!ref)
			return NULL;
		for (i = 0; i < n; i++)
			ref[i] = fx->stream[p_first + i];
	}

	for (r = 0; r < nrev; r++)
		if (p_first >= rev[r].start && p_first < rev[r].end)
			ref_rev = r;
	if (ref_rev < 0)
		goto fail;

	probe = (n < PROBE) ? n : PROBE;

	out = calloc(1, sizeof(*out));
	acc = calloc((size_t)n, sizeof(*acc));
	if (!out || !acc)
		goto fail;
	out->ticks = malloc((size_t)n * sizeof(*out->ticks));
	out->votes = calloc((size_t)n, sizeof(*out->votes));
	out->same = calloc((size_t)n, sizeof(*out->same));
	out->extra = calloc((size_t)n, sizeof(*out->extra));
	out->extra_at = calloc((size_t)n, sizeof(*out->extra_at));
	if (!out->ticks || !out->votes || !out->same || !out->extra ||
	    !out->extra_at)
		goto fail;

	out->nrev = nrev;
	out->n = n;
	out->p_first = p_first;
	out->nused = 1;
	for (i = 0; i < n; i++) {
		acc[i] = (double)ref[i];
		out->votes[i] = 1;
		out->same[i] = 1;
	}

	/* ---- fold in every other revolution -------------------------- */
	for (r = 0; r < nrev; r++) {
		struct align_out al;
		double cost = 0.0;
		uint32_t est;
		int32_t at;

		if (r == ref_rev)
			continue;

		/*
		 * The index pulse is the same angular position every time
		 * round, so the sector sits at about the same offset into
		 * each revolution. "About" is not good enough to average
		 * on - the index is only located to the nearest pulse and
		 * the passes differ by a few dozen - so the estimate only
		 * seeds a search for the real thing.
		 */
		est = rev[r].start + (p_first - rev[ref_rev].start);
		at = find_sector(fx->stream, fx->nb_pulses, ref, probe,
		                 est, rev[r].start, rev[r].end, &cost);
		/*
		 * A tick is not a fixed amount of anything - it is whatever
		 * the dump's sample clock was - so the test has to be in
		 * cell periods. A fifth of a cell of mean disagreement is
		 * already far worse than any pass of the same track should
		 * manage, and means this is not the same piece of track.
		 */
		if (at < 0 || cost > 0.20 * period) {
			DBG("rev %d: no match at %d (cost %.2f ticks = "
			    "%.3f cell)\n", r, at, cost, cost / period);
			continue;
		}

		memset(&al, 0, sizeof(al));
		{
			int m = (int)(rev[r].end - (uint32_t)at);
			if (m > n + BAND)
				m = n + BAND;
			if (m < n - BAND)
				continue;
			/* One twentieth of a cell: generous next to the
			 * hundredth that passes of the same track actually
			 * differ by, tight enough that a split has to be
			 * real to be worth 25 of these. */
			if (align_rev(ref, n, fx->stream + at, m,
			              0.05 * period, &al) < 0)
				continue;
		}

		for (i = 0; i < n; i++) {
			int j = al.peer[i];

			if (al.op[i] == OP_MATCH && j >= 0) {
				acc[i] += (double)fx->stream[at + j];
				out->same[i]++;
				out->votes[i]++;
			} else if (al.op[i] == OP_SPLIT && j >= 0) {
				/*
				 * This pass saw a reversal inside what the
				 * reference read as one interval. It still
				 * agrees about the one at the end, so the
				 * end still gets its vote; remember where
				 * the extra one went so a repair can put it
				 * back.
				 */
				if (!out->extra[i])
					out->extra_at[i] =
					        fx->stream[at + j];
				out->extra[i]++;
				out->votes[i]++;
			}
			/*
			 * OP_MERGE: this pass ran two of the reference's
			 * intervals together, so it has no reversal where the
			 * reference put one. No vote - that is the whole
			 * point of counting them.
			 */
		}

		out->resid += al.resid;
		out->splits += al.splits;
		out->merges += al.merges;
		out->nused++;

		DBG("rev %d: at %d cost %.2f match %d split %d merge %d "
		    "resid %.2f\n", r, at, cost, al.matched, al.splits,
		    al.merges, al.resid);

		free(al.op);
		free(al.peer);
	}

	if (out->nused > 1)
		out->resid /= (out->nused - 1);

	for (i = 0; i < n; i++) {
		out->ticks[i] = (uint32_t)(acc[i] / out->same[i] + 0.5);

		/*
		 * Where the pass libhxcfe happened to pick is the odd one
		 * out, say so. A reversal it read that most of the others
		 * did not, or one most of them saw that it missed, is not a
		 * search problem - it is a correction with the rest of the
		 * dump behind it.
		 */
		if (out->nused >= 3) {
			if (out->votes[i] * 2 <= out->nused)
				out->outvoted_extra++;
			else if (out->extra[i] * 2 > out->nused)
				out->outvoted_missing++;
		}
	}

	DBG("consensus: %d intervals, %d outvoted reversals, %d outvoted gaps\n",
	    n, out->outvoted_extra, out->outvoted_missing);

	free(acc);
	free(ref);
	return out;

fail:
	free(acc);
	free(ref);
	dr_revs_free(out);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* The revolutions engine                                              */
/* ------------------------------------------------------------------ */
/*
 * Read the sector again, but let every pass vote.
 *
 * For each reversal the passes did not unanimously agree on, the
 * majority reading is taken as the starting point - that alone is a
 * different, better-evidenced reading of the sector than the one pass
 * libhxcfe picked. From there the search reverses the least confident of
 * those votes, one and two at a time, because a 3-2 vote is barely a
 * vote at all and the CRC is a better arbiter than a single extra pass.
 *
 * Adding or removing a reversal splits or merges an interval, so the
 * number of cells is unchanged either way and the rest of the sector
 * does not move. That is what makes this expressible as a set of bit
 * flips rather than a re-decode.
 */
struct contested {
	int    cell;         /* cell the reversal sits on (or would)      */
	int    add;          /* 1 = put one in, 0 = take one out          */
	double margin;       /* how lopsided the vote was, 0..1           */
};

static int cmp_contested(const void *a, const void *b)
{
	const struct contested *x = a, *y = b;

	if (x->margin < y->margin) return -1;
	if (x->margin > y->margin) return 1;
	return 0;
}

/* Decode `cells` into a message and record where it differs from the
 * reading we started with. Returns the number of differing bits, or -1
 * if there are more than a candidate can hold. */
static int cells_to_bits(const dr_view *v, const uint8_t *cells,
                         uint8_t *msg, int *bits)
{
	int b, k, n = 0;

	for (b = 0; b < v->msg_len; b++) {
		uint8_t val = 0, diff;

		for (k = 0; k < 8; k++) {
			int cc, dc, o = b * v->stride;

			dr_bit_cells(v->encoding, o, k, &cc, &dc);
			val = (uint8_t)(val << 1);
			if (v->encoding == DR_ENC_ISO_FM) {
				if (cc + 0 >= 0 && cells[dc])
					val |= 1;
			} else if (!cells[cc] && cells[dc]) {
				val |= 1;
			}
		}
		msg[b] = val;

		diff = (uint8_t)(val ^ v->msg[b]);
		for (k = 0; k < 8 && diff; k++) {
			if (!(diff & (0x80 >> k)))
				continue;
			if (n >= DR_MAX_WEIGHT)
				return -1;
			bits[n++] = b * 8 + k;
		}
	}
	return n;
}

/*
 * The reading the passes vote for, as opposed to the one libhxcfe picked.
 *
 * Fills `msg` (v->msg_len bytes) with the majority reading and returns
 * the number of reversals the passes did not agree on, or -1 if there is
 * nothing to combine. `contested` receives the full contested list when
 * a caller wants to search around it.
 */
static int majority_reading(dr_view *v, uint8_t *base, uint8_t *msg,
                            struct contested *ct, int *nct_out,
                            int *majority_out)
{
	dr_revmap *rm = (dr_revmap *)v->revs;
	dr_interval *iv = NULL;
	int bits[DR_MAX_WEIGHT];
	int niv, i, j, nct = 0, majority = 0;
	double period = 0.0;

	niv = dr_intervals_collect(v, &iv, &period);
	if (niv <= 0 || period <= 0.0) {
		free(iv);
		return -1;
	}

	for (i = 0; i < v->ncells; i++)
		base[i] = (uint8_t)v->cells[i].state;

	for (j = 0; j < niv; j++) {
		uint32_t k;
		int seen, ext;

		if (iv[j].pulse < rm->p_first)
			continue;
		k = iv[j].pulse - rm->p_first;
		if ((int)k >= rm->n)
			continue;

		seen = rm->votes[k];
		ext = rm->extra[k];

		if (seen < rm->nused) {
			/* Not every pass saw the reversal this interval
			 * ends on. */
			if (ct) {
				ct[nct].cell = iv[j].cell;
				ct[nct].add = 0;
				ct[nct].margin =
				        fabs(2.0 * seen / rm->nused - 1.0);
			}
			if (seen * 2 <= rm->nused) {
				base[iv[j].cell] = 0;
				majority++;
			}
			nct++;
		}
		if (ext > 0) {
			/* Some passes saw one inside it. */
			int off = (int)((double)rm->extra_at[k] / period + 0.5);
			int at = iv[j].cell - iv[j].gap + off;

			if (off >= 1 && at > 0 && at < v->ncells &&
			    at != iv[j].cell && !base[at]) {
				if (ct) {
					ct[nct].cell = at;
					ct[nct].add = 1;
					ct[nct].margin =
					      fabs(2.0 * ext / rm->nused - 1.0);
				}
				if (ext * 2 > rm->nused) {
					base[at] = 1;
					majority++;
				}
				nct++;
			}
		}
	}

	free(iv);
	if (nct_out) *nct_out = nct;
	if (majority_out) *majority_out = majority;
	if (msg)
		cells_to_bits(v, base, msg, bits);
	return nct;
}

int dr_revs_reading(dr_view *v, uint8_t *msg, int *contested, int *majority)
{
	dr_revmap *rm = (dr_revmap *)v->revs;
	uint8_t *base;
	int rc;

	if (!v || !v->msg || !rm || rm->nused < 2)
		return -1;
	base = malloc((size_t)v->ncells);
	if (!base)
		return -1;
	rc = majority_reading(v, base, msg, NULL, contested, majority);
	free(base);
	return rc;
}

int dr_revs_search(dr_view *v, const dr_options *opt, dr_repair_result *out)
{
	dr_revmap *rm = (dr_revmap *)v->revs;
	dr_options defopt;
	struct contested *ct = NULL;
	uint8_t *base = NULL, *cells = NULL, *msg = NULL;
	dr_candidate *found = NULL;
	int bits[DR_MAX_WEIGHT];
	int nct = 0, i, nfound = 0, fcap = 0, rc = 0, majority = 0;
	long explored = 0;

	memset(out, 0, sizeof(*out));
	out->revs = 1;

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}
	if (!v || !v->msg || !rm || rm->nused < 2)
		return -1;

	/* Two entries per interval is the most the contested list can
	 * need: the reversal an interval ends on, and one inside it. */
	ct = malloc((size_t)v->ncells * 2 * sizeof(*ct));
	base = malloc((size_t)v->ncells);
	cells = malloc((size_t)v->ncells);
	msg = malloc((size_t)v->msg_len);
	if (!ct || !base || !cells || !msg) {
		rc = -1;
		goto done;
	}

	if (majority_reading(v, base, NULL, ct, &nct, &majority) < 0) {
		rc = -1;
		goto done;
	}

	out->contested = nct;
	out->majority = majority;

	/*
	 * Is the CRC itself in doubt?
	 *
	 * The two CRC bytes are part of the sector like any other and
	 * nothing protects them, so ask the same question of them as of
	 * any other byte: do the passes read them the same way? Not by
	 * looking for contested reversals underneath them - a reversal
	 * gained or lost anywhere earlier shifts the cell phase and
	 * changes them from a distance - but by decoding the majority
	 * reading and comparing.
	 *
	 * When they differ, the number the search is matching against is a
	 * guess, every "CRC-valid" reading it returns matches a target
	 * that may never have been on the disk, and the usual arithmetic -
	 * one reading in 65536 passes by chance - does not apply. Better
	 * to say so than to hand back a confident wrong answer.
	 */
	{
		int bits2[DR_MAX_WEIGHT];

		if (cells_to_bits(v, base, msg, bits2) >= 0 &&
		    (msg[v->msg_len - 2] != v->msg[v->msg_len - 2] ||
		     msg[v->msg_len - 1] != v->msg[v->msg_len - 1]))
			out->crc_contested = 1;
	}

	snprintf(out->note, sizeof(out->note),
	         "%d of %d pass(es); %d reversal(s) not unanimous, %d of them "
	         "read differently by the majority%s",
	         rm->nused, rm->nrev, nct, majority,
	         out->crc_contested ? " - and the stored CRC is one of the "
	                              "bytes they disagree about" : "");

	if (!nct) {
		rc = 0;
		goto done;
	}

	/* Least confident first - those are the ones worth overruling. */
	qsort(ct, (size_t)nct, sizeof(*ct), cmp_contested);

	/* ---- the majority reading, then one and two votes reversed --- */
	{
		int limit = nct < opt->max_pool ? nct : opt->max_pool;
		int pass;

		for (pass = 0; pass <= 2; pass++) {
			int a, b;

			for (a = (pass ? 0 : -1); a < (pass ? limit : 0); a++) {
				for (b = (pass == 2 ? a + 1 : -1);
				     b < (pass == 2 ? limit : 0); b++) {
					int nb;

					memcpy(cells, base, (size_t)v->ncells);
					if (a >= 0)
						cells[ct[a].cell] =
						        (uint8_t)!cells[ct[a].cell];
					if (b >= 0)
						cells[ct[b].cell] =
						        (uint8_t)!cells[ct[b].cell];
					explored++;

					nb = cells_to_bits(v, cells, msg, bits);
					if (nb < 0 || dr_crc16(msg, v->msg_len))
						continue;
					if (nb == 0)
						continue;

					if (nfound == fcap) {
						int nc = fcap ? fcap * 2 : 16;
						dr_candidate *nl =
						    realloc(found,
						      (size_t)nc * sizeof(*nl));
						if (!nl) { rc = -1; goto done; }
						found = nl;
						fcap = nc;
					}
					memset(&found[nfound], 0,
					       sizeof(found[nfound]));
					found[nfound].weight = nb;
					for (i = 0; i < nb; i++) {
						found[nfound].bits[i] = bits[i];
						found[nfound].before[i] =
						  (uint8_t)((v->msg[bits[i] >> 3]
						    >> (7 - (bits[i] & 7))) & 1);
					}
					found[nfound].origin = DR_MODE_REVS;
					nfound++;
					if (nfound >= opt->max_results)
						goto enough;
				}
			}
		}
	}
enough:
	out->list = found;
	out->count = nfound;
	out->explored = explored;
	found = NULL;

done:
	free(found);
	free(ct);
	free(base);
	free(cells);
	free(msg);
	return rc;
}
