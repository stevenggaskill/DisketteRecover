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
#include <ctype.h>

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

	/* Macintosh HFS, when that is what this is. */
	int   hfs;
	long  hfs_albst;         /* first allocation block, in sectors   */
	long  hfs_alblksiz;      /* allocation block size, in bytes      */
	long  hfs_nalblks;
	long  hfs_vbmst;

	/* Root directory, flattened. */
	struct dr_fs_file {
		char  name[72];
		int   start;
		long  size;
		int   attr;
		int   deleted;
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
		if (e[11] == 0x0F)
			continue;          /* a long-name slot             */
		if (e[11] & 0x08)
			continue;          /* volume label                 */
		/*
		 * Deleted entries are kept. Erasing a file on a FAT disk
		 * overwrites one byte of its name and frees its clusters;
		 * everything else - the length, where it started, and very
		 * often the data itself - is still sitting there. On a disk
		 * whose live filesystem has been damaged, those entries are
		 * the same kind of evidence a stale archive directory is.
		 */
		/* A stale entry can record a length the volume never had
		 * room for; that is not a file, it is a slot that was
		 * reused and half-rewritten. */
		if ((long)e[28] + ((long)e[29] << 8) + ((long)e[30] << 16) +
		    ((long)e[31] << 24) > (long)fs->info.total_sectors * SECSZ)
			continue;
		fs->files[fs->nfiles].deleted = (e[0] == 0xE5);
		trim_name(e, fs->files[fs->nfiles].name);
		if (e[0] == 0xE5)
			fs->files[fs->nfiles].name[0] = '?';
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


/* ---------------------------------------------------------------- */
/* Macintosh HFS                                                     */
/* ---------------------------------------------------------------- */
/*
 * Not every floppy in a box of floppies is a PC floppy. An HFS volume
 * has no FAT and no 8.3 directory, so the FAT reader says "no
 * filesystem" and the whole account above the sector goes quiet - on a
 * disk that may be perfectly readable.
 *
 * The catalogue is a B-tree and naming the file a sector belongs to
 * would mean walking it. The volume bitmap is one flat run of bits and
 * answers the question that matters most: is anything allocated here at
 * all? On these disks that single bit has settled more bad sectors than
 * any search.
 */
static int be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static long be32(const uint8_t *p)
{
	return ((long)p[0] << 24) | ((long)p[1] << 16) |
	       ((long)p[2] << 8) | p[3];
}

static int hfs_open(dr_ctx *c, dr_fs *fs)
{
	uint8_t mdb[SECSZ];
	dr_fs_info *in = &fs->info;
	long need;
	int i;

	/* The master directory block is the third sector of the volume. */
	if (read_one(c, 0, 0, 3, mdb) != 0)
		return -1;
	if (be16(mdb) != 0x4244)
		return -1;

	fs->hfs = 1;
	fs->hfs_vbmst    = be16(mdb + 0x0E);
	fs->hfs_nalblks  = be16(mdb + 0x12);
	fs->hfs_alblksiz = be32(mdb + 0x14);
	fs->hfs_albst    = be16(mdb + 0x1C);
	if (fs->hfs_alblksiz < SECSZ || (fs->hfs_alblksiz % SECSZ) ||
	    fs->hfs_nalblks < 1)
		return -1;

	snprintf(in->kind, sizeof(in->kind), "HFS");
	{
		int n = mdb[0x24];
		if (n > 27)
			n = 27;
		for (i = 0; i < n && i < (int)sizeof(in->oem) - 1; i++)
			in->oem[i] = (char)mdb[0x25 + i];
		in->oem[i] = 0;
	}
	in->bps   = SECSZ;
	in->spc   = (int)(fs->hfs_alblksiz / SECSZ);
	in->nfats = 0;
	in->nfiles = (int)be32(mdb + 0x54) + (int)be32(mdb + 0x58);
	in->total_sectors = fs->hfs_albst +
	                    fs->hfs_nalblks * (fs->hfs_alblksiz / SECSZ);
	in->data_lba = fs->hfs_albst;
	in->root_lba = fs->hfs_vbmst;
	in->clusters = (int)fs->hfs_nalblks;
	/* HFS records no geometry: it is addressed by logical block, and
	 * the drive is expected to know the rest. Take it from what the
	 * scan actually found rather than assuming a 1.44 MB disk. */
	{
		int n = 0, k;
		const dr_sector *sl = dr_sectors(c, &n);

		in->spt = 0;
		in->heads = 1;
		for (k = 0; k < n; k++) {
			if (sl[k].sector_size != SECSZ)
				continue;
			if (sl[k].sector_id > in->spt)
				in->spt = sl[k].sector_id;
			if (sl[k].side + 1 > in->heads)
				in->heads = sl[k].side + 1;
		}
		if (in->spt < 1 || in->spt > 64)
			return -1;
	}

	need = in->total_sectors;
	if (need > MAX_LBA)
		need = MAX_LBA;
	fs->nlba  = need;
	fs->img   = calloc((size_t)need, SECSZ);
	fs->have  = calloc((size_t)need, 1);
	fs->owner = malloc(sizeof(int) * (size_t)need);
	if (!fs->img || !fs->have || !fs->owner)
		return -1;
	for (i = 0; i < need; i++)
		fs->owner[i] = -1;
	collect(c, fs, in->spt, in->heads);
	in->present = 1;
	return 0;
}

/* Is this allocation block spoken for? */
static int hfs_allocated(const dr_fs *fs, long lba)
{
	long blk = (lba - fs->hfs_albst) / (fs->hfs_alblksiz / SECSZ);
	long bit = fs->hfs_vbmst * SECSZ * 8 + blk;
	long off = fs->hfs_vbmst * SECSZ + blk / 8;

	if (blk < 0 || blk >= fs->hfs_nalblks)
		return -1;
	(void)bit;
	if (off < 0 || off >= fs->nlba * SECSZ)
		return -1;
	return (fs->img[off] >> (7 - (blk & 7))) & 1;
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
		memset(in, 0, sizeof(*in));
		if (hfs_open(c, fs) == 0)
			return fs;
		dr_fs_free(fs);
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

int dr_fs_locate(dr_fs *fs, dr_ctx *c, int sector_index, dr_fs_loc *out)
{
	const dr_fs_info *in;
	const dr_sector *sl;
	long lba = -1;
	int n = 0;

	if (!fs || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	out->mirror_sector = -1;
	in = &fs->info;

	/* Derived fresh from the sector's address rather than read out of
	 * the map built at open time: applying a repair re-scans, and a
	 * cached index would then point at the wrong sector. */
	sl = c ? dr_sectors(c, &n) : NULL;
	if (sl && sector_index >= 0 && sector_index < n &&
	    sl[sector_index].sector_size == SECSZ &&
	    sl[sector_index].sector_id >= 1 &&
	    sl[sector_index].sector_id <= in->spt)
		lba = ((long)sl[sector_index].track * in->heads +
		       sl[sector_index].side) * in->spt +
		      (sl[sector_index].sector_id - 1);
	if (lba < 0 || lba >= fs->nlba)
		return -1;
	out->lba = lba;

	if (fs->hfs) {
		int used;

		if (lba < 3) {
			out->area = DR_AREA_BOOT;
			snprintf(out->note, sizeof(out->note),
			         "HFS boot blocks / master directory block");
			return 0;
		}
		if (lba < fs->hfs_albst) {
			out->area = DR_AREA_ROOT;
			snprintf(out->note, sizeof(out->note),
			         "HFS volume bitmap");
			return 0;
		}
		used = hfs_allocated(fs, lba);
		if (used == 0) {
			out->area = DR_AREA_FREE;
			snprintf(out->note, sizeof(out->note),
			         "allocation block %ld - the volume bitmap says "
			         "nothing is stored here, so nothing was lost",
			         (lba - fs->hfs_albst) /
			         (fs->hfs_alblksiz / SECSZ));
		} else {
			out->area = DR_AREA_FILE;
			out->cluster = (int)((lba - fs->hfs_albst) /
			                     (fs->hfs_alblksiz / SECSZ));
			snprintf(out->note, sizeof(out->note),
			         "allocation block %d - allocated to some "
			         "file; HFS catalogue lookup is not "
			         "implemented, so which one is not known",
			         out->cluster);
		}
		return 0;
	}

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
			/* Live files first, then deleted ones: a sector
			 * inside a deleted file is not free space, it is
			 * the last copy of something somebody threw away
			 * and may well want back. */
			int pass;

			for (pass = 0; pass < 2 && out->area != DR_AREA_FILE;
			     pass++)
			for (f = 0; f < fs->nfiles; f++) {
				int n, k;

				if (fs->files[f].deleted != pass)
					continue;
				n = chain(fs, fs->files[f].start, cl,
				          in->clusters + 2);
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
					         "%s%s (%ld bytes)",
					         out->cluster, out->file_offset,
					         out->file_offset + SECSZ,
					         fs->files[f].deleted
					           ? "the deleted file " : "",
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
/* OLE compound documents - PowerPoint, Word, Excel                  */
/* ---------------------------------------------------------------- */
/*
 * A .ppt or .doc is a little filesystem in its own right: a sector
 * table, a directory, and named streams laid out in 512-byte sectors.
 * Two things in it are worth having.
 *
 * The first is that a PowerPoint 97 file saved for backwards
 * compatibility contains the *same presentation twice*, and with it the
 * same preview thumbnail twice - two streams with the same name and the
 * same length, byte for byte identical. When a bad sector lands in one
 * of them the other is sitting a few kilobytes away. Zeus's 9/0 s9 is
 * exactly that.
 *
 * The second is that what these streams contain is a chain of records,
 * each declaring its own length, which must tile the stream exactly:
 * 727 metafile records landing precisely on the last byte. One wrong
 * byte in a length field and the chain walks off the end. That is a
 * referee with hundreds of constraints where the sector CRC has
 * sixteen - and on Zeus the sector CRC turns out to be wrong anyway.
 */

typedef struct {
	char  name[64];
	char  path[256];         /* including the storages above it      */
	int   type;              /* 1 = storage, 2 = stream, 5 = root    */
	int   start;
	long  size;
	int   left, right, child;
	int   live;              /* a real entry, not a free slot        */
} cfb_dirent;

typedef struct {
	const uint8_t *d;
	long      len;
	int       ssz;
	long      cutoff;
	int32_t  *fat;
	long      nfatent;
	cfb_dirent *ents;
	int       nents;
} cfb;

static long i32(const uint8_t *p)
{
	return (long)(int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static long cfb_off(const cfb *c, long sec) { return 512 + sec * c->ssz; }

static int cfb_chain(const cfb *c, long start, long *out, int max)
{
	int n = 0;
	long s = start;

	while (n < max && s >= 0 && s < c->nfatent) {
		int k, seen = 0;
		for (k = 0; k < n; k++)
			if (out[k] == s) { seen = 1; break; }
		if (seen)
			break;
		out[n++] = s;
		s = c->fat[s];
	}
	return n;
}

static void cfb_free(cfb *c)
{
	if (!c)
		return;
	free(c->fat);
	free(c->ents);
	free(c);
}

/* Give every entry the path of the storages it sits under. */
static void cfb_paths(cfb *c, int id, const char *prefix)
{
	cfb_dirent *e;

	if (id < 0 || id >= c->nents)
		return;
	e = &c->ents[id];
	if (!e->live || e->path[0])
		return;
	snprintf(e->path, sizeof(e->path), "%s%s",
	         prefix, e->type == 5 ? "" : e->name);
	cfb_paths(c, e->left, prefix);
	cfb_paths(c, e->right, prefix);
	if (e->child >= 0) {
		char sub[288];
		snprintf(sub, sizeof(sub), "%s%s", e->path,
		         e->type == 5 ? "" : "/");
		cfb_paths(c, e->child, sub);
	}
}

static cfb *cfb_open(const uint8_t *d, long len)
{
	static const uint8_t sig[8] =
	        { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
	cfb *c;
	long nfat, dirs, i, difat0, ndifat;
	long *difat, ndf = 0, cap;

	if (!d || len < 4096 || memcmp(d, sig, 8))
		return NULL;
	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->d = d;
	c->len = len;
	c->ssz = 1 << u16le(d + 0x1E);
	c->cutoff = i32(d + 0x38);
	if (c->ssz != 512 && c->ssz != 4096) {
		free(c);
		return NULL;
	}
	nfat   = i32(d + 0x2C);
	dirs   = i32(d + 0x30);
	difat0 = i32(d + 0x44);
	ndifat = i32(d + 0x48);
	if (nfat < 1 || nfat > 4096) {
		free(c);
		return NULL;
	}

	cap = nfat + 128;
	difat = malloc(sizeof(long) * (size_t)cap);
	if (!difat) {
		free(c);
		return NULL;
	}
	for (i = 0; i < 109 && ndf < cap; i++)
		difat[ndf++] = i32(d + 0x4C + i * 4);
	{
		long s = difat0, guard = 0;
		while (s >= 0 && ndifat && guard++ < 1024 &&
		       cfb_off(c, s) + c->ssz <= len) {
			const uint8_t *p = d + cfb_off(c, s);
			for (i = 0; i < c->ssz / 4 - 1 && ndf < cap; i++)
				difat[ndf++] = i32(p + i * 4);
			s = i32(p + c->ssz - 4);
		}
	}

	c->nfatent = nfat * (c->ssz / 4);
	c->fat = malloc(sizeof(int32_t) * (size_t)c->nfatent);
	if (!c->fat) {
		free(difat);
		free(c);
		return NULL;
	}
	for (i = 0; i < c->nfatent; i++)
		c->fat[i] = -1;
	for (i = 0; i < nfat && i < ndf; i++) {
		long s = difat[i], k;
		if (s < 0 || cfb_off(c, s) + c->ssz > len)
			continue;
		for (k = 0; k < c->ssz / 4; k++)
			c->fat[i * (c->ssz / 4) + k] =
			        (int32_t)i32(d + cfb_off(c, s) + k * 4);
	}
	free(difat);

	/* Directory. Entries are kept at their own directory ids - the
	 * tree's left/right/child links are ids, and the path a stream
	 * sits at is the useful part: a PowerPoint file holds the same
	 * stream names twice, once live and once inside the storage that
	 * keeps the PowerPoint 95 copy. */
	{
		long *ch = malloc(sizeof(long) * (size_t)c->nfatent);
		int n, k;

		if (!ch) {
			cfb_free(c);
			return NULL;
		}
		n = cfb_chain(c, dirs, ch, (int)c->nfatent);
		c->nents = n * (c->ssz / 128);
		c->ents = calloc((size_t)c->nents + 1, sizeof(*c->ents));
		if (!c->ents) {
			free(ch);
			cfb_free(c);
			return NULL;
		}
		for (k = 0; k < n; k++) {
			long base = cfb_off(c, ch[k]);
			int j;

			if (base + c->ssz > len)
				break;
			for (j = 0; j < c->ssz / 128; j++) {
				const uint8_t *e = d + base + j * 128;
				int nl = u16le(e + 64), m, q;
				cfb_dirent *o = &c->ents[k * (c->ssz / 128) + j];

				o->type = e[66];
				o->left  = (int)i32(e + 68);
				o->right = (int)i32(e + 72);
				o->child = (int)i32(e + 76);
				o->start = (int)i32(e + 116);
				o->size  = i32(e + 120);
				if (nl < 2 || nl > 64 ||
				    (o->type != 1 && o->type != 2 && o->type != 5))
					continue;
				m = nl / 2 - 1;
				if (m > 63)
					m = 63;
				for (q = 0; q < m; q++)
					o->name[q] = (char)e[q * 2];
				o->name[m] = 0;
				o->live = 1;
			}
		}
		free(ch);
		cfb_paths(c, 0, "");
	}
	return c;
}

/* Pull a stream out whole. Only streams held in full sectors - a
 * damaged sector cannot be inside the mini-stream anyway, since the
 * mini-stream is packed 64 bytes at a time. */
static uint8_t *cfb_stream(const cfb *c, const cfb_dirent *e, long *out_len)
{
	long *ch, i;
	uint8_t *b;
	int n;

	if (e->size < c->cutoff)
		return NULL;
	ch = malloc(sizeof(long) * (size_t)c->nfatent);
	if (!ch)
		return NULL;
	n = cfb_chain(c, e->start, ch, (int)c->nfatent);
	b = calloc((size_t)n * c->ssz + 1, 1);
	if (!b) {
		free(ch);
		return NULL;
	}
	for (i = 0; i < n; i++) {
		long o = cfb_off(c, ch[i]);
		if (o + c->ssz <= c->len)
			memcpy(b + i * c->ssz, c->d + o, (size_t)c->ssz);
	}
	*out_len = e->size < (long)n * c->ssz ? e->size : (long)n * c->ssz;
	free(ch);
	return b;
}

/* Which stream holds this byte of the file, and where in it? */
static int cfb_owner(const cfb *c, long fileoff, long *stream_off)
{
	long sec = (fileoff - 512) / c->ssz, *ch;
	int i, best = -1;

	if (fileoff < 512)
		return -1;
	ch = malloc(sizeof(long) * (size_t)c->nfatent);
	if (!ch)
		return -1;
	for (i = 0; i < c->nents && best < 0; i++) {
		int n, k;

		if (!c->ents[i].live || c->ents[i].type != 2 ||
		    c->ents[i].size < c->cutoff)
			continue;
		n = cfb_chain(c, c->ents[i].start, ch, (int)c->nfatent);
		for (k = 0; k < n; k++)
			if (ch[k] == sec) {
				best = i;
				*stream_off = (long)k * c->ssz +
				              (fileoff - 512 - sec * c->ssz);
				break;
			}
	}
	free(ch);
	return best;
}

/* ---- the record chains -------------------------------------------- */

/* A PowerPoint / OfficeArt record: 2 bytes of version+instance, 2 of
 * type, 4 of length. A container's children live inside its length, so
 * the walk descends into it rather than stepping over it. Every record
 * must fit, and the last one must end exactly on the stream's end. */
static int ppt_records_tile(const uint8_t *b, long len, int *nrec)
{
	long o = 0;
	int n = 0;

	while (o + 8 <= len) {
		long ln = (long)((uint32_t)b[o+4] | ((uint32_t)b[o+5] << 8) |
		                 ((uint32_t)b[o+6] << 16) |
		                 ((uint32_t)b[o+7] << 24));
		int container = (b[o] & 0x0F) == 0x0F;

		if (ln < 0 || o + 8 + ln > len)
			break;
		n++;
		o += container ? 8 : 8 + ln;
	}
	if (nrec)
		*nrec = n;
	return o == len && n > 0;
}

/* A Windows metafile: DWORD size in words, WORD function, then
 * parameters; the chain ends on function 0 and must land exactly on the
 * length the header declared. */
static int wmf_records_tile(const uint8_t *b, long len, long base, int *nrec)
{
	long sz, end, o;
	int n = 0;

	if (base + 18 > len)
		return 0;
	sz = (long)((uint32_t)b[base+6] | ((uint32_t)b[base+7] << 8) |
	            ((uint32_t)b[base+8] << 16) | ((uint32_t)b[base+9] << 24));
	end = base + sz * 2;
	if (sz < 9 || end > len)
		return 0;
	o = base + 18;
	while (o + 6 <= end) {
		long rsz = (long)((uint32_t)b[o] | ((uint32_t)b[o+1] << 8) |
		                  ((uint32_t)b[o+2] << 16) |
		                  ((uint32_t)b[o+3] << 24));
		int fn = u16le(b + o + 4);

		if (rsz < 3 || o + rsz * 2 > end)
			return 0;
		n++;
		if (nrec)
			*nrec = n;
		if (!fn)
			return o + rsz * 2 == end;
		o += rsz * 2;
	}
	return 0;
}

/* Find the metafile a property set carries as its thumbnail. */
static long wmf_base(const uint8_t *b, long len)
{
	long i;

	if (len < 64 || u16le(b) != 0xFFFE)
		return -1;
	for (i = 0; i + 18 < len; i++)
		if (u16le(b + i) == 1 && u16le(b + i + 2) == 9 &&
		    (u16le(b + i + 4) == 0x0300 || u16le(b + i + 4) == 0x0100))
			return i;
	return -1;
}

/* Does this stream hold together? Returns 1 yes, 0 no, -1 no opinion. */
static int cfb_stream_ok(const cfb_dirent *e, const uint8_t *b, long len,
                         char *how, size_t howsz)
{
	int n = 0;

	if (strstr(e->name, "SummaryInformation")) {
		long base = wmf_base(b, len);
		int ok;

		if (base < 0)
			return -1;
		ok = wmf_records_tile(b, len, base, &n);
		snprintf(how, howsz,
		         "its preview metafile: %s after %d record(s)",
		         ok ? "every record tiles exactly"
		            : "the record chain breaks", n);
		return ok;
	}
	if (!strcmp(e->name, "PowerPoint Document")) {
		int ok = ppt_records_tile(b, len, &n);

		/* A handful of records is not evidence that the stream is
		 * broken - it is evidence that this is not the record
		 * layout we think it is. Only a walk that got properly
		 * under way and then failed says anything. */
		if (n < 8 && !ok)
			return -1;
		snprintf(how, howsz,
		         "its record chain %s after %d record(s)",
		         ok ? "tiles exactly to the end" : "breaks", n);
		return ok;
	}
	return -1;
}


/* The same stream, stored twice inside one compound document. Match by
 * name and length, then demand that the two agree everywhere *except*
 * inside the damaged sector: that agreement, over thousands of bytes,
 * is what makes the twin's version of the missing 512 evidence rather
 * than a guess. Returns 1 with `out` filled, 0 if a twin exists but
 * does not check out, -1 if there is none. */
static int cfb_twin(const uint8_t *file, long flen, long fileoff, int len,
                    uint8_t *out, char *how, size_t howsz)
{
	cfb *c = cfb_open(file, flen);
	uint8_t *mine = NULL, *theirs = NULL;
	long mlen = 0, tlen = 0, soff = -1;
	int owner, i, rc = -1;

	if (!c)
		return -1;
	owner = cfb_owner(c, fileoff, &soff);
	if (owner < 0)
		goto out;
	mine = cfb_stream(c, &c->ents[owner], &mlen);
	if (!mine || soff < 0 || soff + len > mlen)
		goto out;

	for (i = 0; i < c->nents; i++) {
		long agree = 0, j;
		char msg[220];
		int ok;

		if (i == owner || !c->ents[i].live || c->ents[i].type != 2 ||
		    c->ents[i].size != c->ents[owner].size ||
		    strcmp(c->ents[i].name, c->ents[owner].name))
			continue;
		free(theirs);
		theirs = cfb_stream(c, &c->ents[i], &tlen);
		if (!theirs || tlen != mlen)
			continue;

		/*
		 * Not "identical everywhere" - the two copies are written
		 * by different halves of the application and a few header
		 * fields genuinely differ. What matters is the run of
		 * agreement either side of the damage: if the twin matches
		 * for thousands of bytes right up to the sector and
		 * thousands of bytes straight after it, the 512 in between
		 * are not a guess. A handful of differences in a header
		 * five kilobytes away says nothing about them.
		 */
		{
			long left = 0, right = 0, other = 0;

			for (j = soff - 1; j >= 0 && mine[j] == theirs[j]; j--)
				left++;
			for (j = soff + len; j < mlen && mine[j] == theirs[j]; j++)
				right++;
			for (j = 0; j < mlen; j++)
				if ((j < soff || j >= soff + len) &&
				    mine[j] != theirs[j])
					other++;
			agree = left + right;
			if ((left < 256 && soff > 256) ||
			    (right < 256 && mlen - (soff + len) > 256)) {
				snprintf(how, howsz,
				         "'%s' is stored twice in this file, "
				         "but the two copies part company %ld "
				         "byte(s) before and %ld after the "
				         "damage - not a copy of these bytes",
				         c->ents[owner].name, left, right);
				rc = 0;
				goto out;
			}
			(void)other;
		}

		memcpy(mine + soff, theirs + soff, (size_t)len);
		ok = cfb_stream_ok(&c->ents[owner], mine, mlen, msg, sizeof(msg));
		memcpy(out, mine + soff, (size_t)len);
		if (ok == 1) {
			snprintf(how, howsz,
			         "'%s' is stored twice in this file; the twin "
			         "matches for %ld byte(s) either side of the "
			         "damage, and with its %d bytes in place, %s",
			         c->ents[owner].name, agree, len, msg);
			rc = 1;
		} else {
			snprintf(how, howsz,
			         "'%s' is stored twice in this file; the twin "
			         "matches for %ld byte(s) either side of the "
			         "damage, but %s", c->ents[owner].name, agree,
			         ok == 0 ? msg : "nothing here can check it");
			rc = (ok == 0) ? 0 : 1;
		}
		goto out;
	}
out:
	free(theirs);
	free(mine);
	cfb_free(c);
	return rc;
}

/* Put a payload to whatever structure the compound document has.
 * Returns 1 holds, 0 broken, -1 no opinion. */
static int cfb_check(const uint8_t *file, long flen, long fileoff,
                     char *how, size_t howsz)
{
	cfb *c = cfb_open(file, flen);
	uint8_t *b = NULL;
	long blen = 0, soff = -1;
	int owner, rc = -1;

	if (!c)
		return -1;
	owner = cfb_owner(c, fileoff, &soff);
	if (owner < 0)
		goto out;
	b = cfb_stream(c, &c->ents[owner], &blen);
	if (!b)
		goto out;
	rc = cfb_stream_ok(&c->ents[owner], b, blen, how, howsz);
out:
	free(b);
	cfb_free(c);
	return rc;
}

static int palm_ok(const uint8_t *b, long len, char *name, size_t namesz,
                   char *how, size_t howsz);
static int framing_check(const uint8_t *b, long len, long at, long span,
                         char *how, size_t howsz, double *score);

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
		if (!fs->files[f].deleted &&
		    strcmp(fs->files[f].name, loc->file) == 0)
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


#ifndef DR_NO_ZLIB
/* How much of a member survives the damage.
 *
 * A deflate stream cannot be decoded from the middle - the Huffman
 * tables and the 32 KB window are both state - so one bad sector
 * normally costs the whole member from that point on. What it does not
 * cost is the part *before* it: feeding the decoder only the bytes that
 * precede the damage yields every byte it had produced by then, and on
 * a well-compressed file that can be a great deal. Sand's Contacts.adx
 * gives up 3,699 of its 58,368 bytes this way.
 *
 * With `keep` non-NULL the bytes themselves come back in a buffer the
 * caller owns, so the readable part of a lost file can be written out
 * rather than merely counted.
 *
 * Returns the number of bytes recoverable, or -1. */
static long inflate_prefix_to(const uint8_t *src, long avail,
                              uint8_t **keep)
{
	z_stream z;
	uint8_t out[16384];
	uint8_t *acc = NULL;
	long total = 0;
	int rc;

	if (keep)
		*keep = NULL;
	if (avail < 2)
		return -1;
	memset(&z, 0, sizeof(z));
	if (inflateInit2(&z, -15) != Z_OK)
		return -1;
	z.next_in = (Bytef *)src;
	z.avail_in = (uInt)avail;
	do {
		long got;

		z.next_out = out;
		z.avail_out = sizeof(out);
		rc = inflate(&z, Z_NO_FLUSH);
		got = (long)(sizeof(out) - z.avail_out);
		if (keep && got > 0) {
			uint8_t *na = realloc(acc, (size_t)(total + got));
			if (!na) {
				free(acc);
				acc = NULL;
				keep = NULL;
			} else {
				acc = na;
				memcpy(acc + total, out, (size_t)got);
			}
		}
		total += got;
		if (rc != Z_OK && rc != Z_BUF_ERROR)
			break;
	} while (z.avail_in || z.avail_out == 0);
	inflateEnd(&z);
	if (keep)
		*keep = acc;
	else
		free(acc);
	return total;
}

static long inflate_prefix(const uint8_t *src, long avail)
{
	return inflate_prefix_to(src, avail, NULL);
}
#endif

/*
 * A half-decompressed image is not a file. GIF in particular is a
 * chain: header, screen descriptor, palette, then a run of blocks each
 * of which is itself a chain of length-prefixed chunks, ended by a
 * zero byte, and the file ends with 0x3B. Cut that chain anywhere and
 * a viewer has nothing to hold on to - it reads a length byte that
 * runs off the end and gives up, often showing nothing at all.
 *
 * Cutting it at the last chunk boundary and writing the two bytes that
 * close it off turns the same bytes into a real, short GIF: the rows
 * that survived are drawn, and the rest is left blank. SLAT's
 * TransportImg.gif inflates to 2,292 of its 2,796 bytes and shows 48
 * of its 160 rows this way.
 *
 * Returns the length to keep, having appended any terminator into
 * `buf` (which must have room for two more bytes), or -1 to say this
 * is not a format it knows how to close.
 */
static long gif_close(uint8_t *buf, long n, long full,
                      char *how, size_t howsz)
{
	long p, last;

	if (n < 13 || memcmp(buf, "GIF8", 4) != 0)
		return -1;
	p = 13;
	if (buf[10] & 0x80)                       /* global colour table */
		p += 3L << ((buf[10] & 7) + 1);
	if (p >= n)
		return -1;                        /* not even the palette */
	last = p;
	while (p < n) {
		if (buf[p] == 0x3B)               /* already complete */
			return p + 1;
		if (buf[p] == 0x21) {             /* extension */
			p += 2;
		} else if (buf[p] == 0x2C) {      /* image descriptor */
			if (p + 10 > n)
				break;
			if (buf[p + 9] & 0x80)    /* local colour table */
				p += 3L << ((buf[p + 9] & 7) + 1);
			p += 11;                  /* + LZW minimum code size */
		} else {
			break;                    /* not a block we know */
		}
		if (p > n)
			break;
		while (p < n && buf[p]) {         /* the sub-block chain */
			long blk = 1 + buf[p];

			if (p + blk > n)
				break;
			p += blk;
			last = p;
		}
		if (p >= n || buf[p])
			break;                    /* chain ran off the end */
		p++;
		last = p;
	}
	if (last <= 13 || last > n)
		return -1;
	buf[last] = 0x00;                         /* end of sub-blocks */
	buf[last + 1] = 0x3B;                     /* trailer */
	if (how)
		snprintf(how, howsz, "trimmed to the last whole GIF block "
		         "and closed off - %ld of %ld byte(s), and it opens",
		         last + 2, full);
	return last + 2;
}

/* Walk a ZIP's central directory and judge every entry the sector
 * overlaps. A 512-byte sector is bigger than a small archive member and
 * routinely straddles a boundary: on Sand the damaged sector starts two
 * bytes before the end of one entry and the damage itself is a hundred
 * bytes into the next. Judging only the entry the sector starts in
 * passes that sector while the file behind it stays broken.
 * Returns 1 proven, 0 refuted, -1 no opinion. */
static int zip_check(const uint8_t *buf, long len, long at, long span,
                     char *how, size_t howsz)
{
	long i, cd = -1;
	int verdict = -1, checked = 0, failed = 0;
	long badbody = -1, badusz = 0;
	char firstbad[80];

	firstbad[0] = 0;
	if (span < 1)
		span = 1;
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
		int nlen, elen, clen, meth, v;
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
		body = lho + 30 + u16le(buf + lho + 26) +
		       u16le(buf + lho + 28);
		if (body + csz > len)
			continue;
		if (body >= at + span || body + csz <= at)
			continue;          /* no overlap with this sector */

		if (meth == 0) {
			unsigned long c = crc32(0L, Z_NULL, 0);
			c = crc32(c, buf + body, (uInt)csz);
			v = (csz == usz && c == crc);
		} else if (meth == 8) {
			v = inflate_check(buf + body, csz, crc, usz);
		} else {
			continue;
		}
		checked++;
		if (!v) {
			failed++;
			if (!firstbad[0]) {
				snprintf(firstbad, sizeof(firstbad), "%s", name);
				badbody = body;
				badusz = usz;
			}
		}
	}
	if (!checked)
		return -1;
	verdict = failed ? 0 : 1;
	if (verdict)
		snprintf(how, howsz,
		         "all %d zip entr%s this sector touches inflate, and "
		         "their CRC-32s match - proven", checked,
		         checked == 1 ? "y" : "ies");
	else if (badbody >= 0 && at > badbody)
		snprintf(how, howsz,
		         "zip entry '%s' does not inflate to its recorded "
		         "CRC-32 (%d of %d this sector touches fail); %ld of "
		         "its %ld byte(s) are still recoverable from the part "
		         "before the damage",
		         firstbad, failed, checked,
		         inflate_prefix(buf + badbody, at - badbody), badusz);
	else
		snprintf(how, howsz,
		         "zip entry '%s' does not inflate to its recorded "
		         "CRC-32 (%d of %d entries this sector touches fail)",
		         firstbad, failed, checked);
	return verdict;
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
		long want;

		if (fs->files[f].deleted)
			continue;
		want = (fs->files[f].size + (long)in->spc * SECSZ - 1) /
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
		double part = -1.0;

		if (buf) {
			v = zip_check(buf, flen, loc->file_offset, len,
			              out->how, sizeof(out->how));
			if (v < 0)
				v = gzip_check(buf, flen, out->how,
				               sizeof(out->how));
			if (v < 0)
				v = cfb_check(buf, flen, loc->file_offset,
				              out->how, sizeof(out->how));
			if (v < 0)
				v = palm_ok(buf, flen, NULL, 0, out->how,
				            sizeof(out->how));
			if (v < 0)
				v = framing_check(buf, flen, loc->file_offset,
				                  len, out->how,
				                  sizeof(out->how), &part);
			free(buf);
		}
		if (v >= 0) {
			out->checked = 1;
			out->proven = (v == 1);
			out->refuted = (v == 0);
			out->score = v ? 1.0 : (part >= 0.0 ? part : 0.0);
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
	long          hdr;           /* local file header offset          */
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
		e->hdr = lho;
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
	int spliced = 0, touched = 0;
	char last[64];

	last[0] = 0;
	if (!fs || !loc || loc->area != DR_AREA_FILE || !out)
		return -1;
	for (i = 0; i < fs->nfiles; i++)
		if (!fs->files[i].deleted &&
		    strcmp(fs->files[i].name, loc->file) == 0) { self = i; break; }
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
	if (!na) {
		/* Not an archive - but a compound document keeps its own
		 * second copy, and it is the same idea. */
		rc = cfb_twin(mine, mlen, loc->file_offset, len, out,
		              how, (size_t)howsz);
		goto out;
	}

	/*
	 * Every entry the sector overlaps, not just the one it starts in.
	 * A 512-byte sector is bigger than a small archive member, and on
	 * Sand the damaged one begins two bytes before the end of
	 * 'slscntc5.rep' with all of the damage inside the next entry.
	 * Splicing only the first left the file broken while the check
	 * reported it proven.
	 */
	for (i = 0; i < na; i++) {
		if (a[i].body >= loc->file_offset + len ||
		    a[i].body + a[i].csz <= loc->file_offset)
			continue;
		touched++;
		for (j = 0; j < fs->nfiles; j++) {
			int k, got = 0;

			free(theirs);
			theirs = assemble_file(fs, j, &tlen);
			if (!theirs)
				continue;
			nb = zip_entries(theirs, tlen, b, MAXENT);
			for (k = 0; k < nb; k++) {
				if (strcmp(b[k].name, a[i].name) ||
				    b[k].csz != a[i].csz ||
				    b[k].usz != a[i].usz ||
				    b[k].crc != a[i].crc ||
				    b[k].method != a[i].method)
					continue;
				if (j == self && b[k].body == a[i].body)
					continue;    /* the very same bytes */
				/* Take the local file header along with the
				 * body when the two are laid out the same.
				 * A sector that straddles entries covers the
				 * header between them, and nothing else is
				 * going to put that back. */
				if (a[i].body - a[i].hdr == b[k].body - b[k].hdr)
					memcpy(mine + a[i].hdr,
					       theirs + b[k].hdr,
					       (size_t)(a[i].body - a[i].hdr));
				memcpy(mine + a[i].body, theirs + b[k].body,
				       (size_t)a[i].csz);
				spliced++;
				got = 1;
				snprintf(last, sizeof(last), "%s", a[i].name);
				break;
			}
			if (got)
				break;
		}
	}

	if (!spliced)
		goto out;

	/* Let the archive's own checksums referee the result - every entry
	 * the sector touches, spliced or not. */
	{
		char msg[220];
		int v = zip_check(mine, mlen, loc->file_offset, len,
		                  msg, sizeof(msg));

		memcpy(out, mine + loc->file_offset, (size_t)len);
		if (v == 1) {
			snprintf(how, (size_t)howsz,
			         "'%s' is archived twice on this disk; with the "
			         "second copy in place all %d entr%s this "
			         "sector touches inflate and match their "
			         "CRC-32 - proven", last, touched,
			         touched == 1 ? "y" : "ies");
			rc = 1;
		} else {
			snprintf(how, (size_t)howsz,
			         "'%s' is archived twice on this disk, but the "
			         "second copy does not check out either", last);
			rc = 0;
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

int dr_fs_detail(dr_fs *fs, const dr_fs_loc *loc, char *buf, int n)
{
	uint8_t *file;
	long flen = 0;
	int rc = -1;

	if (!fs || !loc || !buf || n < 32 || loc->area != DR_AREA_FILE)
		return -1;
	buf[0] = 0;
	file = assemble(fs, loc, NULL, 0, &flen);
	if (!file)
		return -1;
#ifndef DR_NO_ZLIB
	{
		zip_entry *e = calloc(MAXENT, sizeof(*e));
		cfb *c;
		int ne, i;

		if (e) {
			ne = zip_entries(file, flen, e, MAXENT);
			for (i = 0; i < ne; i++)
				if (loc->file_offset < e[i].body + e[i].csz &&
				    e[i].body < loc->file_offset + 512) {
					snprintf(buf, (size_t)n,
					         "zip entry '%s', %ld byte(s) "
					         "packed", e[i].name, e[i].csz);
					rc = 0;
					break;
				}
			free(e);
		}
		if (rc == 0)
			goto done;

		c = cfb_open(file, flen);
		if (!c) {
			/*
			 * Not a container we can walk into. Then the useful
			 * thing to say is which file this *is*, precisely
			 * enough to go and find another copy: a version
			 * number and a copyright line pin a Windows driver
			 * down to one build, and a copy of the wrong build
			 * is no use at all.
			 */
			char ver[40], cop[72];
			long i;

			ver[0] = cop[0] = 0;
			for (i = 0; i + 8 < flen && !(ver[0] && cop[0]); i++) {
				if (!ver[0] && file[i] >= '0' && file[i] <= '9') {
					int k = 0, dots = 0, digits = 0;
					while (k < 20 && i + k < flen &&
					       ((file[i+k] >= '0' && file[i+k] <= '9') ||
					        file[i+k] == '.')) {
						if (file[i+k] == '.')
							dots++;
						else
							digits++;
						k++;
					}
					if (dots == 2 && digits >= 5 &&
					    (i == 0 || file[i-1] < '0' ||
					     file[i-1] > '9')) {
						memcpy(ver, file + i, (size_t)k);
						ver[k] = 0;
					}
				}
				if (!cop[0] && file[i] == 'C' &&
				    i + 9 < flen &&
				    !memcmp(file + i, "Copyright", 9)) {
					int k = 0;
					while (k < 70 && i + k < flen &&
					       file[i+k] >= 0x20 && file[i+k] < 0x7F)
						k++;
					if (k >= 20) {
						memcpy(cop, file + i, (size_t)k);
						cop[k] = 0;
					}
				}
			}
			if (ver[0] || cop[0]) {
				snprintf(buf, (size_t)n,
				         "'%s' is %ld bytes%s%s%s%s - find that "
				         "exact build and --from-file will use it",
				         loc->file, loc->file_size,
				         ver[0] ? ", version " : "", ver,
				         cop[0] ? ", " : "", cop);
				rc = 0;
			}
		}
		if (c) {
			long soff = -1;
			int owner = cfb_owner(c, loc->file_offset, &soff);

			if (owner >= 0) {
				snprintf(buf, (size_t)n,
				         "compound document: stream '%s', "
				         "bytes %ld..%ld of %ld",
				         c->ents[owner].path[0]
				           ? c->ents[owner].path
				           : c->ents[owner].name,
				         soff, soff + 512,
				         c->ents[owner].size);
				rc = 0;
			}
			cfb_free(c);
		}
	}
done:
#endif
	free(file);
	return rc;
}

/* ---------------------------------------------------------------- */
/* A copy of the file from somewhere else                            */
/* ---------------------------------------------------------------- */

static const uint8_t *find_bytes(const uint8_t *hay, long hlen,
                                 const uint8_t *ned, long nlen, long from)
{
	long i;

	if (nlen <= 0 || hlen < nlen)
		return NULL;
	for (i = from; i + nlen <= hlen; i++)
		if (hay[i] == ned[0] && !memcmp(hay + i, ned, (size_t)nlen))
			return hay + i;
	return NULL;
}

#define ANCHOR 48
#define NEED   256

int dr_fs_from_file(dr_fs *fs, const dr_fs_loc *loc, const char *path,
                    uint8_t *out, int len, char *how, int howsz)
{
	uint8_t *mine = NULL, *cand = NULL;
	FILE *f;
	long mlen = 0, clen = 0, at;
	int rc = -1;
	long best_back = 0, best_fwd = 0, best_at = -1;

	if (!fs || !loc || !path || !out || loc->area != DR_AREA_FILE)
		return -1;
	f = fopen(path, "rb");
	if (!f) {
		snprintf(how, (size_t)howsz, "cannot open %s", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	clen = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (clen <= 0 || clen > 64L * 1024 * 1024) {
		fclose(f);
		return -1;
	}
	cand = malloc((size_t)clen);
	if (!cand || fread(cand, 1, (size_t)clen, f) != (size_t)clen) {
		fclose(f);
		free(cand);
		return -1;
	}
	fclose(f);

	mine = assemble(fs, loc, NULL, 0, &mlen);
	if (!mine)
		goto out;
	at = loc->file_offset;
	if (at < ANCHOR || at + len > mlen)
		goto out;

	/*
	 * Anchor on the bytes immediately before the damage. They came
	 * from a different sector, one whose CRC passed, so they are
	 * known good - and finding them in the candidate says where in it
	 * this part of the file ended up.
	 */
	{
		const uint8_t *p = cand;
		long from = 0;

		while ((p = find_bytes(cand, clen, mine + at - ANCHOR,
		                       ANCHOR, from)) != NULL) {
			long pos = (p - cand) + ANCHOR;
			long back = 0, fwd = 0;

			while (back < 65536 && at - 1 - back >= 0 &&
			       pos - 1 - back >= 0 &&
			       mine[at - 1 - back] == cand[pos - 1 - back])
				back++;
			while (fwd < 65536 && at + len + fwd < mlen &&
			       pos + len + fwd < clen &&
			       mine[at + len + fwd] == cand[pos + len + fwd])
				fwd++;
			if (back + fwd > best_back + best_fwd) {
				best_back = back;
				best_fwd = fwd;
				best_at = pos;
			}
			from = (p - cand) + 1;
		}
	}

	if (best_at < 0 || best_back < NEED || best_fwd < NEED) {
		snprintf(how, (size_t)howsz,
		         "%s does not line up here - the best match agrees for "
		         "only %ld byte(s) before and %ld after, so it is a "
		         "different build or a different file",
		         path, best_back, best_fwd);
		rc = -1;
		goto out;
	}
	if (best_at + len > clen)
		goto out;

	memcpy(out, cand + best_at, (size_t)len);
	/*
	 * How much the agreement is worth. A few hundred bytes either
	 * side says the file lines up; thousands say it is the same
	 * build, byte for byte, and then the 512 in the middle are not a
	 * guess even when the sector's own checksum disagrees - because
	 * on these disks the checksum is the thing that usually died.
	 */
	rc = (best_back >= 4096 && best_fwd >= 4096) ? 1 : 0;
	snprintf(how, (size_t)howsz,
	         "%s lines up at offset %ld and agrees for %ld byte(s) before "
	         "the damage and %ld after", path, best_at, best_back, best_fwd);
out:
	free(mine);
	free(cand);
	return rc;
}


/* ---------------------------------------------------------------- */
/* PalmOS databases                                                  */
/* ---------------------------------------------------------------- */
/*
 * A .prc or .pdb opens with a 78-byte header - a 32-byte name, a type
 * and creator, a record count - and then a list of records, each
 * declaring where in the file its data starts. Those offsets have to
 * rise, and they have to land inside the file. That is a few dozen
 * constraints where the sector CRC has sixteen, and it costs nothing.
 *
 * The header also carries the database's own name, which matters here
 * for a second reason: erasing a file on a FAT disk destroys the first
 * letter of its name and nothing else. The name inside the file puts
 * that letter back.
 */
static int palm_ok(const uint8_t *b, long len, char *name, size_t namesz,
                   char *how, size_t howsz)
{
	int attr, nrec, res, step, i;
	long need, prev = -1;
	char type[5], creator[5];

	if (len < 78)
		return -1;
	for (i = 0; i < 31 && b[i]; i++)
		if (b[i] < 0x20 || b[i] > 0x7E)
			return -1;       /* not a name, so not one of these */
	if (!b[0])
		return -1;
	attr = (b[32] << 8) | b[33];
	nrec = (b[76] << 8) | b[77];
	res  = attr & 0x0001;
	step = res ? 10 : 8;
	if (nrec < 1 || nrec > 4096)
		return -1;
	need = 78 + (long)nrec * step;
	if (need > len)
		return 0;
	memcpy(type, b + 60, 4);    type[4] = 0;
	memcpy(creator, b + 64, 4); creator[4] = 0;

	for (i = 0; i < nrec; i++) {
		const uint8_t *e = b + 78 + (long)i * step;
		const uint8_t *o = res ? e + 6 : e;
		long off = ((long)o[0] << 24) | ((long)o[1] << 16) |
		           ((long)o[2] << 8) | o[3];

		if (off < need || off > len || off < prev)
			return 0;
		prev = off;
	}
	if (name && namesz) {
		int k;
		for (k = 0; k < 31 && k < (int)namesz - 1 && b[k]; k++)
			name[k] = (char)b[k];
		name[k] = 0;
	}
	snprintf(how, howsz,
	         "PalmOS %s '%.31s' (%s/%s): all %d %s offsets rise and land "
	         "inside the file", res ? "resource database" : "database",
	         b, type, creator, nrec, res ? "resource" : "record");
	return 1;
}

/* Put back the letter the directory lost. Erasing a file overwrites the
 * first byte of its 8.3 name with 0xE5 and nothing else, so if the name
 * inside the file agrees with what is left, the missing letter is not a
 * guess. */
static void palm_restore_name(char *dosname, const char *inner)
{
	size_t i;
	char up[32];

	if (!dosname || dosname[0] != '?' || !inner || !inner[0])
		return;
	for (i = 0; i < sizeof(up) - 1 && inner[i]; i++)
		up[i] = (char)toupper((unsigned char)inner[i]);
	up[i] = 0;
	for (i = 1; dosname[i] && dosname[i] != '.'; i++)
		if (up[i] != dosname[i])
			return;          /* the two do not agree - leave it */
	if (!up[0] || up[0] == '.')
		return;
	dosname[0] = up[0];
}


/* Name a compound document by what is inside it - a Word document, a
 * workbook, a deck - so that a file recovered from a deleted entry can
 * be told apart from the file that has since been written over it. */
static int cfb_describe(const uint8_t *b, long len, char *how, size_t howsz)
{
	cfb *c = cfb_open(b, len);
	int i, n = 0;
	char names[160];

	if (!c)
		return -1;
	names[0] = 0;
	for (i = 0; i < c->nents; i++) {
		size_t at;

		if (!c->ents[i].live || c->ents[i].type != 2)
			continue;
		n++;
		at = strlen(names);
		if (n <= 4 && at + strlen(c->ents[i].name) + 3 < sizeof(names))
			snprintf(names + at, sizeof(names) - at, "%s%s",
			         at ? ", " : "", c->ents[i].name);
	}
	cfb_free(c);
	if (!n)
		return -1;
	snprintf(how, howsz,
	         "compound document, %d stream(s) - %s%s", n, names,
	         n > 4 ? ", ..." : "");
	return 1;
}


/* ---------------------------------------------------------------- */
/* Files made of fixed-size records                                  */
/* ---------------------------------------------------------------- */
/*
 * A database file - Btrieve, dBase, Quicken, QuickBooks - is very often
 * an array of fixed-size records each opening with the same marker.
 * Teres's QDATA.QDB is 4,821 records of 56 bytes, every one of them
 * beginning AB CD.
 *
 * Nothing in the file declares that. It is simply visible: one 2-byte
 * value occurring thousands of times at a constant stride. Once it has
 * been measured from the parts of the file that read cleanly, it says
 * where the marker must appear inside the damaged sector - and a
 * reading that does not put it there is wrong, whatever the sector's
 * checksum says. In a 512-byte sector that is nine independent
 * constraints of sixteen bits each where the CRC offers one.
 */
typedef struct {
	int  marker;             /* the 2-byte value                     */
	long stride;
	long phase;              /* marker offsets are == phase mod stride*/
	long seen;               /* how many were counted                */
	long expected;           /* how many the stride predicts         */
} framing;

/* Try one candidate marker: are its occurrences evenly spaced? */
static int framing_try(const uint8_t *b, long len, int value, long count,
                       framing *out)
{
	long *off = malloc(sizeof(long) * (size_t)count);
	long i, n = 0, modal = 0, modal_n = 0, k, agree = 0;
	uint32_t *gaps;
	int rc = -1;

	if (!off)
		return -1;
	for (i = 0; i + 1 < len && n < count; i++)
		if ((b[i] | (b[i+1] << 8)) == value)
			off[n++] = i;
	if (n < 32) {
		free(off);
		return -1;
	}

	gaps = calloc(1024, sizeof(*gaps));
	if (!gaps) {
		free(off);
		return -1;
	}
	for (k = 1; k < n; k++) {
		long g = off[k] - off[k-1];
		if (g >= 8 && g < 1024)
			gaps[g]++;
	}
	for (k = 8; k < 1024; k++)
		if ((long)gaps[k] > modal_n) {
			modal_n = gaps[k];
			modal = k;
		}
	free(gaps);

	/* The stride has to explain most of the gaps, and the markers
	 * have to share one phase, or this is a coincidence rather than a
	 * record layout. */
	/*
	 * Only the stride is asked for, not a phase. A record file is
	 * usually paged, and each page starts its records afresh - the
	 * markers in Teres's QDB sit at offset 28 within one page and 12
	 * within another. The stride holds everywhere; the phase has to
	 * be taken locally, from the last marker before the sector in
	 * question.
	 */
	/*
	 * Only the stride is asked for, not a phase. A record file is
	 * usually paged and each page starts its records afresh - the
	 * markers in Teres's QDB sit at offset 28 within one page and 12
	 * within another. The stride holds everywhere; the phase has to
	 * be taken locally, from the last marker before the sector.
	 */
	if (modal >= 8 && modal_n * 10 >= (n - 1) * 6) {
		out->marker = value;
		out->stride = modal;
		out->phase = -1;
		out->seen = modal_n;
		out->expected = n - 1;
		rc = 0;
	}
	(void)agree;
	free(off);
	return rc;
}

static int find_framing(const uint8_t *b, long len, framing *out)
{
	uint32_t *count;
	int best[24], nbest = 0, i, j, rc = -1;

	if (len < 4096)
		return -1;
	count = calloc(65536, sizeof(*count));
	if (!count)
		return -1;
	for (i = 0; i + 1 < len; i++)
		count[b[i] | (b[i+1] << 8)]++;

	/*
	 * Not simply the commonest pair - on a database file that is
	 * 00 00, which is filler, not structure. A marker has to be
	 * common enough to be a record start and rare enough not to be
	 * the background: somewhere between 32 occurrences and one per
	 * sixteen bytes of file.
	 */
	for (i = 0; i < 65536; i++) {
		long c = count[i];

		if (c < 32 || c * 16 > len)
			continue;
		for (j = 0; j < nbest; j++)
			if (c > (long)count[best[j]])
				break;
		if (nbest < (int)(sizeof(best)/sizeof(best[0])))
			nbest++;
		{
			int k;
			for (k = nbest - 1; k > j; k--)
				best[k] = best[k-1];
			if (j < nbest)
				best[j] = i;
		}
	}
	/*
	 * And not the first candidate that qualifies, either. Plenty of
	 * byte pairs inside a record occur at the record stride as well -
	 * and one of them, on this file, has a modal gap of 69 that is
	 * pure coincidence and would have been taken first. What
	 * identifies the real marker is how *cleanly* its occurrences sit
	 * on the stride: 91% of the gaps between AB CDs are exactly 56,
	 * where the runner-up manages 66%.
	 */
	{
		framing cur;
		double best_frac = 0.0;

		for (i = 0; i < nbest; i++) {
			double frac;

			if (framing_try(b, len, best[i], count[best[i]],
			                &cur) != 0)
				continue;
			if (cur.expected <= 0)
				continue;
			frac = (double)cur.seen / (double)cur.expected;
			if (frac > best_frac) {
				best_frac = frac;
				*out = cur;
				rc = 0;
			}
		}
	}
	free(count);
	return rc;
}

/* Does this reading put the marker where the record layout says it
 * must be? Returns 1 yes, 0 no, -1 no opinion. */
static int framing_check(const uint8_t *b, long len, long at, long span,
                         char *how, size_t howsz, double *score)
{
	framing f;
	long o, anchor = -1, want = 0, got = 0;

	if (find_framing(b, len, &f) != 0)
		return -1;

	/* The phase comes from the last record start before the sector -
	 * close enough that no page boundary can have intervened. */
	for (o = at - 2; o >= 0 && o > at - 4 * f.stride; o--)
		if ((b[o] | (b[o+1] << 8)) == f.marker) {
			anchor = o;
			break;
		}
	if (anchor < 0)
		return -1;

	for (o = anchor + f.stride; o + 1 < len && o < at + span;
	     o += f.stride) {
		if (o < at)
			continue;
		want++;
		if ((b[o] | (b[o+1] << 8)) == f.marker)
			got++;
	}
	if (want < 2)
		return -1;
	if (score)
		*score = (double)got / (double)want;
	snprintf(how, howsz,
	         "records of %ld bytes, each carrying %02X %02X at the same "
	         "offset: %ld of %ld in this sector land where the layout "
	         "says",
	         f.stride, f.marker & 0xFF, (f.marker >> 8) & 0xFF, got, want);
	return got == want;
}

/* ---------------------------------------------------------------- */
/* What the owner still has                                          */
/* ---------------------------------------------------------------- */

#ifndef DR_NO_ZLIB
/* Count the archive members that still come out.
 *
 * Deliberately not by way of the central directory: the directory is
 * the last thing in the file, so a mark near the outside of the disk
 * takes it out and a reader that depends on it reports an empty
 * archive. Every local header carries its own name, sizes and CRC-32,
 * so they can be found by their signature and checked one at a time -
 * which is how a damaged archive still gives up most of its contents.
 *
 * And then the directory again, because it is worth something after
 * all. An archiver that rewrites a file leaves the old directory
 * behind in the middle of it, and a damaged archive can therefore have
 * a *second* copy of its own catalogue sitting in a part of the disk
 * the mark never reached. That copy does not say where anything is any
 * more - the offsets are from the old layout - but it still says what
 * the archive contained, how long each member is packed, and what its
 * contents must check to. Given a length and a CRC-32, the data can be
 * hunted for: try to inflate that many bytes from every offset and see
 * which one comes out right. Thirty-two bits make a false positive
 * impossible in a file this size.
 *
 * On SLAT that finds a member whose own local header had been
 * overwritten - and finds it twice over, because the archive stored the
 * same animation under two names.
 */

typedef struct {
	char          name[64];
	unsigned long crc;
	long          csz, usz;
	int           meth;
	int           got;
	long          prefix;    /* bytes the stream gives up before it   */
	                         /* breaks, when it cannot be verified    */
} zsurv;

/* Does `csz` bytes at `off` inflate to the recorded length and CRC? */
static int stream_matches(const uint8_t *b, long len, long off,
                          long csz, long usz, unsigned long crc)
{
	if (off < 0 || off + csz > len)
		return 0;
	return inflate_check(b + off, csz, crc, usz) == 1;
}

static void zip_survey(const uint8_t *b, long len,
                       const uint8_t *disk, long disklen, long damaged_at,
                       int *parts, int *ok,
                       int *found, char *lost, size_t lostsz)
{
	zsurv *have = NULL, *want = NULL;
	int nhave = 0, nwant = 0, i, j;
	long p;

	*parts = *ok = 0;
	if (found)
		*found = 0;
	if (lost && lostsz)
		lost[0] = 0;

	have = calloc(MAXENT, sizeof(*have));
	want = calloc(MAXENT, sizeof(*want));
	if (!have || !want) {
		free(have);
		free(want);
		return;
	}

	/* ---- what the local headers say ---- */
	for (p = 0; p + 30 <= len && nhave < MAXENT; p++) {
		long csz, usz, body;
		unsigned long crc;
		int nl, el, meth, good = 0, m;

		if (!(b[p] == 'P' && b[p+1] == 'K' &&
		      b[p+2] == 3 && b[p+3] == 4))
			continue;
		meth = u16le(b + p + 8);
		crc  = (unsigned long)b[p+14] | ((unsigned long)b[p+15] << 8) |
		       ((unsigned long)b[p+16] << 16) |
		       ((unsigned long)b[p+17] << 24);
		csz  = (long)b[p+18] | ((long)b[p+19] << 8) |
		       ((long)b[p+20] << 16) | ((long)b[p+21] << 24);
		usz  = (long)b[p+22] | ((long)b[p+23] << 8) |
		       ((long)b[p+24] << 16) | ((long)b[p+25] << 24);
		nl   = u16le(b + p + 26);
		el   = u16le(b + p + 28);
		if ((meth != 0 && meth != 8) || nl < 1 || nl > 255 ||
		    el > 4096 || csz < 1)
			continue;
		body = p + 30 + nl + el;
		if (body + csz > len)
			continue;
		if (meth == 0) {
			unsigned long c = crc32(0L, Z_NULL, 0);
			c = crc32(c, b + body, (uInt)csz);
			good = (csz == usz && c == crc);
		} else {
			good = stream_matches(b, len, body, csz, usz, crc);
		}
		/* A signature inside compressed data is not an entry: take
		 * it only if it checks out, or if its body ends exactly
		 * where the next one begins. */
		if (!good && !(body + csz + 4 <= len &&
		               b[body+csz] == 'P' && b[body+csz+1] == 'K'))
			continue;
		m = nl < 63 ? nl : 63;
		memcpy(have[nhave].name, b + p + 30, (size_t)m);
		have[nhave].name[m] = 0;
		have[nhave].crc = crc;
		have[nhave].csz = csz;
		have[nhave].usz = usz;
		have[nhave].got = good;
		/*
		 * Only from the bytes that precede the damage. Feeding the
		 * decoder the whole member instead would have it carry on
		 * through the corruption producing rubbish, and report more
		 * "recovered" bytes than the file has.
		 */
		have[nhave].prefix =
		        (!good && damaged_at > body && damaged_at < body + csz)
		          ? inflate_prefix(b + body, damaged_at - body) : -1;
		nhave++;
		(*parts)++;
		*ok += good;
		p = body + csz - 1;
	}

	/* ---- what any surviving copy of the directory says ---- */
	for (p = 0; p + 46 <= len && nwant < MAXENT; p++) {
		long csz, usz;
		unsigned long crc;
		int nl, el, cl, meth, m;

		if (!(b[p] == 'P' && b[p+1] == 'K' &&
		      b[p+2] == 1 && b[p+3] == 2))
			continue;
		meth = u16le(b + p + 10);
		crc  = (unsigned long)b[p+16] | ((unsigned long)b[p+17] << 8) |
		       ((unsigned long)b[p+18] << 16) |
		       ((unsigned long)b[p+19] << 24);
		csz  = (long)b[p+20] | ((long)b[p+21] << 8) |
		       ((long)b[p+22] << 16) | ((long)b[p+23] << 24);
		usz  = (long)b[p+24] | ((long)b[p+25] << 8) |
		       ((long)b[p+26] << 16) | ((long)b[p+27] << 24);
		nl   = u16le(b + p + 28);
		el   = u16le(b + p + 30);
		cl   = u16le(b + p + 32);
		if ((meth != 0 && meth != 8) || nl < 1 || nl > 255 ||
		    csz < 1 || csz > len)
			continue;
		m = nl < 63 ? nl : 63;
		memcpy(want[nwant].name, b + p + 46, (size_t)m);
		want[nwant].name[m] = 0;
		want[nwant].crc = crc;
		want[nwant].csz = csz;
		want[nwant].usz = usz;
		want[nwant].meth = meth;
		nwant++;
		p += 46 + nl + el + cl - 1;
	}

	/* ---- anything the directory names but the headers lost ---- */
	for (i = 0; i < nwant; i++) {
		int already = 0;

		for (j = 0; j < nhave; j++)
			if (have[j].got && have[j].crc == want[i].crc &&
			    have[j].csz == want[i].csz) {
				already = 1;
				break;
			}
		if (already)
			continue;

		/* Hunt for it. Bounded: this only runs for members the
		 * headers could not account for, and most offsets are
		 * rejected by inflate within a byte or two. */
		if (want[i].meth == 8 && len <= 8L * 1024 * 1024) {
			long off;

			for (off = 0; off + want[i].csz <= len; off++) {
				if (!stream_matches(b, len, off, want[i].csz,
				                    want[i].usz, want[i].crc))
					continue;
				want[i].got = 1;
				break;
			}
			/*
			 * And if it is not in the file, the rest of the
			 * disk. A floppy that has been written to more than
			 * once keeps older copies of its files in clusters
			 * nothing has claimed since; the checksum does not
			 * care which file the bytes are filed under.
			 */
			if (!want[i].got && disk && disklen > 0 &&
			    disklen <= 8L * 1024 * 1024) {
				for (off = 0; off + want[i].csz <= disklen;
				     off++) {
					if (!stream_matches(disk, disklen, off,
					                    want[i].csz,
					                    want[i].usz,
					                    want[i].crc))
						continue;
					want[i].got = 1;
					break;
				}
			}
			if (want[i].got) {
				(*parts)++;
				(*ok)++;
				if (found)
					(*found)++;
			}
		}
		if (!want[i].got && lost && lostsz) {
			size_t n = strlen(lost);
			long got_bytes = -1;

			/* Even a member that cannot be repaired is not
			 * necessarily a total loss: feeding the decoder the
			 * stream until it breaks yields everything it had
			 * produced by then. */
			for (j = 0; j < nhave; j++)
				if (have[j].crc == want[i].crc &&
				    have[j].csz == want[i].csz) {
					got_bytes = have[j].prefix;
					break;
				}
			if (n + strlen(want[i].name) + 48 < lostsz)
				snprintf(lost + n, lostsz - n, "%s%s%s",
				         n ? ", " : "", want[i].name,
				         got_bytes > 0 ? "" : "");
			if (got_bytes > 0) {
				n = strlen(lost);
				if (n + 48 < lostsz)
					snprintf(lost + n, lostsz - n,
					         " (%ld of %ld byte(s) still "
					         "readable)", got_bytes,
					         want[i].usz);
			}
		}
	}
	free(have);
	free(want);
}
#endif

int dr_fs_files(dr_fs *fs, dr_fs_file *out, int max)
{
	const dr_fs_info *in;
	int f, n = 0, *cl, *owner = NULL;

	if (!fs || !out)
		return 0;
	in = &fs->info;
	cl = malloc(sizeof(int) * (size_t)(in->clusters + 2));
	if (!cl)
		return 0;

	/* Which live file claims each cluster - two files claiming the
	 * same one is a classic FAT fault and it means at least one of
	 * them is not what it says it is. */
	{
		int g;

		owner = calloc((size_t)in->clusters + 2, sizeof(int));
		for (g = 0; owner && g < fs->nfiles; g++) {
			int m, q;

			if (fs->files[g].deleted)
				continue;
			m = chain(fs, fs->files[g].start, cl, in->clusters + 2);
			for (q = 0; q < m; q++)
				if (cl[q] >= 2 && cl[q] < in->clusters + 2 &&
				    !owner[cl[q]])
					owner[cl[q]] = g + 1;
		}
	}

	for (f = 0; f < fs->nfiles && n < max; f++) {
		dr_fs_file *o = &out[n];
		int nc = chain(fs, fs->files[f].start, cl, in->clusters + 2);
		int k, bad = 0;
		long firstbad = -1;

		memset(o, 0, sizeof(*o));
		snprintf(o->name, sizeof(o->name), "%s", fs->files[f].name);
		o->size = fs->files[f].size;
		o->deleted = fs->files[f].deleted;
		o->chain_bytes = (long)nc * in->spc * SECSZ;
		if (owner && !o->deleted)
			for (k = 0; k < nc; k++) {
				int w = (cl[k] >= 2 && cl[k] < in->clusters + 2)
				        ? owner[cl[k]] : 0;
				if (w && w != f + 1) {
					snprintf(o->cross, sizeof(o->cross),
					         "%s", fs->files[w - 1].name);
					break;
				}
			}
		for (k = 0; k < nc; k++) {
			int j;
			for (j = 0; j < in->spc; j++) {
				long lba = in->data_lba +
				           (long)(cl[k] - 2) * in->spc + j;
				if (lba >= 0 && lba < fs->nlba &&
				    fs->have[lba] == 2) {
					bad++;
					if (firstbad < 0)
						firstbad = (long)k * in->spc *
						           SECSZ + (long)j * SECSZ;
				}
			}
		}
		o->bad = bad;
#ifndef DR_NO_ZLIB
		if (!o->deleted) {
			dr_fs_loc l;
			long flen = 0;
			uint8_t *b;

			memset(&l, 0, sizeof(l));
			snprintf(l.file, sizeof(l.file), "%s", o->name);
			l.file_offset = -1;
			b = assemble(fs, &l, NULL, 0, &flen);
			if (b) {
				if (flen > 30 && b[0] == 'P' && b[1] == 'K')
					zip_survey(b, flen, fs->img,
					           fs->nlba * SECSZ, firstbad,
					           &o->parts, &o->parts_ok,
					           &o->found, o->lost,
					           sizeof(o->lost));
				free(b);
			}
		}
#endif
		/* Formats that describe themselves get looked at. */
		{
			long flen = 0;
			uint8_t *b = dr_fs_read(fs, f, &flen);

			if (b && flen > 78) {
				char inner[32], msg[240];

				inner[0] = 0;
				if (palm_ok(b, flen, inner, sizeof(inner),
				            msg, sizeof(msg)) == 1) {
					snprintf(o->note, sizeof(o->note),
					         "%s", msg);
					palm_restore_name(o->name, inner);
				} else if (cfb_describe(b, flen, msg,
				                        sizeof(msg)) == 1) {
					snprintf(o->note, sizeof(o->note),
					         "%s", msg);
				}
			}
			free(b);
		}

		if (o->deleted) {
			/*
			 * For a deleted file the question is not whether it
			 * reads but whether anything has been written over
			 * it. Its clusters are free as far as the FAT is
			 * concerned, so what matters is how many of them a
			 * live file has since claimed.
			 */
			long cluster = (long)in->spc * SECSZ;
			int need = (int)((o->size + cluster - 1) / cluster);
			int taken = 0, q;

			if (need > in->clusters)
				need = in->clusters;
			for (q = 0; owner && q < need; q++) {
				int c = fs->files[f].start + q;
				if (c >= 2 && c < in->clusters + 2 && owner[c])
					taken++;
			}
			o->reused = taken;
			if (o->note[0]) {
				size_t at = strlen(o->note);
				snprintf(o->note + at, sizeof(o->note) - at,
				         "; deleted, %s",
				         taken ? "and partly overwritten"
				               : "and nothing has overwritten "
				                 "it");
			} else if (!taken) {
				snprintf(o->note, sizeof(o->note),
				         "deleted, and no live file has taken "
				         "its %d cluster(s) back", need);
			} else {
				snprintf(o->note, sizeof(o->note),
				         "deleted, and %d of its %d cluster(s) "
				         "have been given to other files",
				         taken, need);
			}
		} else if (o->note[0]) {
			/* a format-level verdict wins over the generic
			 * bookkeeping below */
		} else if (o->cross[0]) {
			snprintf(o->note, sizeof(o->note),
			         "its clusters are also claimed by %s - the "
			         "FAT has them cross-linked, so at most one "
			         "of the two is whole", o->cross);
		} else if (o->chain_bytes < o->size) {
			snprintf(o->note, sizeof(o->note),
			         "its cluster chain gives out after %ld of "
			         "%ld byte(s) - the FAT entry that should "
			         "continue it is wrong", o->chain_bytes,
			         o->size);
		} else if (o->parts) {
			int k = snprintf(o->note, sizeof(o->note),
			                 "%d of %d archive member(s) still "
			                 "extract", o->parts_ok, o->parts);
			if (o->found && k > 0 && k < (int)sizeof(o->note))
				k += snprintf(o->note + k,
				              sizeof(o->note) - k,
				              " (%d of them found through a "
				              "second copy of the archive's "
				              "own directory)", o->found);
			if (o->lost[0] && k > 0 && k < (int)sizeof(o->note))
				snprintf(o->note + k, sizeof(o->note) - k,
				         "; lost: %s", o->lost);
		}
		else if (!bad)
			snprintf(o->note, sizeof(o->note), "intact");
		n++;
	}
	free(owner);
	free(cl);
	return n;
}

#ifndef DR_NO_ZLIB
/*
 * Where the first unreadable sector lands inside this file's bytes,
 * counting only from offset `from`, or -1 if everything from there on
 * decoded.
 *
 * Asking per member rather than per file matters: the damage sits in
 * one member, and every member after it is as sound as every member
 * before. Taking the file's first bad sector as the end of the good
 * data throws away the whole tail of the archive for no reason.
 */
static long next_bad_offset(dr_fs *fs, int index, long from)
{
	const dr_fs_info *in = &fs->info;
	int *cl, n, k, j;

	cl = malloc(sizeof(int) * (size_t)(in->clusters + 2));
	if (!cl)
		return -1;
	n = chain(fs, fs->files[index].start, cl, in->clusters + 2);
	for (k = 0; k < n; k++)
		for (j = 0; j < in->spc; j++) {
			long lba, at = (long)k * in->spc * SECSZ +
			               (long)j * SECSZ;

			if (at + SECSZ <= from)
				continue;
			lba = in->data_lba +
			      (long)(cl[k] - 2) * in->spc + j;
			if (lba >= 0 && lba < fs->nlba && fs->have[lba] == 2) {
				free(cl);
				return at;
			}
		}
	free(cl);
	return -1;
}

/* A member name, reduced to something safe to create on disk. */
static void member_path(char *dst, size_t dstsz, const char *dir,
                        const uint8_t *nm, int nl)
{
	char leaf[256];
	int i, j = 0;

	for (i = 0; i < nl && i < 255; i++) {
		int ch = nm[i];

		if (ch == '/' || ch == '\\')
			j = 0;                   /* keep the last component */
		else if (ch >= 32 && ch < 127 && ch != ':' && ch != '"')
			leaf[j++] = (char)ch;
	}
	leaf[j] = 0;
	if (!j)
		snprintf(leaf, sizeof(leaf), "member");
	snprintf(dst, dstsz, "%s/%s", dir, leaf);
}

/*
 * Write out what is still readable of every member of a damaged
 * archive.
 *
 * A ZIP entry carries a CRC-32 of its own contents, so each member can
 * be judged on its own: the ones the damage misses come out whole and
 * proven, and the ones it hits still give up everything the
 * decompressor had produced before it reached the bad byte. That
 * prefix is often most of the file - deflate is streaming, so the
 * damage costs the tail, not the whole thing.
 *
 * Returns the number of members written, or -1.
 */
int dr_fs_salvage(dr_fs *fs, int index, const char *dir,
                  dr_fs_member *out, int max)
{
	uint8_t *b = NULL;
	long len = 0, damaged_at, p;
	int n = 0;

	if (!fs || index < 0 || index >= fs->nfiles || !dir)
		return -1;
	b = dr_fs_read(fs, index, &len);
	if (!b || len < 30) {
		free(b);
		return -1;
	}
	if (!(b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4)) {
		free(b);
		return 0;                       /* not an archive */
	}

	for (p = 0; p + 30 <= len && (!out || n < max); p++) {
		long csz, usz, body, got, keep;
		unsigned long crc;
		uint8_t *data = NULL;
		int nl, el, meth, whole;
		char path[2400], note[200];
		FILE *f;

		if (!(b[p] == 'P' && b[p+1] == 'K' &&
		      b[p+2] == 3 && b[p+3] == 4))
			continue;
		meth = u16le(b + p + 8);
		crc  = (unsigned long)b[p+14] | ((unsigned long)b[p+15] << 8) |
		       ((unsigned long)b[p+16] << 16) |
		       ((unsigned long)b[p+17] << 24);
		csz  = (long)b[p+18] | ((long)b[p+19] << 8) |
		       ((long)b[p+20] << 16) | ((long)b[p+21] << 24);
		usz  = (long)b[p+22] | ((long)b[p+23] << 8) |
		       ((long)b[p+24] << 16) | ((long)b[p+25] << 24);
		nl   = u16le(b + p + 26);
		el   = u16le(b + p + 28);
		if ((meth != 0 && meth != 8) || nl < 1 || nl > 255 ||
		    el > 4096 || csz < 1)
			continue;
		body = p + 30 + nl + el;
		if (body + csz > len)
			continue;

		/* How much of this member's compressed bytes we trust. */
		damaged_at = next_bad_offset(fs, index, body);
		whole = (damaged_at < 0 || damaged_at >= body + csz);
		if (meth == 0) {
			long have = whole ? csz : damaged_at - body;

			if (have < 0)
				have = 0;
			got = have;
			data = malloc((size_t)(got + 2));
			if (data && got)
				memcpy(data, b + body, (size_t)got);
		} else {
			long feed = whole ? csz : damaged_at - body;

			if (feed < 0)
				feed = 0;
			got = feed > 1 ? inflate_prefix_to(b + body, feed,
			                                   &data) : 0;
			if (got > 0 && data) {
				uint8_t *rm = realloc(data,
				                      (size_t)(got + 2));
				if (rm)
					data = rm;
			}
		}
		if (got <= 0 || !data) {
			free(data);
			continue;
		}

		keep = got;
		note[0] = 0;
		if (whole && got == usz &&
		    crc32(crc32(0L, NULL, 0), data, (uInt)got) == crc)
			snprintf(note, sizeof(note),
			         "complete - its own CRC-32 agrees");
		else {
			long cl2 = gif_close(data, got, usz, note,
			                     sizeof(note));

			if (cl2 > 0)
				keep = cl2;
			else
				snprintf(note, sizeof(note),
				         "%ld of %ld byte(s) readable before "
				         "the damage", got, usz);
		}

		member_path(path, sizeof(path), dir, b + p + 30, nl);
		f = fopen(path, "wb");
		if (f) {
			fwrite(data, 1, (size_t)keep, f);
			fclose(f);
			if (out) {
				snprintf(out[n].name, sizeof(out[n].name),
				         "%s", path);
				out[n].size = keep;
				out[n].full = usz;
				out[n].whole = (note[0] == 'c');
				snprintf(out[n].note, sizeof(out[n].note),
				         "%s", note);
			}
			n++;
		}
		free(data);
		p = body + csz - 1;
	}
	free(b);
	return n;
}
#else
int dr_fs_salvage(dr_fs *fs, int index, const char *dir,
                  dr_fs_member *out, int max)
{
	(void)fs; (void)index; (void)dir; (void)out; (void)max;
	return -1;
}
#endif /* DR_NO_ZLIB */

uint8_t *dr_fs_read(dr_fs *fs, int index, long *len)
{
	const dr_fs_info *in;
	uint8_t *b;
	int *cl, n, k;
	long size;

	if (!fs || !len || index < 0 || index >= fs->nfiles)
		return NULL;
	in = &fs->info;
	size = fs->files[index].size;
	if (size <= 0 || size > 32L * 1024 * 1024)
		return NULL;

	cl = malloc(sizeof(int) * (size_t)(in->clusters + 2));
	if (!cl)
		return NULL;
	n = chain(fs, fs->files[index].start, cl, in->clusters + 2);
	/*
	 * A deleted file usually has no chain left - DOS frees the FAT
	 * entries and only the directory entry remembers where it began.
	 * When the links are gone, read the clusters consecutively from
	 * that start: a file written to a freshly formatted floppy is
	 * almost always contiguous, and it is the only guess available.
	 */
	if (fs->files[index].deleted &&
	    (long)n * in->spc * SECSZ < size) {
		n = (int)((size + (long)in->spc * SECSZ - 1) /
		          ((long)in->spc * SECSZ));
		for (k = 0; k < n; k++)
			cl[k] = fs->files[index].start + k;
	}

	{
		long cluster = (long)in->spc * SECSZ;
		long room = ((size + cluster - 1) / cluster) * cluster;

		b = calloc((size_t)room + 1, 1);
		if (!b) {
			free(cl);
			return NULL;
		}
		for (k = 0; k < n; k++) {
			long src, want = cluster;
			long dst = (long)k * cluster;

			if (dst >= room || cl[k] < 2 ||
			    cl[k] >= in->clusters + 2)
				break;
			if (dst + want > room)
				want = room - dst;
			src = (in->data_lba + (long)(cl[k] - 2) * in->spc) *
			      SECSZ;
			if (src + want <= fs->nlba * SECSZ)
				memcpy(b + dst, fs->img + src, (size_t)want);
		}
	}
	free(cl);
	*len = size;
	return b;
}
