/*
 * DisketteRecover - the zoomed-in view of one sector: raw cells, the
 * flux interval each cell came from, the binning confidence, and the
 * decoded bytes the CRC is computed over.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dr_internal.h"

#define WEAK_PERR      0.35
#define VIOLATION_PERR 0.25
#define MAX_PERR       0.49

static double tail_prob(double margin, double jitter)
{
	if (jitter <= 0.0)
		jitter = 0.16;
	if (margin < 0.0)
		margin = 0.0;
	/* Probability that timing jitter pushed the interval across the
	 * nearest binning boundary. */
	return 0.5 * erfc(margin / (jitter * 1.4142135623730951));
}

static void set_perr(dr_cell *c, double p, dr_evidence ev)
{
	if (p > MAX_PERR)
		p = MAX_PERR;
	if (p > c->p_err) {
		c->p_err = (float)p;
		c->evidence = (uint8_t)ev;
	}
}

/* How many of the dump's passes read this interval exactly as the
 * reference did - neither running it into its neighbour for want of the
 * reversal at its end, nor splitting it on one the reference missed.
 * -1 when the consensus does not cover it. */
static int interval_votes(const dr_revmap *rm, const dr_interval *iv, int j)
{
	uint32_t k;

	if (!rm || iv[j].pulse < rm->p_first)
		return -1;
	k = iv[j].pulse - rm->p_first;
	if ((int)k >= rm->n)
		return -1;
	return rm->same[k];
}

