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
	DR_EV_VIOLATION,         /* illegal MFM cell spacing              */
	DR_EV_DISSENT            /* the dump's passes disagree here       */
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

	int       crc_suspect;   /* the stored CRC is inside the damage   */
	double    crc_expected_errors;  /* expected bad bits in those two */
	int       flux_available;/* 1 if flux timings were aligned        */
	void     *flux;          /* dr_flux_map *, kept for the re-binner */
	void     *revs;          /* dr_revmap *, the other passes         */
	void     *fit;           /* cached structural model of the data   */
	double    period;        /* ticks per cell                        */
	double    fit_a, fit_b;  /* measured cell length ~ a + b*bin      */
	double    fit_sigma;
	int       fit_n;
	char      model[160];     /* short description of the evidence used*/
	char      regions[160];   /* how the timing varies along the sector */
	int       nblocks, nblocks_local;
	double    gain_min, gain_max;

	/* What the dump's other passes over this track had to say. */
	int       nrev;          /* complete revolutions in the dump      */
	int       nrev_used;     /* of those, aligned to this sector      */
	double    rev_resid;     /* mean pass-to-pass disagreement, cells */
	int       rev_dissent;   /* reversals not every pass agreed on    */
	char      passes[160];

	/* Internal bookkeeping used by dr_apply(). */
	void     *side;          /* HXCFE_SIDE *                          */
	int       base_cell;     /* first cell of the CRC message         */
	int       stride;        /* cells per byte                        */
	int       first_bit;     /* first message bit eligible for repair */
	int       sector_index;
} dr_view;

typedef enum {
	DR_MODE_AUTO = 0,       /* try each engine, cheapest evidence first*/
	DR_MODE_REVS,
	DR_MODE_MIRROR,        /* the same bytes, stored twice on the disk*/
	DR_MODE_BITS,
	DR_MODE_REBIN,
	DR_MODE_PATTERN         /* trust the data's own regularity        */
} dr_mode;

/* ------------------------------------------------------------------ */
/* A repair candidate: a set of message bits to flip.                  */
/* ------------------------------------------------------------------ */
/* A bit-flip search never goes deep, but a re-binning result - or a data
 * model restoring a whole damaged span - can move a great many bits at
 * once. A sector of filler with seventy bytes of burst damage needs well
 * over a hundred. */
#define DR_MAX_WEIGHT 256
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

	/* A cell-phase correction: from cell `slip_at` of the message
	 * onward, the decoder was `slip` cells out of step. Every byte
	 * after the slip decodes differently, which is why this cannot be
	 * expressed as a list of bit flips. */
	int     slip_at;
	int     slip;

	dr_mode origin;                /* which engine proposed it        */

	/* How much likelier this reading's data is under the sector's own
	 * statistics than what was decoded (nats; higher is better). */
	double  data_prior;
	int     restores;              /* 0->1: puts back a lost reversal */
	int     removes;               /* 1->0: deletes a spurious one    */

	/* How many of the flipped bits land where the flux said something
	 * was wrong. Errors do not fall in places the timings call clean:
	 * across every repair on these disks that has a verifiable truth,
	 * every bit of it was inside the span the flux had already
	 * flagged. A candidate that mends a byte the timings are certain
	 * about is mending the CRC, not the disk. */
	int     in_damage;
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

	/* Pattern engine bookkeeping. */
	int           pattern;          /* results came from the data model*/
	int           revs;             /* ...or from the dump's own passes*/
	int           contested;        /* reversals the passes disputed   */
	int           majority;         /* of those, overruled by the many */
	int           crc_contested;    /* ...that land in the CRC bytes    */
	int           slip;             /* cells of phase the repair undoes */
	int           slip_byte;        /* ...from this message byte on     */
	int           crc_fixed;        /* stored-CRC bits it had to correct*/
	int           damage_bytes;     /* bytes the flux flags as uncertain*/
	int           period;           /* the repeat it locked onto       */
	int           outliers;         /* bytes that break the pattern    */
	double        coverage;         /* fraction of bytes on-pattern    */
	char          note[256];
} dr_repair_result;



/*
 * What the automatic second re-binning pass uses when the first finds
 * nothing: a prior that expects the disturbance to have drifted in and
 * out smoothly, and a list long enough to reach the reading that did.
 * Both are measured rather than guessed: see "A disturbance that is
 * spread out" in README.md.
 */
/* Whole-disk variants the tool will write in one run. */
#define DR_VARIANTS_MAX   16

