/*
 * DisketteRecover - realign the decoded cell stream with the raw flux
 * pulses that produced it.
 *
 * libhxcfe's flux analyser turns a pulse list into a cell stream: it
 * plants a reversal at cell 0 and then, for every pulse, advances by the
 * number of cell periods its PLL decided on and marks a reversal there.
 * So the k-th gap between reversals is the PLL's verdict on one pulse,
 * and the two sequences line up bar a constant offset - the pulse the
 * analysed revolution started at.
 *
 * Two things stop that from being a one-line lookup:
 *
 *  - Rounding a pulse length to whole cells only approximates the PLL,
 *    which tracks phase, so the sequences agree on most gaps but not
 *    all - and a damaged stretch throws the PLL off for a while.
 *  - Around a bad transition the analyser can swallow or add a reversal,
 *    which shifts the correspondence by one from there on.
 *
 * So we anchor on a stretch that matches strongly, then walk outwards
 * re-synchronising whenever the local agreement says the offset moved.
 * libhxcfe only fills side->cell_to_tick[] on its track-viewer path, so
 * nothing here may depend on it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "dr_internal.h"

#define DBG(...) do { if (getenv("DR_DEBUG")) \
	fprintf(stderr, "dr_flux: " __VA_ARGS__); } while (0)

#define NO_PULSE   0xFFFFFFFFu
#define ANCHOR     24        /* pre-filter window                       */
#define ANCHOR_MIN 23        /* matches needed to score an offset       */
#define VERIFY     4000      /* gaps used to score an offset            */
#define MIN_GAPS   64        /* shortest scoreable stretch              */
#define MIN_AGREE  60        /* percent of gaps that must agree         */
#define RESYNC     3         /* how far the walk may re-synchronise     */
#define WINDOW     16        /* look-ahead used to pick a re-sync       */

static double cell_period(const HXCFE_SIDE *side, int tick_freq)
{
	double bitrate = (double)side->bitrate;

	if (bitrate < 1000.0 && side->timingbuffer)
		bitrate = (double)side->timingbuffer[0];
	if (bitrate < 1000.0)
		return 0.0;
	/* HxC counts "bitrate" in data bits/s and lays down two cells per
	 * data bit, so a cell lasts tick_freq / (2 * bitrate) ticks. */
	return (double)tick_freq / (2.0 * bitrate);
}

static uint8_t clampcode(long v)
{
	if (v < 1)
		return 1;
	if (v > 15)
		return 15;
	return (uint8_t)v;
}

/* How many of the next `w` gaps match if gap k maps to pulse k+d. */
static int local_score(const uint8_t *codes, int np,
                       const uint8_t *gaps, int ngap,
                       int k, int d, int w)
{
	int i, n = 0;

	for (i = 0; i < w; i++) {
		int g = k + i, p = k + i + d;
		if (g >= ngap || p < 0 || p >= np)
			break;
		if (codes[p] == gaps[g])
			n++;
	}
	return n;
}

