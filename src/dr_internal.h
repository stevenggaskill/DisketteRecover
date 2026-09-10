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

/* ---- encoding helpers --------------------------------------------- */
void dr_decode(const HXCFE_SIDE *s, dr_encoding enc, int cell,
               uint8_t *out, int len);
int  dr_byte_stride(dr_encoding enc);
void dr_bit_cells(dr_encoding enc, int base, int bit, int *clock, int *data);
void dr_write_byte(HXCFE_SIDE *s, dr_encoding enc, int base, uint8_t value);

#endif /* DR_INTERNAL_H */