#define DR_SMOOTH_SPREAD  16.0
#define DR_WIDTH_SPREAD   20000

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
	                        /* - the enumeration goes at least this   */
	                        /* deep, so one knob sets both            */
	int     max_outliers;   /* pattern engine: bytes off-pattern      */
	double  dropout_bias;   /* nats favouring a lost 1 over a gained 1*/
	int     restore_only;   /* only consider putting reversals back   */
	double  burst_gain;     /* error odds multiplier right after an   */
	                        /* error - 0 disables the burst prior     */
	double  burst_len;      /* how fast that decays, in message bits  */
	int     crc_budget;     /* stored-CRC bits a repair may correct,  */
	                        /* when the damage reaches them (2)       */
	double  smooth;         /* how hard to insist that whatever moved */
	                        /* the reversals moved them gradually: a  */
	                        /* speck, a scratch and the reader's own  */
	                        /* PLL all act over a stretch of track,   */
	                        /* never on one reversal. 0 turns it off. */
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

/* Run every engine that applies and rank their candidates together on
 * one scale, rather than taking whichever answers first. */
int         dr_repair_auto(dr_ctx *c, dr_view *v, const dr_options *o,
                           dr_repair_result *out);

/* Occam's razor: most sector data is not random. Find the pattern the
 * field almost obeys, and see whether restoring it satisfies the CRC. */
int         dr_pattern_search(dr_view *v, const dr_options *o,
                              dr_repair_result *out);

/* Which regularity the data field turned out to obey. */
typedef enum {
	DR_FIT_NONE = 0,
	DR_FIT_PERIODIC,        /* byte[i] == modal[i mod period]         */
	DR_FIT_COUNTER          /* fixed-size records counting by a step  */
} dr_fit_kind;

/* Describe the regularity found, for `inspect`. */
typedef struct {
	dr_fit_kind kind;
	int    period;          /* periodic fit: the repeat length        */
	int    rec, phase;      /* counter fit: record size and alignment */
	int    big_endian;
	uint64_t step, v0;
	int    outliers;        /* bytes the model does not explain       */
	int    explained;
	double coverage;
	int    distinct;        /* distinct byte values in the field      */
	int    top_value;
	int    top_count;
	char   desc[160];
} dr_pattern_info;

int         dr_pattern_analyse(const dr_view *v, dr_pattern_info *info);
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

/* Deliberately corrupt decoded data bits - used to build test images.
 * With drop_only, a bit is touched only if it currently reads 1, so the
 * damage is a lost reversal: the failure real media actually produces. */
int         dr_damage_slip(dr_ctx *c, int sector_index, int at_byte,
                           int cells);
int         dr_damage(dr_ctx *c, int sector_index, const int *bits, int nbits,
                      int drop_only);


/* ------------------------------------------------------------------ */
/* The filesystem on top of the sectors                                */
/* ------------------------------------------------------------------ */
/* A CRC is sixteen bits of evidence about five hundred and twelve
 * bytes, and on these disks it is often itself inside the damage. The
 * filesystem above it carries far stronger evidence, and it is evidence
 * nobody has been using: a FAT is stored twice, and a ZIP, a PNG or a
 * gzip member carries a 32-bit checksum of its own contents. Where a
 * bad sector lands decides which of those referees applies - and
 * whether anything was lost at all, since a sector in free space holds
 * no file's data. */

typedef enum {
	DR_AREA_UNKNOWN = 0,
	DR_AREA_BOOT,
	DR_AREA_FAT,
	DR_AREA_ROOT,
	DR_AREA_FILE,            /* allocated to a file                  */
	DR_AREA_FREE,            /* in the data area, owned by nothing   */
	DR_AREA_OUTSIDE          /* past the end of the filesystem       */
} dr_fs_area;

typedef struct dr_fs dr_fs;

typedef struct {
	int   present;
	int   guessed;           /* layout rebuilt - no readable boot    */
	char  kind[64];          /* "FAT12" / "FAT16"                    */
	char  oem[12];
	int   bps, spc, reserved, nfats, root_entries;
	long  total_sectors;
	int   fat_sectors, spt, heads;
	long  root_lba, data_lba;
	int   clusters;
	int   nfiles;
	long  fat_mismatch;      /* bytes where the FAT copies differ    */
	int   sectors_read;      /* sectors the assembly actually got    */
	int   sectors_bad;       /* ...of which had a bad CRC            */
} dr_fs_info;

typedef struct {
	dr_fs_area area;
	long  lba;
	int   fat_copy;          /* 1-based; FAT area only               */
	int   fat_rel;           /* sector index within that FAT         */
	int   mirror_sector;     /* sector index of the other copy, or -1*/
	int   mirror_clean;      /* ...and whether that one reads clean  */
	int   cluster;
	char  file[72];          /* owning file, "" when none            */
	long  file_offset;       /* byte offset of this sector in it     */
	long  file_size;
	char  note[200];
} dr_fs_loc;

/* What an independent, above-the-sector check made of a payload. */
typedef struct {
	int    checked;          /* a real check applied                 */
	int    proven;           /* an independent checksum matched      */
	int    refuted;          /* ...or definitely did not            */
	double score;            /* 0..1, for ranking variants           */
	char   how[256];
} dr_fs_verdict;

