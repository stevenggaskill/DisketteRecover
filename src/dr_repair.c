/*
 * DisketteRecover - the corrector.
 *
 * The CRC is affine over GF(2): flipping message bit p always XORs a
 * fixed mask into the CRC. Repairing a sector therefore means finding a
 * set of bits whose masks XOR to the current syndrome.
 *
 * Weight 1 and 2 are searched exhaustively over the whole field - that
 * is only a few thousand hash lookups and it works even for images that
 * carry no timing information at all. Deeper weights enumerate the
 * least trustworthy bits (the ones the flux timing could not bin
 * confidently) and let the hash supply the remaining bit. The search
 * stops at the first weight that produces a CRC-valid reading, and each
 * survivor is re-verified by recomputing the CRC for real.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dr_internal.h"

/* ---- open-addressed table: 16-bit CRC mask -> message bit --------- */
#define HASH_BITS 17
#define HASH_SIZE (1 << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)

typedef struct {
	uint16_t key;
	int32_t  bit;    /* -1 = empty */
} hentry;

static void hput(hentry *h, uint16_t key, int bit)
{
	uint32_t i = ((uint32_t)key * 2654435761u) & HASH_MASK;

	while (h[i].bit >= 0) {
		if (h[i].key == key)
			return;          /* keep the first; candidates are verified */
		i = (i + 1) & HASH_MASK;
	}
	h[i].key = key;
	h[i].bit = bit;
}

static int hget(const hentry *h, uint16_t key)
{
	uint32_t i = ((uint32_t)key * 2654435761u) & HASH_MASK;

	while (h[i].bit >= 0) {
		if (h[i].key == key)
			return h[i].bit;
		i = (i + 1) & HASH_MASK;
	}
	return -1;
}

/* ---- helpers ------------------------------------------------------ */
typedef struct {
	int    bit;
	double perr;
	double cost;
} poolbit;

static int cmp_pool(const void *a, const void *b)
{
	const poolbit *x = a, *y = b;

	if (x->perr > y->perr) return -1;
	if (x->perr < y->perr) return 1;
	return x->bit - y->bit;
}

static int cmp_cand(const void *a, const void *b)
{
	const dr_candidate *x = a, *y = b;
	int i;

	if (x->weight != y->weight)
		return x->weight - y->weight;
	if (x->log_likelihood > y->log_likelihood) return -1;
	if (x->log_likelihood < y->log_likelihood) return 1;
	for (i = 0; i < x->weight; i++)
		if (x->bits[i] != y->bits[i])
			return x->bits[i] - y->bits[i];
	return 0;
}

static int same_bits(const dr_candidate *a, const dr_candidate *b)
{
	int i;

	if (a->weight != b->weight)
		return 0;
	for (i = 0; i < a->weight; i++)
		if (a->bits[i] != b->bits[i])
			return 0;
	return 1;
}

static int msg_bit(const uint8_t *msg, int p)
{
	return (msg[p >> 3] >> (7 - (p & 7))) & 1;
}

static double bit_cost(const dr_view *v, int p)
{
	double q = v->bit_perr[p];

	if (q <= 0.0)
		q = 1e-12;
	if (q >= 0.5)
		q = 0.499999;
	return log(q) - log(1.0 - q);
}

/* ---- candidate accumulation --------------------------------------- */
struct acc {
	dr_candidate *list;
	int           n, cap;
};

static int acc_add(struct acc *a, const dr_view *v, const int *bits, int w)
{
	dr_candidate cd;
	uint8_t *m;
	int i, j, t;

	memset(&cd, 0, sizeof(cd));
	cd.weight = w;
	for (i = 0; i < w; i++)
		cd.bits[i] = bits[i];

	/* keep the tuple sorted so duplicates collapse */
	for (i = 1; i < w; i++)
		for (j = i; j > 0 && cd.bits[j - 1] > cd.bits[j]; j--) {
			t = cd.bits[j - 1];
			cd.bits[j - 1] = cd.bits[j];
			cd.bits[j] = t;
		}

	for (i = 1; i < w; i++)
		if (cd.bits[i] == cd.bits[i - 1])
			return 0;               /* a bit flipped twice is a no-op */

	/* Verify for real rather than trusting the hash. */
	m = dr_candidate_message(v, &cd);
	if (!m)
		return -1;
	if (dr_crc16(m, v->msg_len) != 0) {
		free(m);
		return 0;
	}
	free(m);

	cd.log_likelihood = 0.0;
	for (i = 0; i < w; i++) {
		cd.log_likelihood += bit_cost(v, cd.bits[i]);
		cd.before[i] = (uint8_t)msg_bit(v->msg, cd.bits[i]);
	}

	if (a->n == a->cap) {
		int ncap = a->cap ? a->cap * 2 : 64;
		dr_candidate *nl = realloc(a->list, (size_t)ncap * sizeof(*nl));
		if (!nl)
			return -1;
		a->list = nl;
		a->cap = ncap;
	}
	a->list[a->n++] = cd;
	return 1;
}

