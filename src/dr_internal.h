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

/* A byte model of what this disk's data looks like (see dr_pattern.c). */
typedef struct dr_model dr_model;

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

	dr_model  *model;        /* built lazily from the clean sectors   */

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

/* ---- the other revolutions ----------------------------------------- */

/* What every pass of the head over this sector agreed on (dr_revs.c).
 * All arrays are indexed by reference interval, 0..n-1. */
typedef struct {
	int       nrev;      /* complete revolutions in the dump          */
	int       nused;     /* of those, aligned to this sector          */
	int       n;         /* reference intervals                       */
	uint32_t  p_first;   /* dump pulse index that ticks[0] came from   */
	uint32_t *ticks;     /* mean duration over the passes that agree  */
	uint8_t  *votes;     /* passes with a reversal at its far end      */
	uint8_t  *same;      /* passes that read it as exactly one interval*/
	uint8_t  *extra;     /* passes that saw a reversal inside it      */
	uint32_t *extra_at;  /* where the first of them put it, in ticks  */
	int       splits;    /* extra reversals seen, summed over passes  */
	int       merges;    /* reversals a pass did not see              */
	double    resid;     /* mean |pass - pass| on matched intervals   */
	int outvoted_extra;  /* reversals most passes did not see         */
	int outvoted_missing;/* reversals most passes saw and this missed */
} dr_revmap;

dr_revmap *dr_revs_build(dr_view *v);
void       dr_revs_free(dr_revmap *r);
int        dr_revs_search(dr_view *v, const dr_options *opt,
                          dr_repair_result *out);
/* The reading the passes vote for, into a v->msg_len buffer. Returns the
 * number of reversals they did not all agree on, or -1 if the dump holds
 * only one pass. */
int        dr_revs_reading(dr_view *v, uint8_t *msg, int *contested,
                           int *majority);

/* ---- flux timing model --------------------------------------------- */

/* One flux interval inside the sector window. */
typedef struct {
	int      cell;      /* window-relative cell of the ending reversal */
	int      gap;       /* cell count the decoder assigned it          */
	uint32_t pulse;     /* the dump pulse it was measured from         */
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
	double sigma;       /* interval residual std, in cells            */
	double rho;         /* lag-1 autocorrelation of those residuals   */
	double sigma_pos;   /* std of the underlying transition-position  */
	                    /* error, in cells                            */
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
double dr_cell_period(const dr_view *v);
int    dr_intervals_collect(dr_view *v, dr_interval **out, double *period);
void   dr_timing_fit(const dr_interval *iv, int n, dr_timing *t);
double dr_bin_cost(const dr_timing *t, double meas, int k);

/* Learned from every sector that reads cleanly. One 512-byte sector is
 * far too little to estimate anything from; a whole disk is 1.4 MB and
 * enough for a real order-2 model, which is what tells English prose
 * from noise. */
dr_model *dr_model_build(dr_ctx *c);
void      dr_model_free(dr_model *m);
double    dr_model_logp(const dr_model *m, int p2, int p1, int cur);
double    dr_model_score(const dr_model *m, const uint8_t *d, int n);

/* The structural model of the data field, fitted once and cached on the
 * view. NULL when nothing trustworthy fits. */
void *dr_fit_get(dr_view *v);
int   dr_fit_outliers(void *fit, const uint8_t *data, int n);
void  dr_fit_release(void *fit);

/* Re-rank an engine's results by how plausible their data is. */
int dr_rescore_data(dr_ctx *c, dr_view *v, const dr_options *opt,
                    dr_repair_result *r);

/* ---- encoding helpers --------------------------------------------- */
void dr_decode(const HXCFE_SIDE *s, dr_encoding enc, int cell,
               uint8_t *out, int len);
int  dr_byte_stride(dr_encoding enc);
void dr_bit_cells(dr_encoding enc, int base, int bit, int *clock, int *data);
void dr_write_byte(HXCFE_SIDE *s, dr_encoding enc, int base, uint8_t value);

#endif /* DR_INTERNAL_H */