dr_view *dr_view_open(dr_ctx *c, int sector_index, const dr_options *opt)
{
	dr_view *v;
	HXCFE_SIDE *side;
	dr_flux_map *flux = NULL;
	dr_timing tm;
	const dr_sector *sd;
	dr_options defopt;
	int stride, base, span, i, k;
	int sync_bytes, data_bytes;
	int nrev = 0, nflux = 0, nweak = 0, weak_useful;
	int ndissent = 0;
	dr_revmap *rm;

	if (!opt) {
		dr_options_default(&defopt);
		opt = &defopt;
	}

	if (!c || sector_index < 0 || sector_index >= c->nsectors)
		return NULL;

	sd = &c->sectors[sector_index];
	side = dr_side(c, sd->track, sd->side);
	if (!side || !side->databuffer)
		return NULL;

	v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;

	v->sect = *sd;
	v->encoding = sd->encoding;
	v->sector_index = sector_index;
	v->side = side;

	stride = dr_byte_stride(sd->encoding);
	v->stride = stride;

	/* Which field do we zoom into? The broken one, data first. */
	if (sd->data_crc == DR_CRC_BAD || sd->data_crc == DR_CRC_OK) {
		snprintf(v->field, sizeof(v->field), "data");
		base = sd->data_cell;
		sync_bytes = (sd->encoding == DR_ENC_ISO_MFM) ? 3 : 0;
		data_bytes = sd->sector_size;
		v->msg_len = sync_bytes + 1 + data_bytes + 2;
		v->data_offset = sync_bytes + 1;
		v->data_len = data_bytes;
	} else {
		snprintf(v->field, sizeof(v->field), "header");
		base = sd->start_cell;
		sync_bytes = (sd->encoding == DR_ENC_ISO_MFM) ? 3 : 0;
		v->msg_len = sync_bytes + 7;   /* mark + C,H,R,N + CRC16 */
		v->data_offset = sync_bytes + 1;
		v->data_len = 4;
	}

	v->base_cell = base;
	v->msg_bits = v->msg_len * 8;
	v->first_bit = sync_bytes * 8;

	v->msg = calloc((size_t)v->msg_len, 1);
	if (!v->msg) {
		dr_view_free(v);
		return NULL;
	}
	dr_decode(side, sd->encoding, base, v->msg, v->msg_len);

	v->syndrome     = dr_crc16(v->msg, v->msg_len);
	v->computed_crc = dr_crc16(v->msg, v->msg_len - 2);
	v->stored_crc   = (uint16_t)((v->msg[v->msg_len - 2] << 8) |
	                              v->msg[v->msg_len - 1]);

	/* ---- cell window ------------------------------------------- */
	span = v->msg_len * stride;
	v->first_cell = base;
	v->ncells = span;
	v->cells = calloc((size_t)span, sizeof(dr_cell));
	if (!v->cells) {
		dr_view_free(v);
		return NULL;
	}

	for (i = 0; i < span; i++) {
		dr_cell *cc = &v->cells[i];
		cc->cell = dr_wrap(side->tracklen, base + i);
		cc->state = (uint8_t)dr_getcell(side, base + i);
		cc->weak = (uint8_t)dr_getweak(side, base + i);
		cc->bin = -1;
		cc->best_bin = -1;
		cc->p_bin = -1.0f;
		cc->interval_ticks = -1;
		cc->interval_cells = -1.0f;
		cc->margin = -1.0f;
		cc->p_err = (float)opt->base_perr;
		cc->evidence = DR_EV_NONE;
		if (cc->state)
			nrev++;
		if (cc->weak)
			nweak++;
	}

	v->flux = dr_flux_build(c, side);
	flux = (dr_flux_map *)v->flux;
	v->flux_available = flux ? 1 : 0;

	/* A dump usually holds several passes over the track. Fold them
	 * together before anything is measured off the flux. */
	if (flux)
		v->revs = dr_revs_build(v);

	/* ---- fit the track's own timing, then bin against it -------- */
	/* What a 2T, 3T or 4T interval actually measures depends on the
	 * dump's bitrate error and on peak shift from its neighbours, so
	 * the bin centres are learned from this track rather than assumed
	 * to be 2.0/3.0/4.0. */
	{
		dr_interval *iv = NULL;
		double period = 0.0;
		int niv, j;

		memset(&tm, 0, sizeof(tm));
		rm = (dr_revmap *)v->revs;
		niv = dr_intervals_collect(v, &iv, &period);

		if (niv > 0) {
			dr_timing_fit(iv, niv, &tm);
			for (j = 0; j < niv; j++)
				iv[j].adj = dr_timing_adjust(&tm, iv[j].meas,
				        j > 0 ? iv[j - 1].gap : 3,
				        j + 1 < niv ? iv[j + 1].gap : 3);
			v->period = period;
			v->fit_a = tm.a;
			v->fit_b = tm.b;
			v->fit_sigma = tm.sigma;
			v->fit_n = tm.n;
		}

		for (j = 0; j < niv; j++) {
			double p = opt->base_perr;
			double margin = -1.0, p_bin = -1.0;
			dr_evidence ev = DR_EV_NONE;
			int gap = iv[j].gap;
			int best_bin = -1, lo, m;

			if (iv[j].meas > 0.0)
				nflux++;

			if (iv[j].meas > 0.0 && tm.valid) {
				/* Posterior over the three legal bins. */
				double cost[5], best = 1e18, sum = 0.0;
				int k;

				for (k = 2; k <= 4; k++) {
					cost[k] = dr_bin_cost(&tm, iv[j].adj, k);
					if (cost[k] < best) {
						best = cost[k];
						best_bin = k;
					}
				}
				for (k = 2; k <= 4; k++)
					sum += exp(-(cost[k] - best));

				p_bin = (gap >= 2 && gap <= 4)
				          ? exp(-(cost[gap] - best)) / sum : 0.0;
				p = 1.0 - p_bin;

				/* How far the measurement sits from the boundary
				 * with its nearest rival, in cell periods. */
				margin = 0.5 - fabs(iv[j].adj -
				        (tm.a + tm.b * (double)best_bin)) /
				        (tm.b > 0.1 ? tm.b : 1.0);
				if (margin < 0.0) margin = 0.0;
				if (margin > 0.5) margin = 0.5;
				ev = DR_EV_FLUX;
			} else if (iv[j].meas > 0.0) {
				double d = fabs(iv[j].meas - (double)gap);
				margin = 0.5 - d;
				if (margin < 0.0)
					margin = 0.0;
				p = tail_prob(margin, opt->jitter);
				ev = DR_EV_FLUX;
			}

			/* Illegal MFM cell spacing: something is wrong here
			 * whatever the timings say. */
			if (v->encoding == DR_ENC_ISO_MFM && (gap < 2 || gap > 4)) {
				if (VIOLATION_PERR > p) {
					p = VIOLATION_PERR;
					ev = DR_EV_VIOLATION;
				}
			}

			/*
			 * ...and what the dump's other passes made of it.
			 *
			 * A reversal that every pass saw is in the oxide. One
			 * that only some passes saw is the read amplifier
			 * firing on noise, which is what an unmagnetised or
			 * half-erased patch of media looks like from outside
			 * - and no amount of timing analysis on a single pass
			 * can tell the two apart, because a single pass has
			 * nothing to disagree with. This is the one piece of
			 * evidence the extra revolutions provide that cannot
			 * be had any other way, so it overrides.
			 */
			if (rm && rm->nused > 1) {
				int seen = interval_votes(rm, iv, j);

				if (seen >= 0 && seen < rm->nused) {
					double q = 1.0 - (double)seen /
					                 (double)rm->nused;
					if (q > MAX_PERR)
						q = MAX_PERR;
					if (q > p) {
						p = q;
						ev = DR_EV_DISSENT;
						margin = 0.0;
					}
					ndissent++;
				}
			}

			/* The whole interval is jointly uncertain: the reversal
			 * could have landed on any cell inside it. */
			lo = iv[j].cell - gap + 1;
			if (lo < 0)
				lo = 0;
			for (m = lo; m <= iv[j].cell && m < span; m++) {
				dr_cell *cc = &v->cells[m];
				cc->interval_ticks = (int32_t)iv[j].ticks;
				cc->interval_cells = (float)iv[j].meas;
				cc->bin = gap;
				cc->best_bin = best_bin;
				cc->margin = (float)margin;
				cc->p_bin = (float)p_bin;
				set_perr(cc, p, ev);
			}
		}

		free(iv);
	}

	/* libhxcfe's weak-bit flag is a strong hint when it picks out a few
	 * cells, and worthless when a noisy dump makes it fire everywhere -
	 * a flag on every cell tells us nothing about which one is wrong. */
	weak_useful = (nweak * 4 < span);
	if (weak_useful) {
		for (i = 0; i < span; i++) {
			if (v->cells[i].weak)
				set_perr(&v->cells[i], WEAK_PERR, DR_EV_WEAKBIT);
		}
	}

	/* ---- per-message-bit probabilities -------------------------- */
	v->bit_perr = calloc((size_t)v->msg_bits, sizeof(float));
	v->bytes = calloc((size_t)v->msg_len, sizeof(dr_byte));
	if (!v->bit_perr || !v->bytes) {
		dr_view_free(v);
		return NULL;
	}
	v->nbytes = v->msg_len;

	for (i = 0; i < v->msg_len; i++) {
		dr_byte *b = &v->bytes[i];

		b->msg_index = i;
		b->cell = dr_wrap(side->tracklen, base + i * stride);
		b->value = v->msg[i];

		if (i < sync_bytes)
			snprintf(b->role, sizeof(b->role), "sync");
		else if (i == sync_bytes)
			snprintf(b->role, sizeof(b->role), "mark");
		else if (i >= v->msg_len - 2)
			snprintf(b->role, sizeof(b->role), "crc");
		else
			snprintf(b->role, sizeof(b->role), "data");

		for (k = 0; k < 8; k++) {
			int cc_off, dc_off;
			double pc, pd, p;

			dr_bit_cells(sd->encoding, i * stride, k, &cc_off, &dc_off);
			pd = v->cells[dc_off].p_err;
			pc = (sd->encoding == DR_ENC_ISO_MFM)
			         ? v->cells[cc_off].p_err : 0.0;
			p = 1.0 - (1.0 - pc) * (1.0 - pd);
			if (p > MAX_PERR)
				p = MAX_PERR;

			v->bit_perr[i * 8 + k] = (float)p;
			b->p_err[k] = (float)p;
		}
	}

	if (flux && tm.valid)
		snprintf(v->model, sizeof(v->model),
		         "flux timing, %d/%d reversals binned against cell = "
		         "%.2f + %.3f*bin %+.3f*prev %+.3f*next, sigma %.3f "
		         "(%d fitted)",
		         nflux, nrev, tm.a, tm.b, tm.c, tm.d, tm.sigma, tm.n);
	else
		snprintf(v->model, sizeof(v->model),
		         flux ? "flux timing (%d/%d reversals, no usable model, "
		                "%d weak%s)"
		              : "bitstream only (%d/%d reversals, %d weak%s)",
		         nflux, nrev, nweak,
		         (nweak && !weak_useful) ? ", ignored - too many" : "");

	if (rm) {
		v->nrev = rm->nrev;
		v->nrev_used = rm->nused;
		v->rev_resid = rm->resid / (v->period > 0.0 ? v->period : 1.0);
		v->rev_dissent = ndissent;
		snprintf(v->passes, sizeof(v->passes),
		         "%d of %d pass(es) over the track combined; they "
		         "disagree by %.3f cell on average and about %d "
		         "reversal(s) outright",
		         rm->nused, rm->nrev, v->rev_resid, ndissent);
	}

	return v;
}

void dr_view_free(dr_view *v)
{
	if (!v)
		return;
	dr_flux_free((dr_flux_map *)v->flux);
	dr_revs_free((dr_revmap *)v->revs);
	dr_fit_release(v->fit);
	free(v->msg);
	free(v->cells);
	free(v->bytes);
	free(v->bit_perr);
	free(v);
}
