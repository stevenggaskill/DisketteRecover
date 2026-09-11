/*
 * DisketteRecover - what sits above the sector.
 *
 * A sector CRC is sixteen bits of evidence about five hundred and
 * twelve bytes, and on damaged media it is routinely inside the damage
 * itself: on every disk here where a mark ran to the end of a sector,
 * it swallowed the CRC on its way past. Something stronger is needed,
 * and it has been sitting on the disk the whole time.
 *
 *   - A FAT is written twice. If one copy reads cleanly, the other's
 *     contents are not a guess.
 *   - A ZIP entry carries a CRC-32 of its uncompressed contents, and a
 *     deflate stream that has been touched almost never inflates at
 *     all. Thirty-two bits, checked against the data the user actually
 *     cares about, beats sixteen bits checked against the sector.
 *   - A directory entry has a shape, and a cluster chain has to close.
 *   - And a sector in free space holds no file's data at all, so there
 *     is nothing there to recover.
 *
 * That last one is not a small point. Across the disks tested, several
 * unrepairable sectors turned out to belong to no file: the disk was
 * already whole.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dr_internal.h"

#ifndef DR_NO_ZLIB
#include <zlib.h>
#endif

#define MAX_LBA   4096
#define SECSZ     512

struct dr_fs {
	dr_fs_info info;

	uint8_t  *img;          /* assembled sector image               */
	uint8_t  *have;         /* 1 = decoded, 2 = decoded but bad CRC  */
	int      *owner;        /* LBA -> sector index in the scan       */
	long      nlba;

	/* Root directory, flattened. */
	struct dr_fs_file {
		char  name[72];
		int   start;
		long  size;
		int   attr;
	} *files;
	int nfiles;
};

/* ---------------------------------------------------------------- */
/* Assembling the sectors into an image                              */
/* ---------------------------------------------------------------- */