/* ------------------------------------------------------------------ */
int dr_repair_search(dr_view *v, const dr_options *opt, dr_repair_result *out)
{
	dr_options defopt;
	uint16_t *masks = NULL;
	hentry   *hash = NULL;
	poolbit  *pool = NULL;
	struct acc acc;
	int npool = 0, i, w, rc = 0, maxw;
	double thr;

	memset(&acc, 0, sizeof(acc));
	memset(out, 0, sizeof(*out));

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}
	if (!v || !v->msg || v->msg_bits <= 0)
		return -1;
	if (v->syndrome == 0)
		return 0;                       /* already valid */

	masks = malloc((size_t)v->msg_bits * sizeof(uint16_t));
	hash  = malloc((size_t)HASH_SIZE * sizeof(hentry));
	pool  = malloc((size_t)v->msg_bits * sizeof(poolbit));
	if (!masks || !hash || !pool) {
		rc = -1;
		goto done;
	}

	dr_crc16_bit_masks(v->msg_bits, masks);

	for (i = 0; i < HASH_SIZE; i++)
		hash[i].bit = -1;
	for (i = v->first_bit; i < v->msg_bits; i++)
		hput(hash, masks[i], i);

	/* ---- the low-confidence pool, used for weight >= 3 ---------- */
	/*
	 * Media drops transitions far more readily than it invents them: a
	 * weak pulse simply fails to clear the detector's threshold,
	 * whereas a spurious one has to be manufactured out of noise.
	 * Every error whose truth we have been able to check - eleven on
	 * one disk, and the one unambiguous English correction on another -
	 * has been a 1 read as a 0. --restore-only takes that literally and
	 * considers nothing else, which also cuts a weight-3 search by
	 * roughly eightfold.
	 */
	thr = opt->good_threshold;
	for (i = v->first_bit; i < v->msg_bits; i++) {
		if (opt->restore_only && msg_bit(v->msg, i))
			continue;
		if (v->bit_perr[i] < thr)
			continue;
		pool[npool].bit = i;
		pool[npool].perr = v->bit_perr[i];
		npool++;
	}
	if (npool < 8) {
		/* No cell crossed the threshold - a plain sector image has no
		 * timing evidence at all - so fall back to every bit. */
		npool = 0;
		for (i = v->first_bit; i < v->msg_bits; i++) {
			pool[npool].bit = i;
			pool[npool].perr = v->bit_perr[i];
			npool++;
		}
	}
	qsort(pool, (size_t)npool, sizeof(poolbit), cmp_pool);
	if (npool > opt->max_pool)
		npool = opt->max_pool;
	for (i = 0; i < npool; i++)
		pool[i].cost = bit_cost(v, pool[i].bit);
	out->npool = npool;

	maxw = opt->max_weight;
	if (maxw > DR_MAX_WEIGHT)
		maxw = DR_MAX_WEIGHT;
	if (maxw < 1)
		maxw = 1;

	/* ---- search, shallowest weight first ------------------------ */
	for (w = 1; w <= maxw && acc.n == 0; w++) {
		int bits[DR_MAX_WEIGHT];

		out->searched_weight = w;

		if (w == 1) {
			int b = hget(hash, v->syndrome);
			if (b >= 0 && !(opt->restore_only && msg_bit(v->msg, b))) {
				bits[0] = b;
				if (acc_add(&acc, v, bits, 1) < 0) {
					rc = -1;
					goto done;
				}
			}
			continue;
		}

		if (w == 2) {
			/* Exhaustive over the whole field: cheap and it does not
			 * depend on any confidence model being available. */
			for (i = v->first_bit; i < v->msg_bits; i++) {
				int b;

				if (opt->restore_only && msg_bit(v->msg, i))
					continue;
				b = hget(hash,
				         (uint16_t)(v->syndrome ^ masks[i]));
				if (b > i &&
				    !(opt->restore_only && msg_bit(v->msg, b))) {
					bits[0] = i;
					bits[1] = b;
					if (acc_add(&acc, v, bits, 2) < 0) {
						rc = -1;
						goto done;
					}
				}
			}
			continue;
		}

		/* w >= 3: enumerate w-1 low-confidence bits, let the hash
		 * supply the last one. */
		if (npool < w - 1)
			continue;
		{
			int head = w - 1;
			int idx[DR_MAX_WEIGHT];
			int k;

			for (k = 0; k < head; k++)
				idx[k] = k;

			for (;;) {
				uint16_t axor = 0;
				int b;

				for (k = 0; k < head; k++)
					axor ^= masks[pool[idx[k]].bit];

				b = hget(hash, (uint16_t)(axor ^ v->syndrome));
				if (b >= 0) {
					for (k = 0; k < head; k++)
						bits[k] = pool[idx[k]].bit;
					bits[head] = b;
					if (acc_add(&acc, v, bits, w) < 0) {
						rc = -1;
						goto done;
					}
				}

				k = head - 1;
				while (k >= 0 && idx[k] == npool - (head - k))
					k--;
				if (k < 0)
					break;
				idx[k]++;
				for (++k; k < head; k++)
					idx[k] = idx[k - 1] + 1;
			}
		}
	}

	/* ---- sort, de-duplicate, normalise -------------------------- */
	if (acc.n > 1) {
		int j = 1;
		qsort(acc.list, (size_t)acc.n, sizeof(dr_candidate), cmp_cand);
		for (i = 1; i < acc.n; i++) {
			if (!same_bits(&acc.list[i], &acc.list[j - 1]))
				acc.list[j++] = acc.list[i];
		}
		acc.n = j;
	}

	if (acc.n > opt->max_results) {
		acc.n = opt->max_results;
		out->truncated = 1;
	}

	if (acc.n > 0) {
		double best = acc.list[0].log_likelihood;
		for (i = 0; i < acc.n; i++)
			acc.list[i].rel_likelihood =
			        exp(acc.list[i].log_likelihood - best);
	}

	out->list = acc.list;
	out->count = acc.n;
	acc.list = NULL;

