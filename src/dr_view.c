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

/* Find the reversal at or before `cell`, scanning back at most `limit`. */
static int prev_reversal(const HXCFE_SIDE *s, int cell, int limit)
{
	int i;

	for (i = 0; i < limit; i++) {
		if (dr_getcell(s, cell - i))
			return cell - i;
	}
	return cell - limit;
}

dr_view *dr_view_open(dr_ctx *c, int sector_index, const dr_options *opt)
{
	dr_view *v;
	HXCFE_SIDE *side;
	dr_flux_map *flux = NULL;
	const dr_sector *sd;
	dr_options defopt;
	int stride, base, span, i, k;
	int sync_bytes, data_bytes;
	int nrev = 0, nflux = 0, nweak = 0, weak_useful;

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

	flux = dr_flux_build(c, side);
	v->flux_available = flux ? 1 : 0;

	/* ---- walk the flux intervals covering the window ------------ */
	{
		int prev = prev_reversal(side, base - 1, 32);
		int abs_cell;

		for (i = 0; i < span; i++) {
			abs_cell = base + i;
			if (!dr_getcell(side, abs_cell))
				continue;

			{
				int gap = abs_cell - prev;
				double p = opt->base_perr;
				dr_evidence ev = DR_EV_NONE;
				double cells_f = -1.0, margin = -1.0;
				int32_t ticks = -1;
				int bin = -1;
				int lo, j;

				if (flux) {
					uint32_t pi = flux->pulse_of_cell[
					        dr_wrap(side->tracklen, abs_cell)];
					if (pi != 0xFFFFFFFFu && pi < flux->nb_pulses) {
						double bitrate = side->timingbuffer
						        ? (double)side->timingbuffer[
						              dr_wrap(side->tracklen, abs_cell) / 8]
						        : (double)side->bitrate;
						double period;

						if (bitrate < 1000.0)
							bitrate = (double)side->bitrate;
						period = (double)flux->tick_freq /
						         (2.0 * bitrate);

						ticks = (int32_t)flux->stream[pi];
						if (period > 0.0) {
							double d;
							cells_f = (double)ticks / period;
							bin = gap;
							d = fabs(cells_f - (double)gap);
							margin = 0.5 - d;
							if (margin < 0.0)
								margin = 0.0;
							if (margin > 0.5)
								margin = 0.5;
							p = tail_prob(margin, opt->jitter);
							ev = DR_EV_FLUX;
							nflux++;
						}
					}
				}

				/* Illegal MFM cell spacing: something is wrong
				 * here whatever the timings say. */
				if (v->encoding == DR_ENC_ISO_MFM &&
				    (gap < 2 || gap > 4)) {
					if (VIOLATION_PERR > p) {
						p = VIOLATION_PERR;
						ev = DR_EV_VIOLATION;
					}
				}

				/* The whole interval is jointly uncertain: the
				 * reversal could have landed on any cell in it. */
				lo = prev + 1;
				if (lo < base)
					lo = base;
				for (j = lo; j <= abs_cell; j++) {
					dr_cell *cc = &v->cells[j - base];
					cc->interval_ticks = ticks;
					cc->interval_cells = (float)cells_f;
					cc->bin = bin;
					cc->margin = (float)margin;
					set_perr(cc, p, ev);
				}
			}
			prev = abs_cell;
		}
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

	snprintf(v->model, sizeof(v->model),
	         flux ? "flux timing (%d/%d reversals binned, %d weak%s)"
	              : "bitstream only (%d/%d reversals, %d weak%s)",
	         nflux, nrev, nweak,
	         (nweak && !weak_useful) ? ", ignored - too many" : "");

	dr_flux_free(flux);
	return v;
}

void dr_view_free(dr_view *v)
{
	if (!v)
		return;
	free(v->msg);
	free(v->cells);
	free(v->bytes);
	free(v->bit_perr);
	free(v);
}