static int u16le(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* libhxcfe hands sectors back per track; collect them all once. */
static void collect(dr_ctx *c, dr_fs *fs, int spt, int heads)
{
	HXCFE_SECTORACCESS *sa = c->sacc;
	int nt = hxcfe_getNumberOfTrack(c->hxcfe, c->floppy);
	int ns = hxcfe_getNumberOfSide(c->hxcfe, c->floppy);
	int t, s, i;

	for (t = 0; t < nt; t++) {
		for (s = 0; s < ns; s++) {
			HXCFE_SECTCFG **list;
			int32_t n = 0;

			list = hxcfe_getAllTrackSectors(sa, t, s,
			                                ISOIBM_MFM_ENCODING, &n);
			if (!list)
				continue;
			for (i = 0; i < n; i++) {
				HXCFE_SECTCFG *sc = list[i];
				long lba;

				if (sc && sc->input_data &&
				    sc->sectorsize == SECSZ &&
				    sc->sector >= 1 && sc->sector <= spt) {
					lba = ((long)t * heads + s) * spt +
					      (sc->sector - 1);
					if (lba >= 0 && lba < fs->nlba &&
					    !fs->have[lba]) {
						memcpy(fs->img + lba * SECSZ,
						       sc->input_data, SECSZ);
						fs->have[lba] =
						        sc->use_alternate_data_crc ? 2 : 1;
						fs->info.sectors_read++;
						if (sc->use_alternate_data_crc)
							fs->info.sectors_bad++;
					}
				}
				hxcfe_freeSectorConfig(sa, sc);
			}
			free(list);
		}
	}
	hxcfe_clearTrackCache(sa);
	hxcfe_resetSearchTrackPosition(sa);
}

/* Read one sector straight out of libhxcfe, by physical address. */
static int read_one(dr_ctx *c, int track, int side, int id, uint8_t *out)
{
	HXCFE_SECTORACCESS *sa = c->sacc;
	HXCFE_SECTCFG **list;
	int32_t n = 0;
	int i, got = 0;

	list = hxcfe_getAllTrackSectors(sa, track, side,
	                                ISOIBM_MFM_ENCODING, &n);
	if (!list)
		return -1;
	for (i = 0; i < n; i++) {
		HXCFE_SECTCFG *sc = list[i];
		if (!got && sc && sc->input_data && sc->sector == id &&
		    sc->sectorsize == SECSZ) {
			memcpy(out, sc->input_data, SECSZ);
			got = 1;
		}
		hxcfe_freeSectorConfig(sa, sc);
	}
	free(list);
	hxcfe_clearTrackCache(sa);
	hxcfe_resetSearchTrackPosition(sa);
	return got ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* FAT12/16                                                          */
/* ---------------------------------------------------------------- */

static int fat_get(const dr_fs *fs, int copy, int n)
{
	long base = (long)(fs->info.reserved + copy * fs->info.fat_sectors) * SECSZ;
	long len = (long)fs->info.fat_sectors * SECSZ;
	const uint8_t *f = fs->img + base;

	if (n < 0)
		return 0;
	if (strcmp(fs->info.kind, "FAT16") == 0) {
		long i = (long)n * 2;
		if (i + 1 >= len)
			return 0;
		return u16le(f + i);
	} else {
		long i = ((long)n * 3) / 2;
		int v;
		if (i + 1 >= len)
			return 0;
		v = f[i] | (f[i + 1] << 8);
		return (n & 1) ? (v >> 4) : (v & 0xFFF);
	}
}

static int fat_eoc(const dr_fs *fs, int v)
{
	if (strcmp(fs->info.kind, "FAT16") == 0)
		return v >= 0xFFF8 || v < 2;
	return v >= 0xFF8 || v < 2;
}

/* Walk a cluster chain, stopping on a loop or an impossible link. */
static int chain(const dr_fs *fs, int start, int *out, int max)
{
	int n = 0, cl = start;
	int guard = 0;

	while (n < max && cl >= 2 && cl < fs->info.clusters + 2 &&
	       guard++ < 65536) {
		int i, seen = 0;
		for (i = 0; i < n; i++)
			if (out[i] == cl) { seen = 1; break; }
		if (seen)
			break;
		out[n++] = cl;
		cl = fat_get(fs, 0, cl);
		if (fat_eoc(fs, cl))
			break;
	}
	return n;
}

static void trim_name(const uint8_t *e, char *out)
{
	char base[9], ext[4];
	int i, n;

	for (i = 0, n = 0; i < 8; i++)
		if (e[i] != ' ')
			base[n++] = (char)e[i];
	base[n] = 0;
	for (i = 0, n = 0; i < 3; i++)
		if (e[8 + i] != ' ')
			ext[n++] = (char)e[8 + i];
	ext[n] = 0;
	if (ext[0])
		snprintf(out, 72, "%s.%s", base, ext);
	else
		snprintf(out, 72, "%s", base);
}

static void read_root(dr_fs *fs)
{
	long off = fs->info.root_lba * SECSZ;
	int i;

	fs->files = calloc((size_t)fs->info.root_entries, sizeof(*fs->files));
	if (!fs->files)
		return;
	for (i = 0; i < fs->info.root_entries; i++) {
		const uint8_t *e = fs->img + off + (long)i * 32;

		if (e[0] == 0x00)
			break;
		if (e[0] == 0xE5 || e[11] == 0x0F)
			continue;          /* deleted, or a long-name slot */
		if (e[11] & 0x08)
			continue;          /* volume label                 */
		trim_name(e, fs->files[fs->nfiles].name);
		fs->files[fs->nfiles].start = u16le(e + 26);
		fs->files[fs->nfiles].size =
		        (long)e[28] | ((long)e[29] << 8) |
		        ((long)e[30] << 16) | ((long)e[31] << 24);
		fs->files[fs->nfiles].attr = e[11];
		if (fs->files[fs->nfiles].start >= 2)
			fs->nfiles++;
	}
	fs->info.nfiles = fs->nfiles;
}

static void count_fat_mismatch(dr_fs *fs)
{
	long len = (long)fs->info.fat_sectors * SECSZ;
	long i;
	const uint8_t *a, *b;

	if (fs->info.nfats < 2)
		return;
	a = fs->img + (long)fs->info.reserved * SECSZ;
	b = a + len;
	for (i = 0; i < len; i++)
		if (a[i] != b[i])
			fs->info.fat_mismatch++;
}

dr_fs *dr_fs_open(dr_ctx *c)
{
	dr_fs *fs;
	uint8_t boot[SECSZ];
	dr_fs_info *in;
	long need;

	if (!c || !c->floppy)
		return NULL;
	if (read_one(c, 0, 0, 1, boot) != 0)
		return NULL;

	fs = calloc(1, sizeof(*fs));
	if (!fs)
		return NULL;
	in = &fs->info;

	in->bps          = u16le(boot + 11);
	in->spc          = boot[13];
	in->reserved     = u16le(boot + 14);
	in->nfats        = boot[16];
	in->root_entries = u16le(boot + 17);
	in->total_sectors= u16le(boot + 19);
	in->fat_sectors  = u16le(boot + 22);
	in->spt          = u16le(boot + 24);
	in->heads        = u16le(boot + 26);
	memcpy(in->oem, boot + 3, 8);
	in->oem[8] = 0;

	if (in->bps != SECSZ || in->spc < 1 || in->spc > 64 ||
	    in->reserved < 1 || in->nfats < 1 || in->nfats > 2 ||
	    in->fat_sectors < 1 || in->fat_sectors > 256 ||
	    in->root_entries < 16 || in->root_entries > 2048 ||
	    in->spt < 1 || in->spt > 64 || in->heads < 1 || in->heads > 2 ||
	    in->total_sectors < 16) {
		free(fs);
		return NULL;
	}

	in->root_lba = in->reserved + (long)in->nfats * in->fat_sectors;
	in->data_lba = in->root_lba +
	               (((long)in->root_entries * 32 + SECSZ - 1) / SECSZ);
	in->clusters = (int)((in->total_sectors - in->data_lba) / in->spc);
	snprintf(in->kind, sizeof(in->kind), "%s",
	         in->clusters < 4085 ? "FAT12" : "FAT16");

	need = in->total_sectors;
	if (need > MAX_LBA)
		need = MAX_LBA;
	fs->nlba = need;
	fs->img  = calloc((size_t)need, SECSZ);
	fs->have = calloc((size_t)need, 1);
	fs->owner = malloc(sizeof(int) * (size_t)need);
	if (!fs->img || !fs->have || !fs->owner) {
		dr_fs_free(fs);
		return NULL;
	}
	{
		long i;
		for (i = 0; i < need; i++)
			fs->owner[i] = -1;
	}

	collect(c, fs, in->spt, in->heads);
	count_fat_mismatch(fs);
	read_root(fs);

	/* Map every scanned sector back to its LBA. */
	{
		int n = 0, i;
		const dr_sector *ss = dr_sectors(c, &n);
		for (i = 0; i < n; i++) {
			long lba;
			if (ss[i].sector_size != SECSZ ||
			    ss[i].sector_id < 1 || ss[i].sector_id > in->spt)
				continue;
			lba = ((long)ss[i].track * in->heads + ss[i].side) *
			              in->spt + (ss[i].sector_id - 1);
			if (lba >= 0 && lba < fs->nlba && fs->owner[lba] < 0)
				fs->owner[lba] = i;
		}
	}

	in->present = 1;
	return fs;
}

void dr_fs_free(dr_fs *fs)
{
	if (!fs)
		return;
	free(fs->files);
	free(fs->owner);
	free(fs->have);
	free(fs->img);
	free(fs);
}

const dr_fs_info *dr_fs_stat(const dr_fs *fs)
{
	return fs ? &fs->info : NULL;
}

/* ---------------------------------------------------------------- */
/* Where a sector sits                                               */
/* ---------------------------------------------------------------- */

int dr_fs_locate(dr_fs *fs, int sector_index, dr_fs_loc *out)
{
	const dr_fs_info *in;
	long lba = -1, i;

	if (!fs || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->mirror_sector = -1;
	in = &fs->info;

	for (i = 0; i < fs->nlba; i++)
		if (fs->owner[i] == sector_index) { lba = i; break; }
	if (lba < 0)
		return -1;
	out->lba = lba;

	if (lba >= in->total_sectors) {
		out->area = DR_AREA_OUTSIDE;
		snprintf(out->note, sizeof(out->note),
		         "past the end of the filesystem - not part of it");
		return 0;
	}
	if (lba < in->reserved) {
		out->area = DR_AREA_BOOT;
		snprintf(out->note, sizeof(out->note), "boot sector / BPB");
		return 0;
	}
	if (lba < in->reserved + (long)in->nfats * in->fat_sectors) {
		long rel = lba - in->reserved;
		out->area     = DR_AREA_FAT;
		out->fat_copy = (int)(rel / in->fat_sectors) + 1;
		out->fat_rel  = (int)(rel % in->fat_sectors);
		if (in->nfats > 1) {
			long other = in->reserved +
			             (long)(out->fat_copy == 1 ? 1 : 0) *
			             in->fat_sectors + out->fat_rel;
			if (other >= 0 && other < fs->nlba) {
				out->mirror_sector = fs->owner[other];
				out->mirror_clean  = (fs->have[other] == 1);
			}
		}
		snprintf(out->note, sizeof(out->note),
		         "FAT copy %d, sector %d of %d%s",
		         out->fat_copy, out->fat_rel, in->fat_sectors,
		         out->mirror_clean
		           ? " - the other copy of these same bytes reads clean"
		           : "");
		return 0;
	}
	if (lba < in->data_lba) {
		out->area = DR_AREA_ROOT;
		snprintf(out->note, sizeof(out->note),
		         "root directory, entries %ld..%ld",
		         (lba - in->root_lba) * 16, (lba - in->root_lba) * 16 + 15);
		return 0;
	}

	out->cluster = (int)((lba - in->data_lba) / in->spc) + 2;
	{
		int *cl = malloc(sizeof(int) * (size_t)(in->clusters + 2));
		int f;

		out->area = DR_AREA_FREE;
		if (cl) {
			for (f = 0; f < fs->nfiles; f++) {
				int n = chain(fs, fs->files[f].start, cl,
				              in->clusters + 2);
				int k;
				for (k = 0; k < n; k++) {
					if (cl[k] != out->cluster)
						continue;
					out->area = DR_AREA_FILE;
					snprintf(out->file, sizeof(out->file),
					         "%s", fs->files[f].name);
					out->file_size = fs->files[f].size;
					out->file_offset =
					        (long)k * in->spc * SECSZ +
					        ((lba - in->data_lba) % in->spc) * SECSZ;
					snprintf(out->note, sizeof(out->note),
					         "cluster %d - bytes %ld..%ld of "
					         "%s (%ld bytes)",
					         out->cluster, out->file_offset,
					         out->file_offset + SECSZ,
					         fs->files[f].name,
					         fs->files[f].size);
					break;
				}
				if (out->area == DR_AREA_FILE)
					break;
			}
			free(cl);
		}
		if (out->area == DR_AREA_FREE)
			snprintf(out->note, sizeof(out->note),
			         "cluster %d - free space: no file's data is "
			         "here, so nothing was lost", out->cluster);
	}
	return 0;
}

const uint8_t *dr_fs_mirror(dr_fs *fs, const dr_fs_loc *loc)
{
	long other;

	if (!fs || !loc || loc->area != DR_AREA_FAT || fs->info.nfats < 2)
		return NULL;
	if (!loc->mirror_clean)
		return NULL;
	other = fs->info.reserved +
	        (long)(loc->fat_copy == 1 ? 1 : 0) * fs->info.fat_sectors +
	        loc->fat_rel;
	if (other < 0 || other >= fs->nlba)
		return NULL;
	return fs->img + other * SECSZ;
}

/* ---------------------------------------------------------------- */
/* The referees: checks that live above the sector CRC               */
/* ---------------------------------------------------------------- */

/* Pull a file's bytes out of the image, with `payload` spliced in at
 * `at` - the candidate reading standing in for the sector. */
static uint8_t *assemble(dr_fs *fs, const dr_fs_loc *loc,
                         const uint8_t *payload, int len, long *out_len)
{
	const dr_fs_info *in = &fs->info;
	uint8_t *buf;
	int *cl, n, f, k;
	long size;

	for (f = 0; f < fs->nfiles; f++)
		if (strcmp(fs->files[f].name, loc->file) == 0)
			break;
	if (f == fs->nfiles)
		return NULL;

	size = fs->files[f].size;
	if (size <= 0 || size > 32L * 1024 * 1024)
		return NULL;

	cl = malloc(sizeof(int) * (size_t)(in->clusters + 2));
	if (!cl)
		return NULL;
	n = chain(fs, fs->files[f].start, cl, in->clusters + 2);

	buf = calloc((size_t)size + SECSZ, 1);
	if (!buf) {
		free(cl);
		return NULL;
	}
	for (k = 0; k < n; k++) {
		long src = (in->data_lba + (long)(cl[k] - 2) * in->spc) * SECSZ;
		long dst = (long)k * in->spc * SECSZ;
		long want = (long)in->spc * SECSZ;

		if (dst >= size)
			break;
		if (dst + want > size + SECSZ)
			want = size + SECSZ - dst;
		if (src + want <= fs->nlba * SECSZ)
			memcpy(buf + dst, fs->img + src, (size_t)want);
	}
	free(cl);

	if (payload && loc->file_offset >= 0 &&
	    loc->file_offset + len <= size + SECSZ)
		memcpy(buf + loc->file_offset, payload, (size_t)len);

	*out_len = size;
	return buf;
}

#ifndef DR_NO_ZLIB
/* Inflate a raw deflate stream and check it against the CRC-32 and the
 * uncompressed length the archive recorded for it. A deflate stream is
 * a chain of back-references: change one byte inside it and it almost
 * always stops being decodable at all, and if it does decode the
 * CRC-32 catches it. Thirty-two bits about the bytes that matter. */
static int inflate_check(const uint8_t *src, long csz,
                         unsigned long want_crc, long want_len)
{
	z_stream z;
	uint8_t out[16384];
	unsigned long crc = crc32(0L, Z_NULL, 0);
	long total = 0;
	int rc, done = 0;

	memset(&z, 0, sizeof(z));
	if (inflateInit2(&z, -15) != Z_OK)
		return -1;
	z.next_in = (Bytef *)src;
	z.avail_in = (uInt)csz;
	do {
		z.next_out = out;
		z.avail_out = sizeof(out);
		rc = inflate(&z, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
			inflateEnd(&z);
			return 0;          /* refuted: it will not decode */
		}
		{
			long got = (long)(sizeof(out) - z.avail_out);
			if (got > 0) {
				crc = crc32(crc, out, (uInt)got);
				total += got;
			}
			if (got == 0 && rc == Z_BUF_ERROR)
				break;
		}
		if (rc == Z_STREAM_END)
			done = 1;
	} while (!done && total < want_len + (1 << 20));
	inflateEnd(&z);

	if (!done)
		return 0;
	if (total != want_len || crc != want_crc)
		return 0;
	return 1;
}

/* Walk a ZIP's central directory and judge the entry the sector sits
 * inside. Returns 1 proven, 0 refuted, -1 no opinion. */
static int zip_check(const uint8_t *buf, long len, long at,
                     char *how, size_t howsz)
{
	long i, cd = -1;
	int verdict = -1;

	for (i = len - 22; i >= 0 && i > len - 70000; i--) {
		if (buf[i] == 'P' && buf[i+1] == 'K' &&
		    buf[i+2] == 5 && buf[i+3] == 6) {
			cd = (long)buf[i+16] | ((long)buf[i+17] << 8) |
			     ((long)buf[i+18] << 16) | ((long)buf[i+19] << 24);
			break;
		}
	}
	if (cd < 0 || cd >= len)
		return -1;

	for (i = cd; i + 46 <= len; ) {
		unsigned long crc;
		long csz, usz, lho, body;
		int nlen, elen, clen, meth;
		char name[64];

		if (!(buf[i] == 'P' && buf[i+1] == 'K' &&
		      buf[i+2] == 1 && buf[i+3] == 2))
			break;
		meth = u16le(buf + i + 10);
		crc  = (unsigned long)buf[i+16] | ((unsigned long)buf[i+17] << 8) |
		       ((unsigned long)buf[i+18] << 16) |
		       ((unsigned long)buf[i+19] << 24);
		csz  = (long)buf[i+20] | ((long)buf[i+21] << 8) |
		       ((long)buf[i+22] << 16) | ((long)buf[i+23] << 24);
		usz  = (long)buf[i+24] | ((long)buf[i+25] << 8) |
		       ((long)buf[i+26] << 16) | ((long)buf[i+27] << 24);
		nlen = u16le(buf + i + 28);
		elen = u16le(buf + i + 30);
		clen = u16le(buf + i + 32);
		lho  = (long)buf[i+42] | ((long)buf[i+43] << 8) |
		       ((long)buf[i+44] << 16) | ((long)buf[i+45] << 24);
		{
			int k, m = nlen < 63 ? nlen : 63;
			for (k = 0; k < m; k++)
				name[k] = (char)buf[i + 46 + k];
			name[m] = 0;
		}
		i += 46 + nlen + elen + clen;

		if (lho < 0 || lho + 30 > len)
			continue;
		body = lho + 30 + u16le(buf + lho + 26) + u16le(buf + lho + 28);
		if (body + csz > len)
			continue;
		if (at < body || at >= body + csz)
			continue;          /* not the entry we are judging */

		if (meth == 0) {
			unsigned long c = crc32(0L, Z_NULL, 0);
			c = crc32(c, buf + body, (uInt)csz);
			verdict = (csz == usz && c == crc);
		} else if (meth == 8) {
			verdict = inflate_check(buf + body, csz, crc, usz);
		} else {
			continue;
		}
		snprintf(how, howsz,
		         "zip entry '%s' (%ld bytes packed): %s", name, csz,
		         verdict == 1
		           ? "inflates, and its CRC-32 matches - proven"
		           : "does not inflate to its recorded CRC-32");
		return verdict;
	}
	return -1;
}

/* A gzip member ends with a CRC-32 and a length. */
static int gzip_check(const uint8_t *buf, long len, char *how, size_t howsz)
{
	unsigned long want;
	long wlen;

	if (len < 18 || buf[0] != 0x1F || buf[1] != 0x8B || buf[2] != 8)
		return -1;
	want = (unsigned long)buf[len-8] | ((unsigned long)buf[len-7] << 8) |
	       ((unsigned long)buf[len-6] << 16) |
	       ((unsigned long)buf[len-5] << 24);
	wlen = (long)buf[len-4] | ((long)buf[len-3] << 8) |
	       ((long)buf[len-2] << 16) | ((long)buf[len-1] << 24);
	{
		long off = 10;
		int flg = buf[3];
		if (flg & 4) { off += 2 + u16le(buf + off); }
		if (flg & 8) { while (off < len && buf[off]) off++; off++; }
		if (flg & 16) { while (off < len && buf[off]) off++; off++; }
		if (flg & 2) off += 2;
		if (off >= len - 8)
			return -1;
		{
			int v = inflate_check(buf + off, len - 8 - off, want, wlen);
			snprintf(how, howsz, "gzip member: %s",
			         v == 1 ? "inflates, and its CRC-32 matches - proven"
			                : "does not inflate to its recorded CRC-32");
			return v;
		}
	}
}
#endif /* DR_NO_ZLIB */

/* Does this reading, used as a FAT, produce a filesystem that holds
 * together? Every chain has to close, stay inside the volume, and add
 * up to the size the directory recorded. */
static double fat_health(dr_fs *fs, const uint8_t *payload,
                         const dr_fs_loc *loc, char *how, size_t howsz)
{
	const dr_fs_info *in = &fs->info;
	long off = (long)(in->reserved +
	                  (loc->fat_copy - 1) * in->fat_sectors + loc->fat_rel) * SECSZ;
	uint8_t *save;
	int f, good = 0, *cl;
	double s;

	save = malloc(SECSZ);
	if (!save)
		return -1.0;
	memcpy(save, fs->img + off, SECSZ);
	if (payload)
		memcpy(fs->img + off, payload, SECSZ);

	cl = malloc(sizeof(int) * (size_t)(in->clusters + 2));
	for (f = 0; cl && f < fs->nfiles; f++) {
		long want = (fs->files[f].size + (long)in->spc * SECSZ - 1) /
		            ((long)in->spc * SECSZ);
		int n = chain(fs, fs->files[f].start, cl, in->clusters + 2);
		if (n == (int)want)
			good++;
	}
	free(cl);
	memcpy(fs->img + off, save, SECSZ);
	free(save);

	if (!fs->nfiles)
		return -1.0;
	s = (double)good / fs->nfiles;
	snprintf(how, howsz,
	         "as a FAT: %d of %d files' chains come out the length the "
	         "directory says", good, fs->nfiles);
	return s;
}

int dr_fs_score(dr_fs *fs, const dr_fs_loc *loc, const uint8_t *payload,
                int len, dr_fs_verdict *out)
{
	if (!fs || !loc || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->score = 0.5;

	if (loc->area == DR_AREA_FREE || loc->area == DR_AREA_OUTSIDE) {
		out->checked = 1;
		out->score = 1.0;
		snprintf(out->how, sizeof(out->how),
		         "no file's data is here - any reading is as good as "
		         "another, and nothing was lost");
		return 0;
	}

	if (loc->area == DR_AREA_FAT) {
		double s = fat_health(fs, payload, loc, out->how,
		                      sizeof(out->how));
		if (s >= 0.0) {
			out->checked = 1;
			out->score = s;
			if (s >= 0.999)
				out->proven = 0;   /* consistent, not proven */
		}
		return 0;
	}

	if (loc->area == DR_AREA_FILE) {
#ifndef DR_NO_ZLIB
		long flen = 0;
		uint8_t *buf = assemble(fs, loc, payload, len, &flen);
		int v = -1;

		if (buf) {
			v = zip_check(buf, flen, loc->file_offset,
			              out->how, sizeof(out->how));
			if (v < 0)
				v = gzip_check(buf, flen, out->how,
				               sizeof(out->how));
			free(buf);
		}
		if (v >= 0) {
			out->checked = 1;
			out->proven = (v == 1);
			out->refuted = (v == 0);
			out->score = v ? 1.0 : 0.0;
			return 0;
		}
#endif
		snprintf(out->how, sizeof(out->how),
		         "inside '%s'; that format carries no checksum we can "
		         "test, so the sector CRC is the only referee",
		         loc->file);
		return 0;
	}

	if (loc->area == DR_AREA_ROOT) {
		/* Directory entries have a shape: a printable 8.3 name, a
		 * start cluster inside the volume, attributes from a short
		 * list. Count how many of the sixteen hold it. */
		int i, ok = 0, used = 0;
		for (i = 0; i < 16; i++) {
			const uint8_t *e = payload + i * 32;
			int j, printable = 1;
			if (e[0] == 0x00 || e[0] == 0xE5)
				continue;
			used++;
			for (j = 0; j < 11; j++)
				if (e[j] < 0x20 || e[j] == 0x7F)
					printable = 0;
			if (printable && !(e[11] & 0xC0))
				ok++;
		}
		out->checked = 1;
		out->score = used ? (double)ok / used : 1.0;
		snprintf(out->how, sizeof(out->how),
		         "as a directory: %d of %d entries have a usable name "
		         "and attributes", ok, used);
		return 0;
	}

	snprintf(out->how, sizeof(out->how), "no check applies here");
	return 0;
}

#ifndef DR_NO_ZLIB
/* ---------------------------------------------------------------- */
/* The same bytes, archived twice on the same disk                   */
/* ---------------------------------------------------------------- */

typedef struct {
	char          name[64];
	long          body, csz, usz;
	unsigned long crc;
	int           method;
} zip_entry;

/* Read a ZIP's central directory into `ents`; returns how many. */
static int zip_entries(const uint8_t *buf, long len, zip_entry *ents, int max)
{
	long i, cd = -1;
	int n = 0;

	for (i = len - 22; i >= 0 && i > len - 70000; i--)
		if (buf[i] == 'P' && buf[i+1] == 'K' &&
		    buf[i+2] == 5 && buf[i+3] == 6) {
			cd = (long)buf[i+16] | ((long)buf[i+17] << 8) |
			     ((long)buf[i+18] << 16) | ((long)buf[i+19] << 24);
			break;
		}
	if (cd < 0 || cd >= len)
		return 0;

	for (i = cd; i + 46 <= len && n < max; ) {
		zip_entry *e = &ents[n];
		int nlen, elen, clen, k, m;
		long lho;

		if (!(buf[i] == 'P' && buf[i+1] == 'K' &&
		      buf[i+2] == 1 && buf[i+3] == 2))
			break;
		e->method = u16le(buf + i + 10);
		e->crc = (unsigned long)buf[i+16] | ((unsigned long)buf[i+17] << 8) |
		         ((unsigned long)buf[i+18] << 16) |
		         ((unsigned long)buf[i+19] << 24);
		e->csz = (long)buf[i+20] | ((long)buf[i+21] << 8) |
		         ((long)buf[i+22] << 16) | ((long)buf[i+23] << 24);
		e->usz = (long)buf[i+24] | ((long)buf[i+25] << 8) |
		         ((long)buf[i+26] << 16) | ((long)buf[i+27] << 24);
		nlen = u16le(buf + i + 28);
		elen = u16le(buf + i + 30);
		clen = u16le(buf + i + 32);
		lho  = (long)buf[i+42] | ((long)buf[i+43] << 8) |
		       ((long)buf[i+44] << 16) | ((long)buf[i+45] << 24);
		m = nlen < 63 ? nlen : 63;
		for (k = 0; k < m; k++)
			e->name[k] = (char)buf[i + 46 + k];
		e->name[m] = 0;
		i += 46 + nlen + elen + clen;

		if (lho < 0 || lho + 30 > len)
			continue;
		e->body = lho + 30 + u16le(buf + lho + 26) +
		          u16le(buf + lho + 28);
		if (e->body + e->csz > len)
			continue;
		n++;
	}
	return n;
}

/* Whole file, straight out of the image - no payload spliced in. */
static uint8_t *assemble_file(dr_fs *fs, int f, long *out_len)
{
	dr_fs_loc l;

	memset(&l, 0, sizeof(l));
	snprintf(l.file, sizeof(l.file), "%s", fs->files[f].name);
	l.file_offset = -1;
	return assemble(fs, &l, NULL, 0, out_len);
}

#define MAXENT 512

int dr_fs_sister(dr_fs *fs, const dr_fs_loc *loc, uint8_t *out, int len,
                 char *how, int howsz)
{
	uint8_t *mine = NULL, *theirs = NULL;
	zip_entry *a = NULL, *b = NULL;
	long mlen = 0, tlen = 0;
	int na, nb, i, j, self = -1, rc = -1;

	if (!fs || !loc || loc->area != DR_AREA_FILE || !out)
		return -1;
	for (i = 0; i < fs->nfiles; i++)
		if (strcmp(fs->files[i].name, loc->file) == 0) { self = i; break; }
	if (self < 0)
		return -1;

	mine = assemble_file(fs, self, &mlen);
	if (!mine)
		return -1;
	a = calloc(MAXENT, sizeof(*a));
	b = calloc(MAXENT, sizeof(*b));
	if (!a || !b)
		goto out;
	na = zip_entries(mine, mlen, a, MAXENT);
	if (!na)
		goto out;

	/* Which entry does the damaged sector sit inside? */
	for (i = 0; i < na; i++)
		if (loc->file_offset >= a[i].body &&
		    loc->file_offset < a[i].body + a[i].csz)
			break;
	if (i == na)
		goto out;

	for (j = 0; j < fs->nfiles && rc < 0; j++) {
		int k;

		free(theirs);
		theirs = assemble_file(fs, j, &tlen);
		if (!theirs)
			continue;
		nb = zip_entries(theirs, tlen, b, MAXENT);
		for (k = 0; k < nb; k++) {
			uint8_t *save;
			int good;

			if (strcmp(b[k].name, a[i].name) ||
			    b[k].csz != a[i].csz || b[k].usz != a[i].usz ||
			    b[k].crc != a[i].crc || b[k].method != a[i].method)
				continue;
			if (j == self && b[k].body == a[i].body)
				continue;      /* the very same bytes */

			/* Try it, and let the archive's CRC-32 decide. */
			save = malloc((size_t)a[i].csz);
			if (!save)
				continue;
			memcpy(save, mine + a[i].body, (size_t)a[i].csz);
			memcpy(mine + a[i].body, theirs + b[k].body,
			       (size_t)a[i].csz);
			good = (a[i].method == 8)
			        ? inflate_check(mine + a[i].body, a[i].csz,
			                        a[i].crc, a[i].usz)
			        : 1;
			if (good == 1) {
				memcpy(out, mine + loc->file_offset,
				       (size_t)len);
				snprintf(how, (size_t)howsz,
				         "'%s' is archived twice on this disk; "
				         "the copy in %s inflates and matches "
				         "its CRC-32 (%08lX) - proven",
				         a[i].name, fs->files[j].name, a[i].crc);
				rc = 1;
			} else {
				memcpy(out, mine + loc->file_offset,
				       (size_t)len);
				snprintf(how, (size_t)howsz,
				         "'%s' is archived twice on this disk, "
				         "but the second copy does not check out "
				         "either", a[i].name);
				rc = 0;
			}
			memcpy(mine + a[i].body, save, (size_t)a[i].csz);
			free(save);
			break;
		}
	}
out:
	free(theirs);
	free(mine);
	free(a);
	free(b);
	return rc;
}
#else
int dr_fs_sister(dr_fs *fs, const dr_fs_loc *loc, uint8_t *out, int len,
                 char *how, int howsz)
{
	(void)fs; (void)loc; (void)out; (void)len; (void)howsz;
	if (how) how[0] = 0;
	return -1;
}
#endif
