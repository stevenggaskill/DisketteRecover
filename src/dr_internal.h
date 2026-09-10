/*
 * DisketteRecover - internal declarations shared between the modules.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DR_INTERNAL_H
#define DR_INTERNAL_H

/* Include order matters: libhxcfe.h declares HXCFE_SECTCFG / HXCFE_SIDE as
 * opaque void unless the real definitions have been pulled in first. */
#include "types.h"
#include "internal_libhxcfe.h"
#include "internal_floppy.h"
#include "tracks/track_generator.h"
#include "libhxcfe.h"

#include "dr.h"

struct dr_ctx {
	HXCFE              *hxcfe;
	HXCFE_IMGLDR       *loader;
	HXCFE_FLOPPY       *floppy;
	HXCFE_SECTORACCESS *sacc;

	char   path[1024];
	char   error[512];

	int    loader_id;
	int    verbose;
	int    dirty;            /* track cells were patched              */

	dr_sector *sectors;
	int        nsectors;
	int        scanned;
};

/* ---- cell helpers -------------------------------------------------*/
static inline int dr_wrap(int len, int off)
{
	if (len <= 0)
		return 0;
	off %= len;
	if (off < 0)
		off += len;
	return off;
}

static inline int dr_getcell(const HXCFE_SIDE *s, int off)
{
	off = dr_wrap(s->tracklen, off);
	return (s->databuffer[off >> 3] >> (7 - (off & 7))) & 1;
}

static inline void dr_setcell(HXCFE_SIDE *s, int off, int state)
{
	off = dr_wrap(s->tracklen, off);
	if (state)
		s->databuffer[off >> 3] |= (uint8_t)(0x80 >> (off & 7));
	else
		s->databuffer[off >> 3] &= (uint8_t)~(0x80 >> (off & 7));
}

static inline int dr_getweak(const HXCFE_SIDE *s, int off)
{
	if (!s->flakybitsbuffer)
		return 0;
	off = dr_wrap(s->tracklen, off);
	return (s->flakybitsbuffer[off >> 3] >> (7 - (off & 7))) & 1;
}

HXCFE_SIDE *dr_side(dr_ctx *c, int track, int side);

/* ---- flux alignment ----------------------------------------------- */

/* Result of lining the decoded cell stream back up with the raw flux
 * pulses that produced it. */
typedef struct {
	int       valid;
	int       start_pulse;      /* first pulse of the analysed rev    */
	uint32_t *pulse_of_cell;    /* cell -> pulse index, or 0xFFFFFFFF */
	int       ncells;
	int       tick_freq;
	const uint32_t *stream;
	uint32_t  nb_pulses;
} dr_flux_map;

dr_flux_map *dr_flux_build(dr_ctx *c, HXCFE_SIDE *side);
void         dr_flux_free(dr_flux_map *m);

/* ---- flux timing model --------------------------------------------- */

/* One flux interval inside the sector window. */
typedef struct {
	int      cell;      /* window-relative cell of the ending reversal */
	int      gap;       /* cell count the decoder assigned it          */
	uint32_t ticks;     /* measured duration                          */
	double   meas;      /* ticks / cell period, in cells              */
	double   adj;       /* meas with the neighbour pull taken out     */
} dr_interval;

/* Measured cell length as a function of the bin: meas ~ a + b*k.
 *
 * The offset and gain are not cosmetic. A dump's bitrate estimate is
 * never exact, and peak shift (adjacent reversals repelling each other)
 * biases short intervals long and long intervals short - on real media
 * a 4T can sit at 3.83T while a 2T sits at 2.04T. Scoring against the
 * ideal 2/3/4 would call half a good track ambiguous. */
typedef struct {
	double a, b;        /* offset and gain                            */
	double c, d;        /* pull from the previous / next interval     */
	double sigma;       /* residual standard deviation, in cells      */
	int    n;           /* intervals the fit was built from           */
	int    valid;
} dr_timing;

/* Peak shift makes an interval's measured length depend on its
 * neighbours, so measurements are corrected to a neighbour-free frame
 * before being compared with the bin centres. */
double dr_timing_adjust(const dr_timing *t, double meas,
                        int prev_gap, int next_gap);

/* Collect the intervals covering the view's cell window. Returns the
 * count, or -1 when there is no usable flux. */
int    dr_intervals_collect(dr_view *v, dr_interval **out, double *period);
void   dr_timing_fit(const dr_interval *iv, int n, dr_timing *t);
double dr_bin_cost(const dr_timing *t, double meas, int k);

/* ---- encoding helpers --------------------------------------------- */
void dr_decode(const HXCFE_SIDE *s, dr_encoding enc, int cell,
               uint8_t *out, int len);
int  dr_byte_stride(dr_encoding enc);
void dr_bit_cells(dr_encoding enc, int base, int bit, int *clock, int *data);
void dr_write_byte(HXCFE_SIDE *s, dr_encoding enc, int base, uint8_t value);

#endif /* DR_INTERNAL_H */