done:
	free(acc.list);
	free(pool);
	free(hash);
	free(masks);
	return rc;
}

void dr_repair_free(dr_repair_result *r)
{
	if (!r)
		return;
	free(r->list);
	r->list = NULL;
	r->count = 0;
}

/*
 * One cell of the window, reaching past its end when a phase correction
 * pulls cells in from the gap that follows the sector.
 */
static int window_cell(const dr_view *v, int i)
{
	HXCFE_SIDE *side = (HXCFE_SIDE *)v->side;

	if (i >= 0 && i < v->ncells)
		return v->cells[i].state;
	if (!side)
		return 0;
	return dr_getcell(side, v->base_cell + i);
}

/*
 * Decode the message with a cell-phase correction applied.
 *
 * A decoder that gives one run of intervals too many or too few cells
 * does not corrupt a byte - it moves the byte boundary, and everything
 * after it decodes as a different byte string entirely. Undoing that
 * means reading the tail from `slip` cells further along, which is what
 * this does; the reversals themselves do not move.
 */
void dr_decode_slipped(const dr_view *v, int at, int slip, uint8_t *out)
{
	int b, k;

	for (b = 0; b < v->msg_len; b++) {
		uint8_t val = 0;

		for (k = 0; k < 8; k++) {
			int cc, dc, s;

			dr_bit_cells(v->encoding, b * v->stride, k, &cc, &dc);
			s = (cc >= at) ? slip : 0;
			val = (uint8_t)(val << 1);
			if (v->encoding == DR_ENC_ISO_FM) {
				if (window_cell(v, dc + s))
					val |= 1;
			} else if (!window_cell(v, cc + s) &&
			           window_cell(v, dc + s)) {
				val |= 1;
			}
		}
		out[b] = val;
	}
}

uint8_t *dr_candidate_message(const dr_view *v, const dr_candidate *cand)
{
	uint8_t *m;
	int i;

	if (!v || !v->msg)
		return NULL;

	m = malloc((size_t)v->msg_len);
	if (!m)
		return NULL;
	if (cand->slip)
		dr_decode_slipped(v, cand->slip_at, cand->slip, m);
	else
		memcpy(m, v->msg, (size_t)v->msg_len);

	for (i = 0; i < cand->weight; i++) {
		int p = cand->bits[i];
		if (p >= 0 && p < v->msg_bits)
			m[p >> 3] ^= (uint8_t)(0x80 >> (p & 7));
	}
	return m;
}