dr_flux_map *dr_flux_build(dr_ctx *c, HXCFE_SIDE *side)
{
	dr_flux_map *m = NULL;
	const HXCFE_STREAMCHANNEL *ch;
	uint8_t  *codes = NULL;
	uint8_t  *gaps = NULL;
	int      *rev = NULL;
	int      *map = NULL;        /* gap index -> pulse index          */
	double    period;
	uint32_t  np;
	int       nrev = 0, ngap, i, tick_freq;
	int       anchor_gap = -1, anchor_off = 0, best_agree = 0;

	(void)c;

	if (!side || !side->stream_dump || !side->databuffer) {
		DBG("no %s\n", side ? "stream_dump" : "side");
		return NULL;
	}

	ch = &side->stream_dump->channels[0];
	if (!ch->stream || ch->nb_of_pulses < 256) {
		DBG("no pulses\n");
		return NULL;
	}
	np = ch->nb_of_pulses;

	tick_freq = side->tick_freq ? side->tick_freq
	                            : side->stream_dump->tick_freq;
	period = tick_freq > 0 ? cell_period(side, tick_freq) : 0.0;
	if (period <= 0.0) {
		DBG("no tick frequency or bitrate\n");
		return NULL;
	}

	/* ---- reversal gaps out of the decoded cell stream ----------- */
	rev = malloc((size_t)side->tracklen * sizeof(int));
	if (!rev)
		return NULL;
	for (i = 0; i < side->tracklen; i++) {
		if (dr_getcell(side, i))
			rev[nrev++] = i;
	}
	if (nrev < MIN_GAPS + 8) {
		DBG("only %d reversals\n", nrev);
		goto out;
	}

	ngap = nrev - 1;
	gaps = malloc((size_t)ngap);
	codes = malloc(np);
	map = malloc((size_t)ngap * sizeof(int));
	if (!gaps || !codes || !map)
		goto out;

	for (i = 0; i < ngap; i++)
		gaps[i] = clampcode(rev[i + 1] - rev[i]);
	for (i = 0; i < (int)np; i++)
		codes[i] = clampcode((long)((double)ch->stream[i] / period + 0.5));

	/* ---- find the strongest anchor ------------------------------ */
	/* Several anchor positions, so a damaged track start cannot hide
	 * the alignment; a multi-revolution dump's other revolutions lose
	 * because they run out of pulses sooner. */
	{
		int probe[4], nprobe = 0, pi;

		probe[nprobe++] = 2;             /* cell 0 is synthetic */
		if (ngap > 4 * MIN_GAPS) {
			probe[nprobe++] = ngap / 4;
			probe[nprobe++] = ngap / 2;
			probe[nprobe++] = (ngap * 3) / 4;
		}

		for (pi = 0; pi < nprobe; pi++) {
			int skip = probe[pi];
			int limit = (int)np - ANCHOR;
			int start;

			if (skip + ANCHOR + MIN_GAPS >= ngap)
				continue;

			for (start = 0; start <= limit; start++) {
				int anchor = 0, agree = 0, checked = 0, off;

				for (off = 0; off < ANCHOR; off++)
					if (codes[start + off] == gaps[skip + off])
						anchor++;
				if (anchor < ANCHOR_MIN)
					continue;

				for (off = 0; off < VERIFY; off++) {
					if (skip + off >= ngap ||
					    start + off >= (int)np)
						break;
					checked++;
					if (codes[start + off] == gaps[skip + off])
						agree++;
				}

				if (checked >= MIN_GAPS &&
				    agree * 100 >= checked * MIN_AGREE &&
				    agree > best_agree) {
					best_agree = agree;
					anchor_gap = skip;
					anchor_off = start - skip;
				}
			}

			if (best_agree >= (VERIFY * 9) / 10)
				break;                /* already conclusive */
		}
	}

	if (anchor_gap < 0) {
		DBG("no alignment found (%d reversals, %u pulses, period %.1f)\n",
		    nrev, np, period);
		goto out;
	}
	DBG("anchor at gap %d, offset %d, %d agreeing gaps\n",
	    anchor_gap, anchor_off, best_agree);

	/* ---- walk outwards, re-synchronising as needed -------------- */
	{
		int k, d, resyncs = 0;

		d = anchor_off;
		for (k = anchor_gap; k < ngap; k++) {
			if (k + d < 0 || k + d >= (int)np ||
			    codes[k + d] != gaps[k]) {
				int dd, bd = 0;
				int bs = local_score(codes, (int)np, gaps, ngap,
				                     k, d, WINDOW);
				for (dd = -RESYNC; dd <= RESYNC; dd++) {
					int sc;
					if (!dd)
						continue;
					sc = local_score(codes, (int)np, gaps,
					                 ngap, k, d + dd, WINDOW);
					if (sc > bs) {
						bs = sc;
						bd = dd;
					}
				}
				if (bd) {
					d += bd;
					resyncs++;
				}
			}
			map[k] = (k + d >= 0 && k + d < (int)np) ? k + d : -1;
		}

		d = anchor_off;
		for (k = anchor_gap - 1; k >= 0; k--) {
			if (k + d < 0 || k + d >= (int)np ||
			    codes[k + d] != gaps[k]) {
				int dd, bd = 0;
				int bs = local_score(codes, (int)np, gaps, ngap,
				                     k, d, WINDOW);
				for (dd = -RESYNC; dd <= RESYNC; dd++) {
					int sc;
					if (!dd)
						continue;
					sc = local_score(codes, (int)np, gaps,
					                 ngap, k, d + dd, WINDOW);
					if (sc > bs) {
						bs = sc;
						bd = dd;
					}
				}
				if (bd) {
					d += bd;
					resyncs++;
				}
			}
			map[k] = (k + d >= 0 && k + d < (int)np) ? k + d : -1;
		}

		DBG("walk complete, %d re-sync(s)\n", resyncs);
	}

	/* ---- build the cell -> pulse map ---------------------------- */
	m = calloc(1, sizeof(*m));
	if (!m)
		goto out;

	m->pulse_of_cell = malloc((size_t)side->tracklen * sizeof(uint32_t));
	if (!m->pulse_of_cell) {
		free(m);
		m = NULL;
		goto out;
	}
	for (i = 0; i < side->tracklen; i++)
		m->pulse_of_cell[i] = NO_PULSE;

	/* Gap k runs from reversal k to reversal k+1, so the pulse it maps
	 * to is the one that landed on reversal k+1. */
	for (i = 0; i < ngap; i++) {
		if (map[i] >= 0)
			m->pulse_of_cell[rev[i + 1]] = (uint32_t)map[i];
	}

	m->valid       = 1;
	m->start_pulse = anchor_off;
	m->ncells      = side->tracklen;
	m->tick_freq   = tick_freq;
	m->stream      = ch->stream;
	m->nb_pulses   = np;

out:
	free(rev);
	free(gaps);
	free(codes);
	free(map);
	return m;
}

void dr_flux_free(dr_flux_map *m)
{
	if (!m)
		return;
	free(m->pulse_of_cell);
	free(m);
}
