/*
 * DisketteRecover - CRC-guided flux-level floppy repair, built on the
 * HxC Floppy Emulator library (libhxcfe).
 *
 * Copyright (C) 2026 DisketteRecover contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DR_H
#define DR_H

#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Opaque libhxcfe handles - kept as void* so callers of this header   */
/* do not need the whole HxC include tree.                             */
/* ------------------------------------------------------------------ */
typedef struct dr_ctx dr_ctx;

/* Encodings we can reason about at the bit level. */
typedef enum {
	DR_ENC_UNSUPPORTED = 0,
	DR_ENC_ISO_MFM,          /* IBM/ISO System 34 MFM (DD/HD)         */
	DR_ENC_ISO_FM            /* IBM/ISO System 3740 FM (SD)           */
} dr_encoding;

/* Where a sector's CRC stands. */
typedef enum {
	DR_CRC_OK = 0,
	DR_CRC_BAD = 1,
	DR_CRC_ABSENT = 2        /* header found but no data field        */
} dr_crc_state;

/* ------------------------------------------------------------------ */
/* Sector index entry produced by the scan pass.                       */
/* ------------------------------------------------------------------ */
typedef struct {
	int          track;      /* physical cylinder                     */
	int          side;
	int          order;      /* physical order within the track       */

	int          cylinder_id;/* values read out of the address mark   */
	int          head_id;
	int          sector_id;
	int          size_id;
	int          sector_size;

	dr_encoding  encoding;
	int          bitrate;

	dr_crc_state header_crc; /* address-mark CRC state                */
	dr_crc_state data_crc;   /* data-field CRC state                  */

	uint32_t     stored_header_crc;
	uint32_t     stored_data_crc;

	int          start_cell; /* bit offset of the header A1 sync      */
	int          data_cell;  /* bit offset of the data A1 sync        */
	int          end_cell;   /* bit offset just past the data CRC     */
	int          datamark;   /* 0xFB / 0xF8 ...                       */
} dr_sector;

/* ------------------------------------------------------------------ */
/* Per-cell evidence for one sector window.                            */
/* ------------------------------------------------------------------ */

/* How a cell's reliability was established. */
typedef enum {
	DR_EV_NONE = 0,          /* no timing information at all          */
	DR_EV_WEAKBIT,           /* libhxcfe flagged the cell weak        */
	DR_EV_FLUX,              /* derived from measured flux intervals  */
	DR_EV_VIOLATION          /* illegal MFM cell spacing              */
} dr_evidence;

typedef struct {
	int      cell;           /* absolute bit offset in the track      */
	uint8_t  state;          /* 0 = no reversal, 1 = reversal         */
	uint8_t  weak;           /* libhxcfe weak-bit flag                */
	uint8_t  evidence;       /* dr_evidence                           */

	/* Flux binning. Only meaningful when evidence == DR_EV_FLUX and   */
	/* the cell carries a reversal (state == 1): the interval that    */
	/* ended on this cell.                                            */
	int32_t  interval_ticks; /* measured flux interval                */
	float    interval_cells; /* interval expressed in cell periods    */
	int      bin;            /* the bin the decoder used (2T/3T/4T)   */
	int      best_bin;       /* the bin the timings actually favour    */
	float    margin;         /* 0..0.5 distance to the bin boundary   */
	float    p_bin;          /* posterior that `bin` is right          */

	float    p_err;          /* probability this cell is wrong        */
} dr_cell;

/* One decoded byte of the CRC-covered message. */
typedef struct {
	int      msg_index;      /* index inside the CRC message          */
	int      cell;           /* bit offset of the byte's first cell   */
	uint8_t  value;
	char     role[12];       /* "sync","mark","data","crc"            */
	float    p_err[8];       /* per-bit error probability, MSB first  */
} dr_byte;

/* The "zoomed in" view of one sector. */
typedef struct {
	dr_sector sect;

	dr_encoding encoding;
	char     field[8];       /* "data" or "header"                    */

	/* CRC message: 3xA1 + mark + data + 2 CRC bytes (MFM)            */
	uint8_t  *msg;
	int       msg_len;
	int       msg_bits;
	int       data_offset;   /* index of data[0] inside msg           */
	int       data_len;
	uint16_t  syndrome;      /* 0 => CRC valid                        */
	uint16_t  stored_crc;
	uint16_t  computed_crc;

	/* Cell window covering the whole CRC message.                    */
	dr_cell  *cells;
	int       ncells;
	int       first_cell;

	dr_byte  *bytes;
	int       nbytes;

	/* Per-message-bit error probability (msg_bits entries).          */
	float    *bit_perr;

	int       flux_available;/* 1 if flux timings were aligned        */
	void     *flux;          /* dr_flux_map *, kept for the re-binner */
	double    period;        /* ticks per cell                        */
	double    fit_a, fit_b;  /* measured cell length ~ a + b*bin      */
	double    fit_sigma;
	int       fit_n;
	char      model[160];     /* short description of the evidence used*/

	/* Internal bookkeeping used by dr_apply(). */
	void     *side;          /* HXCFE_SIDE *                          */
	int       base_cell;     /* first cell of the CRC message         */
	int       stride;        /* cells per byte                        */
	int       first_bit;     /* first message bit eligible for repair */
	int       sector_index;
} dr_view;

/* ------------------------------------------------------------------ */
/* A repair candidate: a set of message bits to flip.                  */
/* ------------------------------------------------------------------ */
/* A bit-flip search never goes deep, but a re-binning result can move
 * a whole burst worth of bits at once. */
#define DR_MAX_WEIGHT 96
#define DR_MAX_SEARCH_WEIGHT 6