/* Rewrite the message bytes touched by `bits` back into the track. */
static int patch_bits(dr_ctx *c, dr_view *v, const dr_candidate *cand)
{
	HXCFE_SIDE *side = (HXCFE_SIDE *)v->side;
	const int *bits = cand->bits;
	int nbits = cand->weight;
	uint8_t *m;
	int i, b, first = v->msg_len;

	if (!side)
		return -1;

	m = malloc((size_t)v->msg_len);
	if (!m)
		return -1;
	if (cand->slip)
		dr_decode_slipped(v, cand->slip_at, cand->slip, m);
	else
		memcpy(m, v->msg, (size_t)v->msg_len);

	for (i = 0; i < nbits; i++) {
		int p = bits[i];
		if (p < 0 || p >= v->msg_bits) {
			free(m);
			return -1;
		}
		m[p >> 3] ^= (uint8_t)(0x80 >> (p & 7));
	}

	/* Re-encode each touched byte so the cell stream stays legal. A
	 * phase correction moves every byte after it, so from there on the
	 * whole tail is rewritten. */
	if (cand->slip) {
		first = cand->slip_at / v->stride;
		if (first < 0)
			first = 0;
		for (b = first; b < v->msg_len; b++)
			dr_write_byte(side, v->encoding,
			              v->base_cell + b * v->stride, m[b]);
	}
	for (i = 0; i < nbits; i++) {
		b = bits[i] >> 3;
		if (b < first)
			dr_write_byte(side, v->encoding,
			              v->base_cell + b * v->stride, m[b]);
	}

	memcpy(v->msg, m, (size_t)v->msg_len);
	free(m);

	v->syndrome     = dr_crc16(v->msg, v->msg_len);
	v->computed_crc = dr_crc16(v->msg, v->msg_len - 2);
	v->stored_crc   = (uint16_t)((v->msg[v->msg_len - 2] << 8) |
	                              v->msg[v->msg_len - 1]);
	c->dirty = 1;
	return 0;
}

int dr_apply(dr_ctx *c, dr_view *v, const dr_candidate *cand)
{
	if (!c || !v || !cand)
		return -1;
	return patch_bits(c, v, cand);
}

/*
 * Plant a cell-phase slip: from `at_byte` on, shift the sector's cells
 * by `cells`, as a decoder does when it gives a run of intervals one
 * cell too many or too few. The bytes are not corrupted - the boundary
 * moves - which is why a bit-flip search cannot undo it.
 */
int dr_damage_slip(dr_ctx *c, int sector_index, int at_byte, int cells)
{
	dr_view *v;
	HXCFE_SIDE *side;
	uint8_t *tail;
	int at, n, i, rc = 0;

	if (!cells)
		return -1;
	v = dr_view_open(c, sector_index, NULL);
	if (!v)
		return -1;
	side = (HXCFE_SIDE *)v->side;
	at = at_byte * v->stride;
	n = v->msg_len * v->stride - at;
	if (at < 0 || n <= 0 || !side) {
		dr_view_free(v);
		return -1;
	}

	tail = malloc((size_t)n);
	if (!tail) {
		dr_view_free(v);
		return -1;
	}
	for (i = 0; i < n; i++)
		tail[i] = (uint8_t)dr_getcell(side, v->base_cell + at + i + cells);
	for (i = 0; i < n; i++)
		dr_setcell(side, v->base_cell + at + i, tail[i]);

	free(tail);
	c->dirty = 1;
	dr_view_free(v);
	return rc;
}

int dr_damage(dr_ctx *c, int sector_index, const int *bits, int nbits,
              int drop_only)
{
	dr_view *v;
	int keep[64], nkeep = 0, i, rc;

	v = dr_view_open(c, sector_index, NULL);
	if (!v)
		return -1;

	for (i = 0; i < nbits && nkeep < (int)(sizeof(keep)/sizeof(keep[0])); i++) {
		int b = bits[i];

		if (b < 0 || b >= v->msg_bits)
			continue;

		/* A dropout can only take a reversal away, so the bit has to
		 * read 1. Asking for one at a position that reads 0 means the
		 * nearest reversal after it - "drop a reversal around here"
		 * is what the caller meant. */
		if (drop_only) {
			int j, seen = 0;

			for (j = b; j < v->msg_bits; j++) {
				if (!msg_bit(v->msg, j))
					continue;
				for (seen = 0; seen < nkeep; seen++)
					if (keep[seen] == j)
						break;
				if (seen == nkeep) {
					b = j;
					break;
				}
			}
			if (j >= v->msg_bits)
				continue;
		}
		keep[nkeep++] = b;
	}

	if (nkeep) {
		dr_candidate cd;

		memset(&cd, 0, sizeof(cd));
		cd.weight = nkeep;
		memcpy(cd.bits, keep, (size_t)nkeep * sizeof(*keep));
		rc = patch_bits(c, v, &cd);
	} else {
		rc = 0;
	}
	if (rc == 0)
		rc = nkeep;
	dr_view_free(v);
	return rc;
}
