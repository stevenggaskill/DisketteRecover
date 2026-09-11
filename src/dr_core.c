/*
 * DisketteRecover - image context, sector scanning and cell patching.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "dr_internal.h"

/* ------------------------------------------------------------------ */
/* libhxcfe chatter                                                    */
/* ------------------------------------------------------------------ */
static int g_verbose;

/*
 * libhxcfe narrates its own decoding, and on a flux dump that means a
 * screenful of "Invalid rpm or tracklen" for every track whose index
 * timing it does not like - none of which the caller can act on, and all
 * of which it recovers from by falling back to 300 RPM. Real failures
 * reach the caller through dr_last_error() instead, so this is silent
 * unless -v asks for it.
 */
static int32_t dr_printf(int32_t MSGTYPE, const char *chaine, ...)
{
	va_list ap;

	if (!g_verbose)
		return 0;
	if (g_verbose < 2 && MSGTYPE == MSG_DEBUG)
		return 0;

	va_start(ap, chaine);
	fprintf(stderr, "hxcfe: ");
	vfprintf(stderr, chaine, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	return 0;
}

static void dr_err(dr_ctx *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(c->error, sizeof(c->error), fmt, ap);
	va_end(ap);
}

const char *dr_last_error(dr_ctx *c) { return c ? c->error : "no context"; }
const char *dr_path(dr_ctx *c)       { return c ? c->path : ""; }

void dr_options_default(dr_options *o)
{
	o->good_threshold = 1e-3;
	o->base_perr      = 2e-4;
	o->jitter         = 0.16;
	o->max_weight     = 3;
	o->max_pool       = 96;
	o->max_results    = 256;

	o->mode           = DR_MODE_AUTO;
	o->bin_budget     = 12.0;   /* nats; ~e^-12 relative likelihood   */
	o->max_explore    = 500000;
	o->max_ambiguous  = 48;
	o->rebin_width    = 24;
	o->max_outliers   = 24;
	o->dropout_bias   = 1.6;
	o->burst_gain     = 110.0;
	o->burst_len      = 60.0;
	o->crc_budget     = 2;
}

/* ------------------------------------------------------------------ */
static int dr_set_env(dr_ctx *c, const char *assignment)
{
	char name[128];
	const char *eq;
	size_t n;

	if (!c || !c->hxcfe || !assignment)
		return -1;

	eq = strchr(assignment, '=');
	if (!eq)
		return -1;
	n = (size_t)(eq - assignment);
	if (n == 0 || n >= sizeof(name))
		return -1;
	memcpy(name, assignment, n);
	name[n] = 0;

	return hxcfe_setEnvVar(c->hxcfe, name, (char *)(eq + 1));
}

dr_ctx *dr_open(const char *path, int verbose)
{
	return dr_open_ex(path, verbose, NULL, 0);
}

dr_ctx *dr_open_ex(const char *path, int verbose,
                   char *const *sets, int nsets)
{
	dr_ctx *c;
	int32_t err = 0;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->verbose = verbose;
	g_verbose = verbose;
	snprintf(c->path, sizeof(c->path), "%s", path);

	c->hxcfe = hxcfe_init();
	if (!c->hxcfe) {
		dr_err(c, "hxcfe_init() failed");
		return c;
	}
	hxcfe_setOutputFunc(c->hxcfe, dr_printf);

	/* Loader and PLL settings are read while the image is decoded, so
	 * they have to be in place before the load. */
	{
		int i;
		for (i = 0; i < nsets; i++) {
			if (dr_set_env(c, sets[i]) < 0)
				fprintf(stderr, "warning: could not set '%s'\n",
				        sets[i]);
		}
	}

	c->loader = hxcfe_imgInitLoader(c->hxcfe);
	if (!c->loader) {
		dr_err(c, "hxcfe_imgInitLoader() failed");
		return c;
	}

	/* Auto-detection scans forward from a module index, so it has to
	 * start at 0 - passing -1 walks off the front of the table. */
	c->loader_id = hxcfe_imgAutoSetectLoader(c->loader, (char *)path, 0);
	if (c->loader_id < 0) {
		dr_err(c, "no libhxcfe loader accepted '%s'", path);
		return c;
	}

	c->floppy = hxcfe_imgLoad(c->loader, (char *)path, c->loader_id, &err);
	if (!c->floppy) {
		dr_err(c, "load failed (hxcfe error %d)", (int)err);
		return c;
	}

	c->sacc = hxcfe_initSectorAccess(c->hxcfe, c->floppy);
	if (!c->sacc)
		dr_err(c, "hxcfe_initSectorAccess() failed");

	return c;
}

void dr_close(dr_ctx *c)
{
	if (!c)
		return;
	if (c->sacc)
		hxcfe_deinitSectorAccess(c->sacc);
	if (c->floppy && c->loader)
		hxcfe_imgUnload(c->loader, c->floppy);
	if (c->loader)
		hxcfe_imgDeInitLoader(c->loader);
	dr_model_free(c->model);
	if (c->hxcfe)
		hxcfe_deinit(c->hxcfe);
	free(c->sectors);
	free(c);
}

int dr_tracks(dr_ctx *c)
{
	return (c && c->floppy) ? hxcfe_getNumberOfTrack(c->hxcfe, c->floppy) : 0;
}

int dr_sides(dr_ctx *c)
{
	return (c && c->floppy) ? hxcfe_getNumberOfSide(c->hxcfe, c->floppy) : 0;
}

HXCFE_SIDE *dr_side(dr_ctx *c, int track, int side)
{
	if (!c || !c->floppy)
		return NULL;
	return hxcfe_getSide(c->hxcfe, c->floppy, track, side);
}

/* ------------------------------------------------------------------ */
/* Decoding helpers                                                    */
/* ------------------------------------------------------------------ */

/* MFM: 16 cells per byte, pairs of (clock,data); a decoded 1 needs a
 * clear clock cell and a set data cell. */
static void mfm_decode(const HXCFE_SIDE *s, int cell, uint8_t *out, int len)
{
	int i, k, c1, c2;

	for (i = 0; i < len; i++) {
		uint8_t v = 0;
		for (k = 0; k < 8; k++) {
			c1 = dr_getcell(s, cell + i * 16 + k * 2);
			c2 = dr_getcell(s, cell + i * 16 + k * 2 + 1);
			v = (uint8_t)(v << 1);
			if (!c1 && c2)
				v |= 1;
		}
		out[i] = v;
	}
}

/* FM as stored by libhxcfe: 32 cells per byte, groups of 8 cells laid
 * out 0 C 0 D 0 C 0 D, so two data bits live at +3 and +7. */
static void fm_decode(const HXCFE_SIDE *s, int cell, uint8_t *out, int len)
{
	int i, k;

	for (i = 0; i < len; i++) {
		uint8_t v = 0;
		for (k = 0; k < 8; k++) {
			int off = cell + i * 32 + (k >> 1) * 8 + ((k & 1) ? 7 : 3);
			v = (uint8_t)(v << 1);
			if (dr_getcell(s, off))
				v |= 1;
		}
		out[i] = v;
	}
}

int dr_mfm_decode(const HXCFE_SIDE *s, int cell, uint8_t *out, int len)
{
	mfm_decode(s, cell, out, len);
	return 0;
}

void dr_decode(const HXCFE_SIDE *s, dr_encoding enc, int cell,
               uint8_t *out, int len)
{
	if (enc == DR_ENC_ISO_FM)
		fm_decode(s, cell, out, len);
	else
		mfm_decode(s, cell, out, len);
}

int dr_byte_stride(dr_encoding enc)
{
	return (enc == DR_ENC_ISO_FM) ? 32 : 16;
}

/* Cell offsets carrying message bit `bit` (0 = MSB) of the byte that
 * starts at `base`. */
void dr_bit_cells(dr_encoding enc, int base, int bit, int *clock, int *data)
{
	if (enc == DR_ENC_ISO_FM) {
		int g = base + (bit >> 1) * 8;
		*clock = g + ((bit & 1) ? 5 : 1);
		*data  = g + ((bit & 1) ? 7 : 3);
	} else {
		*clock = base + bit * 2;
		*data  = base + bit * 2 + 1;
	}
}

/* Rewrite one byte, regenerating legal clock cells around it. */
static void mfm_write_byte(HXCFE_SIDE *s, int base, uint8_t value)
{
	int k, prev;

	/* Previous data bit: the data cell just before this byte. */
	prev = dr_getcell(s, base - 1);

	for (k = 0; k < 8; k++) {
		int bit = (value >> (7 - k)) & 1;
		dr_setcell(s, base + k * 2, (!prev && !bit) ? 1 : 0);
		dr_setcell(s, base + k * 2 + 1, bit);
		prev = bit;
	}

	/* The following byte's first clock cell depends on our last bit. */
	{
		int nb = base + 16;
		int nbit = dr_getcell(s, nb + 1);
		dr_setcell(s, nb, (!prev && !nbit) ? 1 : 0);
	}
}

static void fm_write_byte(HXCFE_SIDE *s, int base, uint8_t value)
{
	int k;

	for (k = 0; k < 8; k++) {
		int g = base + (k >> 1) * 8;
		int co = g + ((k & 1) ? 5 : 1);
		int dof = g + ((k & 1) ? 7 : 3);
		dr_setcell(s, co, 1);                       /* FM clock */
		dr_setcell(s, dof, (value >> (7 - k)) & 1);
	}
}

void dr_write_byte(HXCFE_SIDE *s, dr_encoding enc, int base, uint8_t value)
{
	if (enc == DR_ENC_ISO_FM)
		fm_write_byte(s, base, value);
	else
		mfm_write_byte(s, base, value);
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */
/* ------------------------------------------------------------------ */
static dr_crc_state crc_state(int32_t use_alternate)
{
	return use_alternate ? DR_CRC_BAD : DR_CRC_OK;
}

static int scan_track(dr_ctx *c, int track, int side, int enc_id,
                      dr_encoding enc, int *order)
{
	HXCFE_SECTCFG **list;
	int32_t n = 0;
	int i, added = 0;

	list = hxcfe_getAllTrackSectors(c->sacc, track, side, enc_id, &n);
	if (!list)
		return 0;

	for (i = 0; i < n; i++) {
		HXCFE_SECTCFG *sc = list[i];
		dr_sector *d;

		if (!sc)
			continue;

		c->sectors = realloc(c->sectors,
		                     (size_t)(c->nsectors + 1) * sizeof(dr_sector));
		d = &c->sectors[c->nsectors++];
		memset(d, 0, sizeof(*d));

		d->track       = track;
		d->side        = side;
		d->order       = (*order)++;
		d->cylinder_id = sc->cylinder;
		d->head_id     = sc->head;
		d->sector_id   = sc->sector;
		d->size_id     = sc->alternate_sector_size_id;
		d->sector_size = sc->sectorsize;
		d->encoding    = enc;
		d->bitrate     = sc->bitrate;
		d->header_crc  = crc_state(sc->use_alternate_header_crc);
		d->stored_header_crc = sc->header_crc;
		d->stored_data_crc   = sc->data_crc;
		d->start_cell  = sc->startsectorindex;
		d->data_cell   = sc->startdataindex;
		d->end_cell    = sc->endsectorindex;
		d->datamark    = sc->use_alternate_datamark ? sc->alternate_datamark : 0;

		if (!sc->use_alternate_datamark && enc == DR_ENC_ISO_MFM)
			d->data_crc = DR_CRC_ABSENT;
		else if (sc->startdataindex == sc->endsectorindex)
			d->data_crc = DR_CRC_ABSENT;
		else
			d->data_crc = crc_state(sc->use_alternate_data_crc);

		added++;
	}

	for (i = 0; i < n; i++)
		hxcfe_freeSectorConfig(c->sacc, list[i]);
	free(list);

	return added;
}

int dr_scan(dr_ctx *c)
{
	int t, s, nt, ns, order;

	if (!c || !c->floppy || !c->sacc) {
		if (c && !c->error[0])
			dr_err(c, "no floppy loaded");
		return -1;
	}

	free(c->sectors);
	c->sectors = NULL;
	c->nsectors = 0;

	nt = hxcfe_getNumberOfTrack(c->hxcfe, c->floppy);
	ns = hxcfe_getNumberOfSide(c->hxcfe, c->floppy);

	for (t = 0; t < nt; t++) {
		for (s = 0; s < ns; s++) {
			order = 0;
			if (!scan_track(c, t, s, ISOIBM_MFM_ENCODING,
			                DR_ENC_ISO_MFM, &order))
				scan_track(c, t, s, ISOIBM_FM_ENCODING,
				           DR_ENC_ISO_FM, &order);
		}
	}

	c->scanned = 1;
	return c->nsectors;
}

const dr_sector *dr_sectors(dr_ctx *c, int *n)
{
	if (n)
		*n = c ? c->nsectors : 0;
	return c ? c->sectors : NULL;
}

int dr_first_bad(dr_ctx *c)
{
	int i;

	for (i = 0; i < c->nsectors; i++) {
		if (c->sectors[i].header_crc == DR_CRC_BAD ||
		    c->sectors[i].data_crc == DR_CRC_BAD)
			return i;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Verification / export                                               */
/* ------------------------------------------------------------------ */
int dr_verify(dr_ctx *c, int sector_index)
{
	dr_sector want;
	int rc = -1, i, n;

	if (sector_index < 0 || sector_index >= c->nsectors)
		return -1;

	want = c->sectors[sector_index];

	/* The sector-access context caches per-track results, so drop the
	 * cache and let libhxcfe decode the patched cells from scratch. */
	hxcfe_clearTrackCache(c->sacc);
	hxcfe_resetSearchTrackPosition(c->sacc);

	if (dr_scan(c) < 0)
		return -1;

	n = c->nsectors;
	for (i = 0; i < n; i++) {
		dr_sector *d = &c->sectors[i];
		if (d->track == want.track && d->side == want.side &&
		    d->sector_id == want.sector_id) {
			rc = (d->header_crc != DR_CRC_BAD &&
			      d->data_crc != DR_CRC_BAD) ? 1 : 0;
			break;
		}
	}
	return rc;
}

int dr_export(dr_ctx *c, const char *path, const char *format)
{
	int32_t id = -1;
	int i, n;

	if (!c->floppy) {
		dr_err(c, "nothing to export");
		return -1;
	}

	if (!format || !*format)
		format = "HXC_HFE";

	/* Exact loader name first, then a case-insensitive match on either
	 * the loader name or its extension, so "hfe" or "img" work too. */
	id = hxcfe_imgGetLoaderID(c->loader, (char *)format);

	if (id < 0) {
		n = hxcfe_imgGetNumberOfLoader(c->loader);
		for (i = 0; i < n && id < 0; i++) {
			const char *nm = hxcfe_imgGetLoaderName(c->loader, i);
			if (!(hxcfe_imgGetLoaderAccess(c->loader, i) & 2))
				continue;
			if (nm && !strcasecmp(nm, format))
				id = i;
		}
		for (i = 0; i < n && id < 0; i++) {
			const char *ex = hxcfe_imgGetLoaderExt(c->loader, i);
			if (!(hxcfe_imgGetLoaderAccess(c->loader, i) & 2))
				continue;
			if (ex && !strcasecmp(ex, format))
				id = i;
		}
	}

	if (id < 0) {
		dr_err(c, "unknown export format '%s' (see `disketterecover formats`)",
		       format);
		return -1;
	}

	if (hxcfe_imgExport(c->loader, c->floppy, (char *)path, id) != HXCFE_NOERROR) {
		dr_err(c, "export to '%s' failed", path);
		return -1;
	}
	return 0;
}