typedef struct {
	int     weight;
	int     bits[DR_MAX_WEIGHT];   /* message bit indices, ascending  */
	double  log_likelihood;        /* higher = more plausible         */
	double  rel_likelihood;        /* normalised against the best     */
	uint8_t before[DR_MAX_WEIGHT]; /* current bit values              */

	/* Re-binning results only: how the flux was re-read. */
	int     rebins;                /* intervals given a different bin */
	double  flux_cost;             /* -log likelihood of the timings  */
} dr_candidate;

typedef struct {
	dr_candidate *list;
	int           count;
	int           searched_weight;  /* deepest weight actually tried  */
	int           npool;            /* candidate bits considered      */
	int           truncated;        /* search hit the result cap      */

	/* Re-binning search bookkeeping. */
	int           rebin;            /* results came from re-binning   */
	int           ambiguous;        /* intervals the timings left open*/
	int           pinned_moves;     /* intervals the timings re-bin   */
	long          explored;         /* assignments actually tested    */
	double        floor_cost;       /* cost of the likeliest reading  */
	double        current_cost;     /* what the decoder's own reading */
	                                /* costs under the timing model   */
	int           floor_valid;      /* ...and whether its CRC passes  */
	int           uncertain_bits;   /* message bits still in doubt    */
	char          note[160];
} dr_repair_result;

typedef enum {
	DR_MODE_AUTO = 0,       /* re-bin when there is flux, else bits   */
	DR_MODE_BITS,
	DR_MODE_REBIN
} dr_mode;

typedef struct {
	double good_threshold;  /* p_err below this = "assumed good"      */
	double base_perr;       /* prior when there is no timing evidence */
	double jitter;          /* flux jitter sigma, in cell periods     */
	int    max_weight;      /* deepest error weight to try            */
	int    max_pool;        /* cap on candidate bits                  */
	int    max_results;     /* cap on returned candidates             */

	dr_mode mode;
	double  bin_budget;     /* nats of timing cost a re-bin may spend */
	long    max_explore;    /* cap on re-binning assignments tested   */
	int     max_ambiguous;  /* refuse to search past this many        */
	int     rebin_width;    /* re-readings kept per disturbed stretch */
} dr_options;

void        dr_options_default(dr_options *o);

/* ------------------------------------------------------------------ */
/* Context lifecycle                                                   */
/* ------------------------------------------------------------------ */
dr_ctx     *dr_open(const char *path, int verbose);
/* Same, but applying "NAME=VALUE" overrides to libhxcfe's environment
 * first - the PLL and loader settings only take effect at load time. */
dr_ctx     *dr_open_ex(const char *path, int verbose,
                       char *const *sets, int nsets);
void        dr_close(dr_ctx *c);
const char *dr_last_error(dr_ctx *c);
const char *dr_path(dr_ctx *c);
int         dr_tracks(dr_ctx *c);
int         dr_sides(dr_ctx *c);

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */
int         dr_scan(dr_ctx *c);                 /* fills the index     */
const dr_sector *dr_sectors(dr_ctx *c, int *n);
/* Index of the first sector (physical order) with a bad CRC, or -1.   */
int         dr_first_bad(dr_ctx *c);

/* ------------------------------------------------------------------ */
/* Zoomed view + repair                                                */
/* ------------------------------------------------------------------ */
dr_view    *dr_view_open(dr_ctx *c, int sector_index, const dr_options *o);
void        dr_view_free(dr_view *v);

int         dr_repair_search(dr_view *v, const dr_options *o,
                             dr_repair_result *out);
/* Re-read the sector's flux under a different, equally legal binning of
 * the transitions, keeping the total cell count intact. */
int         dr_rebin_search(dr_view *v, const dr_options *o,
                            dr_repair_result *out);
void        dr_repair_free(dr_repair_result *r);

/* Materialise a candidate: returns a freshly allocated copy of the
 * CRC message with the candidate's bits flipped. Caller frees. */
uint8_t    *dr_candidate_message(const dr_view *v, const dr_candidate *cand);

/* Write a candidate back into the track bit cells held by libhxcfe. */
int         dr_apply(dr_ctx *c, dr_view *v, const dr_candidate *cand);

/* Re-run libhxcfe's own decoder over the patched track and report
 * whether the sector now reads clean. */
int         dr_verify(dr_ctx *c, int sector_index);

/* Export the (possibly patched) floppy through libhxcfe. */
int         dr_export(dr_ctx *c, const char *path, const char *format);

/* Deliberately corrupt decoded data bits - used to build test images. */
int         dr_damage(dr_ctx *c, int sector_index, const int *bits, int nbits);

/* ------------------------------------------------------------------ */
/* CRC helpers (CRC-16/CCITT-FALSE, poly 0x1021, init 0xFFFF)          */
/* ------------------------------------------------------------------ */
uint16_t    dr_crc16(const uint8_t *buf, int len);
/* Linear XOR contribution of flipping message bit `p` of an `nbits`
 * long message. masks[] must hold nbits entries. */
void        dr_crc16_bit_masks(int nbits, uint16_t *masks);

/* ------------------------------------------------------------------ */
/* JSON serialisation (used by the CLI and the built-in viewer)        */
/* ------------------------------------------------------------------ */
void        dr_json_scan(dr_ctx *c, FILE *f);
void        dr_json_view(dr_ctx *c, dr_view *v, FILE *f);
void        dr_json_candidates(const dr_view *v, const dr_repair_result *r,
                               int offset, int limit, FILE *f);

/* Built-in HTTP viewer. */
int         dr_serve(dr_ctx *c, const char *bind_addr, int port,
                     const dr_options *o);

#endif /* DR_H */