dr_fs      *dr_fs_open(dr_ctx *c);
void        dr_fs_free(dr_fs *fs);
const dr_fs_info *dr_fs_stat(const dr_fs *fs);

/* Where does this sector sit in the filesystem, and what is above it? */
int         dr_fs_locate(dr_fs *fs, dr_ctx *c, int sector_index,
                         dr_fs_loc *out);

/* The other FAT's copy of a FAT sector - exact redundancy, already on
 * the disk. Returns NULL unless the sector is in a FAT and the mirror
 * read cleanly. */
const uint8_t *dr_fs_mirror(dr_fs *fs, const dr_fs_loc *loc);

/* Judge a candidate payload for this sector by the file that owns it:
 * a ZIP entry's CRC-32, a gzip member's, a directory's structure, a
 * FAT's own consistency. Far stronger than the sector CRC when it
 * applies, and the only referee at all when the sector CRC is damaged. */
/* The same bytes, stored a second time somewhere else on this disk.
 * Backup archives are written twice as often as anyone expects: the
 * same ZIP entry, the same name, the same packed length and the same
 * CRC-32, sitting in a second archive a few hundred tracks away. When
 * one copy is under the damaged sector and the other is not, the
 * sector's contents are not a guess - and the archive's own CRC-32
 * says so, with thirty-two bits rather than sixteen.
 *
 * Fills `out` with this sector's `len` true bytes. Returns 1 when the
 * archive's CRC-32 confirms them, 0 when a copy was found but does not
 * check out, -1 when there is no second copy. */
/* One line on where inside its file the sector actually sits - the zip
 * entry, or the stream of a compound document and the storage above it.
 * A PowerPoint file holds the same stream names twice, once live and
 * once in the copy kept for PowerPoint 95, and which of the two is
 * damaged is the difference between a lost presentation and a lost
 * compatibility copy. Returns 0 when it has something to say. */
int         dr_fs_detail(dr_fs *fs, const dr_fs_loc *loc,
                         char *buf, int n);

/* A copy of the same file from somewhere else entirely - another disk,
 * an archive, a download. It will not be laid out identically: builds
 * differ, and even the same file can sit at a different offset. So the
 * sector's known-good neighbours are used as an anchor: find where they
 * occur in the candidate, and require a long run of agreement on both
 * sides before believing the bytes in between. The sector's own CRC
 * then says whether it is right.
 *
 * Returns 1 when the copy agrees for thousands of bytes either side -
 * the same build, byte for byte, so the 512 in the middle are not a
 * guess even if the sector's own checksum disagrees; 0 when it lines up
 * well enough to be worth checking against that checksum; -1 when it
 * does not line up at all. */
int         dr_fs_from_file(dr_fs *fs, const dr_fs_loc *loc,
                            const char *path, uint8_t *out, int len,
                            char *how, int howsz);

/* Per-file damage report: which of the disk's files a bad sector
 * touched, and - where the format carries checksums of its own - how
 * much of each one still comes out. The point of the exercise is not
 * how many sectors read, it is which files the owner still has. */
typedef struct {
	char name[72];
	long size;
	int  deleted;            /* the directory entry was erased       */
	long chain_bytes;        /* what its cluster chain actually spans*/
	int  reused;             /* deleted: clusters a live file took   */
	int  bad;                /* sectors of it with a CRC error       */
	int  parts, parts_ok;    /* archive members that still verify    */
	int  layout;             /* archive entries its directory confirms*/
	int  found;              /* ...recovered via a second directory   */
	char lost[160];          /* members nothing on the disk can supply*/
	char cross[72];          /* another file claiming the same space  */
	char note[400];
} dr_fs_file;

int         dr_fs_files(dr_fs *fs, dr_fs_file *out, int max);

/* Hand back a file's bytes - live or deleted - so they can be written
 * out. `index` is its position in the dr_fs_files() listing. The caller
 * frees. */
uint8_t    *dr_fs_read(dr_fs *fs, int index, long *len);

/* One member written out of a damaged archive. */
typedef struct {
	char name[2400];        /* the path it was written to           */
	long size;              /* bytes written                        */
	long full;              /* bytes it should have had             */
	int  whole;             /* its own CRC-32 agrees                */
	char note[200];
} dr_fs_member;

int         dr_fs_salvage(dr_fs *fs, int index, const char *dir,
                          dr_fs_member *out, int max);

int         dr_fs_sister(dr_fs *fs, const dr_fs_loc *loc,
                         uint8_t *out, int len, char *how, int howsz);

int         dr_fs_score(dr_fs *fs, const dr_fs_loc *loc,
                        const uint8_t *payload, int len,
                        dr_fs_verdict *out);

/* Overwrite a sector's data field with known-good bytes and re-stamp
 * its CRC. Used when the truth came from somewhere else entirely - the
 * other FAT, or the same file archived twice on the same disk. */
int         dr_set_data(dr_ctx *c, dr_view *v, const uint8_t *data, int len);

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
