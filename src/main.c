/*
 * DisketteRecover - command line front end.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>

#include "dr_internal.h"

extern const char dr_web_plot[];

static const char *usage_text =
"DisketteRecover - CRC-guided flux-level floppy repair (built on libhxcfe)\n"
"\n"
"usage: disketterecover <command> [options] <image>\n"
"\n"
"commands:\n"
"  scan     <image>              list every sector and flag CRC failures\n"
"  inspect  <image>              zoomed view of one sector's cells+bytes\n"
"  repair   <image>              search for the most likely CRC-valid fix\n"
"  damage   <image>              flip data bits on purpose (test images)\n"
"  serve    <image>              browser UI for inspect + repair\n"
"  convert  <image>              write the image out in another format\n"
"  plot     <image>              write an interactive scatter of the flux\n"
"                                transition widths to --out FILE.html\n"
"  extract  <image>              write out the files the disk holds,\n"
"                                deleted ones included\n"
"  formats                       list libhxcfe export formats\n"
"\n"
"inspect options:\n"
"  --bytes A:B       zoom in on message bytes A..B - shows the cells, the\n"
"                    flux interval each reversal came from, and the bits\n"
"                    they decode to\n"
"\n"
"selection:\n"
"  --sector N        sector index from `scan` (default: first bad CRC)\n"
"  --track T --side S --id R     select by physical address instead\n"
"\n"
"engine options:\n"
"  --mode M          auto | pattern | revs | rebin | bits (default auto)\n"
"                      pattern: restore the repeat the data almost obeys\n"
"                      revs   : let every pass in the dump vote on where\n"
"                               the reversals are (flux dumps only)\n"
"                      rebin  : re-read the flux under another legal\n"
"                               binning of the transitions (MFM + flux)\n"
"                      bits   : search bit flips (works without flux)\n"
"                    auto runs them all and ranks on one scale\n"
"  --max-outliers N  pattern engine: bytes allowed off-pattern (24)\n"
"  --dropout-bias N  nats favouring a lost 1 over a gained 1 (1.6)\n"
"  --burst-gain G    how much likelier an error is right after another\n"
"                    one - errors clump, so a flip beside a flip is one\n"
"                    event, not two (110; 0 turns the burst prior off)\n"
"  --burst-len B     bits over which that lift decays (60)\n"
"  --smooth W        how smooth to expect the disturbance to be. A\n"
"                    speck, a scratch or the reader's own PLL acts over\n"
"                    a stretch of track, so the displacement drifts in\n"
"                    and out rather than jumping. W trades the default\n"
"                    prior (each reversal displaced on its own) for\n"
"                    that one: W/(1+W) of the cost falls on the change\n"
"                    in displacement instead of its size. 0 is the\n"
"                    default; auto mode tries 16 on its own when the\n"
"                    ordinary pass comes back empty\n"
"  --restore-only    only consider putting dropped reversals back;\n"
"                    every verified error so far has been a 1 read as 0\n"
"  --bin-budget N    nats of timing cost a re-bin may spend (12)\n"
"  --max-ambiguous N refuse to search past this many open intervals (40)\n"
"  --rebin-width N   re-readings kept per disturbed stretch (24). A\n"
"                    disturbance spread over many reversals has a long\n"
"                    tail of near-equal re-readings, so the right one\n"
"                    can sit some way down; raise this to go deeper\n"
"  --max-explore N   cap on re-binning assignments tested (2000000)\n"
"\n"
"model options:\n"
"  --threshold P     p_err below this is 'assumed good' (default 1e-3)\n"
"  --base-perr P     prior error rate with no timing evidence (2e-4)\n"
"  --jitter S        flux jitter sigma in cell periods (0.16)\n"
"  --max-weight N    deepest error weight to search (3, max 6)\n"
"  --max-pool N      how many low-confidence bits to consider (96)\n"
"  --max-results N   cap on returned candidates (256)\n"
"\n"
"repair options:\n"
"  --all             work through every sector with a CRC error\n"
"  --apply K         apply candidate K (0 = most likely) and verify\n"
"  --auto            apply candidate 0 only when it clearly wins\n"
"  --out FILE        write the patched image\n"
"  --format NAME     export format for --out (default: hfe)\n"
"\n"
"filesystem options (scan and repair):\n"
"  --fs              say what each bad sector actually is: free space,\n"
"                    a FAT with a second copy on the disk, or so many\n"
"                    bytes of a named file - and, where the file format\n"
"                    carries a checksum of its own, put every candidate\n"
"                    reading to it. Thirty-two bits about the data beats\n"
"                    sixteen about the sector.\n"
"  --from-copy       write the second copy of these bytes that is\n"
"                    already on this disk - the other FAT, or the same\n"
"                    archive entry stored twice - and re-stamp the CRC.\n"
"                    Not a ranking: the archive's CRC-32 confirms it.\n"
"                    (--from-mirror is the same switch.)\n"
"  --from-file F     a copy of the same file from somewhere else -\n"
"                    another disk, an archive, a download. Its bytes\n"
"                    are anchored on the damaged sector's known-good\n"
"                    neighbours, and applied only if they reproduce\n"
"                    the sector's stored CRC.\n"
"  --salvage         extract: also unpack damaged archives, member by\n"
"                    member, into <name>.d/ - a member the damage\n"
"                    missed comes out whole and proven by its own\n"
"                    CRC-32, and the one it hit gives up everything\n"
"                    readable before it (a GIF is trimmed to its last\n"
"                    whole block and closed off, so it still opens)\n"
"  --deleted         extract: include files whose directory entry was\n"
"                    erased - their data is often still there\n"
"  --variants N      write one image per reading: FILE_a1.hfe,\n"
"                    FILE_a2.hfe ... Open them in a disk browser and\n"
"                    see which one's files still make sense. With\n"
"                    --all these are whole-disk guesses: image k takes\n"
"                    the k-th reading of every sector that would not\n"
"                    settle, so each one is a coherent account of the\n"
"                    disk rather than a mixture of ranks\n"
"  --out with --all  may carry {fixed} and {bad}, e.g.\n"
"                    --out \"Disk_Sand_{fixed}of{bad}.hfe\" writes\n"
"                    Disk_Sand_3of4.hfe - so a directory of repaired\n"
"                    images says how much of each one came back\n"
"\n"
"damage options:\n"
"  --bits a,b,c      message bit indices to flip\n"
"  --slip B:N        shift the sector's cells by N from byte B on, the\n"
"                    way a decoder does when it loses its place\n"
"  --drop-only       only clear bits that read 1, so the damage is a\n"
"                    lost reversal - what real media actually does\n"
"  --out FILE        required\n"
"\n"
"other:\n"
"  --set NAME=VALUE  override a libhxcfe setting before loading, e.g.\n"
"                    --set FLUXSTREAM_PLL_MAX_ERROR_NS=900 (repeatable)\n"
"  --json            machine readable output\n"
"  --port N          port for `serve` (default 842)\n"
"  --bind ADDR       bind address for `serve` (default 127.0.0.1)\n"
"  -v / -vv          libhxcfe chatter\n";

typedef struct {
	const char *cmd;
	const char *image;
	int    sector;
	int    track, side, id;
	int    json;
	int    verbose;
	int    apply;
	int    autoapply;
	int    all;
	int    droponly;
	int    port;
	const char *bind;
	const char *out;
	const char *format;
	const char *bits;
	const char *slip;
	const char *bytes;
	int    fs;
	int    variants;
	int    frommirror;
	const char *fromfile;
	int    deleted;
	int    salvage;
	char *const *sets;
	int    nsets;
	dr_options opt;
} args;

static int parse_args(int argc, char **argv, args *a)
{
	int i;
	static char *setbuf[32];

	memset(a, 0, sizeof(*a));
	a->sets = setbuf;
	dr_options_default(&a->opt);
	a->sector = -1;
	a->track = a->side = a->id = -1;
	a->apply = -1;
	a->port = 842;
	a->bind = "127.0.0.1";
	a->format = "HXC_HFE";

	if (argc < 2)
		return -1;
	a->cmd = argv[1];

	for (i = 2; i < argc; i++) {
		const char *o = argv[i];
		#define NEXT() (++i < argc ? argv[i] : "")

		if (!strcmp(o, "--sector"))        a->sector = atoi(NEXT());
		else if (!strcmp(o, "--track"))    a->track = atoi(NEXT());
		else if (!strcmp(o, "--side"))     a->side = atoi(NEXT());
		else if (!strcmp(o, "--id"))       a->id = atoi(NEXT());
		else if (!strcmp(o, "--threshold"))a->opt.good_threshold = atof(NEXT());
		else if (!strcmp(o, "--base-perr"))a->opt.base_perr = atof(NEXT());
		else if (!strcmp(o, "--jitter"))   a->opt.jitter = atof(NEXT());
		else if (!strcmp(o, "--max-weight"))a->opt.max_weight = atoi(NEXT());
		else if (!strcmp(o, "--max-pool")) a->opt.max_pool = atoi(NEXT());
		else if (!strcmp(o, "--max-results"))a->opt.max_results = atoi(NEXT());
		else if (!strcmp(o, "--bin-budget")) a->opt.bin_budget = atof(NEXT());
		else if (!strcmp(o, "--max-explore")) a->opt.max_explore = atol(NEXT());
		else if (!strcmp(o, "--max-ambiguous")) a->opt.max_ambiguous = atoi(NEXT());
		else if (!strcmp(o, "--rebin-width")) a->opt.rebin_width = atoi(NEXT());
		else if (!strcmp(o, "--max-outliers")) a->opt.max_outliers = atoi(NEXT());
		else if (!strcmp(o, "--dropout-bias")) a->opt.dropout_bias = atof(NEXT());
		else if (!strcmp(o, "--burst-gain")) a->opt.burst_gain = atof(NEXT());
		else if (!strcmp(o, "--burst-len"))  a->opt.burst_len  = atof(NEXT());
		else if (!strcmp(o, "--smooth"))     a->opt.smooth     = atof(NEXT());
		else if (!strcmp(o, "--restore-only")) a->opt.restore_only = 1;
		else if (!strcmp(o, "--drop-only")) a->droponly = 1;
		else if (!strcmp(o, "--mode")) {
			const char *m = NEXT();
			if (!strcmp(m, "bits"))         a->opt.mode = DR_MODE_BITS;
			else if (!strcmp(m, "rebin"))   a->opt.mode = DR_MODE_REBIN;
			else if (!strcmp(m, "pattern")) a->opt.mode = DR_MODE_PATTERN;
			else if (!strcmp(m, "revs"))    a->opt.mode = DR_MODE_REVS;
			else                            a->opt.mode = DR_MODE_AUTO;
		}
		else if (!strcmp(o, "--apply"))    a->apply = atoi(NEXT());
		else if (!strcmp(o, "--auto"))     a->autoapply = 1;
		else if (!strcmp(o, "--all"))      a->all = 1;
		else if (!strcmp(o, "--fs"))       a->fs = 1;
		else if (!strcmp(o, "--salvage")) a->salvage = 1;
		else if (!strcmp(o, "--deleted"))  a->deleted = 1;
		else if (!strcmp(o, "--from-mirror") ||
		         !strcmp(o, "--from-copy")) a->frommirror = 1;
		else if (!strcmp(o, "--variants")) a->variants = atoi(NEXT());
		else if (!strcmp(o, "--from-file")) { a->fromfile = NEXT();
		                                     a->frommirror = 1; }
		else if (!strcmp(o, "--out"))      a->out = NEXT();
		else if (!strcmp(o, "--format"))   a->format = NEXT();
		else if (!strcmp(o, "--bits"))     a->bits = NEXT();
		else if (!strcmp(o, "--slip"))     a->slip = NEXT();
		else if (!strcmp(o, "--bytes"))    a->bytes = NEXT();
		else if (!strcmp(o, "--port"))     a->port = atoi(NEXT());
		else if (!strcmp(o, "--bind"))     a->bind = NEXT();
		else if (!strcmp(o, "--set")) {
			if (a->nsets < 32)
				setbuf[a->nsets++] = (char *)NEXT();
			else
				(void)NEXT();
		}
		else if (!strcmp(o, "--json"))     a->json = 1;
		else if (!strcmp(o, "-v"))         a->verbose = 1;
		else if (!strcmp(o, "-vv"))        a->verbose = 2;
		else if (o[0] == '-') {
			fprintf(stderr, "unknown option '%s'\n", o);
			return -1;
		} else if (!a->image)
			a->image = o;
		else {
			fprintf(stderr, "unexpected argument '%s'\n", o);
			return -1;
		}
		#undef NEXT
	}
	return 0;
}

/* Resolve --sector / --track/--side/--id / first bad. */
/*
 * One mark, several tracks.
 *
 * A sector id is an angular position and consecutive tracks are radially
 * adjacent, so the same id failing on a run of tracks is not several
 * faults - it is one scratch, one speck, one contact transfer, crossing
 * them. Five of the six disks this was built against fail exactly that
 * way, and on the ones where it can be measured the damage lands in the
 * same byte range of each sector, which is the same angular span. Worth
 * saying out loud: it tells the reader the defect is physical and where
 * on the disk it is, and it warns that a neighbouring track is likely to
 * be marginal even where its CRC still passes.
 */
static void print_radial(dr_ctx *c)
{
	const dr_sector *s;
	int n, i, j, side, id;

	s = dr_sectors(c, &n);
	if (!s)
		return;

	for (side = 0; side <= 1; side++)
		for (id = 1; id <= 64; id++) {
			int lo = -1, hi = -1, cnt = 0;

			for (i = 0; i < n; i++) {
				if (s[i].side != side || s[i].sector_id != id)
					continue;
				if (s[i].data_crc != DR_CRC_BAD &&
				    s[i].header_crc != DR_CRC_BAD)
					continue;
				if (lo < 0 || s[i].track < lo) lo = s[i].track;
				if (s[i].track > hi) hi = s[i].track;
				cnt++;
			}
			if (cnt < 2)
				continue;

			/* Only interesting if the tracks are close: the same
			 * id failing at opposite ends of the disk is two
			 * faults, not one mark. */
			if (hi - lo > cnt + 4)
				continue;

			printf("            sector %d on side %d fails across "
			       "tracks %d-%d (%d of them)", id, side, lo, hi,
			       cnt);
			for (j = 0, i = 0; i < n; i++)
				if (s[i].side == side && s[i].sector_id == id &&
				    (s[i].data_crc == DR_CRC_BAD ||
				     s[i].header_crc == DR_CRC_BAD))
					j++;
			printf(" - one physical mark, not %d faults\n", j);
		}
}

static int pick_sector(dr_ctx *c, const args *a)
{
	int i, n;
	const dr_sector *s = dr_sectors(c, &n);

	if (a->sector >= 0) {
		if (a->sector >= n) {
			fprintf(stderr, "sector index %d out of range (%d sectors)\n",
			        a->sector, n);
			return -1;
		}
		return a->sector;
	}

	if (a->track >= 0 || a->id >= 0) {
		for (i = 0; i < n; i++) {
			if (a->track >= 0 && s[i].track != a->track) continue;
			if (a->side  >= 0 && s[i].side  != a->side)  continue;
			if (a->id    >= 0 && s[i].sector_id != a->id) continue;
			return i;
		}
		fprintf(stderr, "no sector matches the given address\n");
		return -1;
	}

	i = dr_first_bad(c);
	if (i < 0) {
		if (n > 0)
			fprintf(stderr, "no CRC error found; pass --sector to pick one\n");
		else
			fprintf(stderr, "no sectors decoded from this image\n");
		return -1;
	}
	return i;
}

/* ------------------------------------------------------------------ */
static void print_scan(dr_ctx *c)
{
	int i, n, bad = 0;
	const dr_sector *s = dr_sectors(c, &n);

	printf("image     : %s\n", dr_path(c));
	printf("geometry  : %d tracks x %d sides, %d sectors decoded\n",
	       dr_tracks(c), dr_sides(c), n);
	printf("\n idx  trk sd  ord   id  size  enc      hdr-crc  data-crc  data-cell\n");
	printf(" ---- --- --  ---  ---  ----  -------  -------  --------  ---------\n");

	for (i = 0; i < n; i++) {
		int isbad = (s[i].header_crc == DR_CRC_BAD ||
		             s[i].data_crc == DR_CRC_BAD);
		if (isbad)
			bad++;
		printf(" %4d %3d  %d  %3d  %3d  %4d  %-7s  %-7s  %-8s  %9d%s\n",
		       i, s[i].track, s[i].side, s[i].order, s[i].sector_id,
		       s[i].sector_size,
		       s[i].encoding == DR_ENC_ISO_MFM ? "ISO/MFM" : "ISO/FM",
		       s[i].header_crc == DR_CRC_OK ? "ok" : "BAD",
		       s[i].data_crc == DR_CRC_OK ? "ok" :
		           (s[i].data_crc == DR_CRC_BAD ? "BAD" : "-"),
		       s[i].data_cell, isbad ? "  <--" : "");
	}

	printf("\n%d sector(s) with a CRC error", bad);
	i = dr_first_bad(c);
	if (i >= 0)
		printf("; first is index %d (track %d side %d sector %d)",
		       i, s[i].track, s[i].side, s[i].sector_id);
	printf("\n");
	print_radial(c);
}

/* ------------------------------------------------------------------ */
static const char *bar(double p)
{
	if (p >= 0.30) return "#####";
	if (p >= 0.10) return "#### ";
	if (p >= 0.02) return "###  ";
	if (p >= 1e-3) return "##   ";
	if (p >= 1e-5) return "#    ";
	return "     ";
}

/*
 * Where the passes vote against the reading libhxcfe picked.
 *
 * Only worth printing when they actually differ, and then only the bytes
 * that changed - on a text sector this is usually self-evidently right
 * or self-evidently wrong at a glance, which is more than a CRC can say
 * when there are more contested reversals than it has bits.
 */
static void print_majority(dr_view *v)
{
	uint8_t *msg;
	int contested = 0, majority = 0, i, shown = 0;

	if (v->nrev_used < 2)
		return;
	msg = malloc((size_t)v->msg_len);
	if (!msg)
		return;
	if (dr_revs_reading(v, msg, &contested, &majority) < 0 || !majority) {
		free(msg);
		return;
	}

	printf("\nthe passes vote for a different reading\n");
	printf("  %d reversal(s) were not unanimous; the majority overrules "
	       "this reading on %d of them\n", contested, majority);

	for (i = 0; i < v->msg_len; i++) {
		if (msg[i] == v->msg[i])
			continue;
		if (shown == 0)
			printf("  byte   was   ->  passes say\n");
		if (shown++ == 24) {
			printf("  ... and %d more\n",
			       v->msg_len - i);
			break;
		}
		printf("  %4d   %02X %c  ->  %02X %c\n", i,
		       v->msg[i], isprint(v->msg[i]) ? v->msg[i] : '.',
		       msg[i], isprint(msg[i]) ? msg[i] : '.');
	}
	if (!shown)
		printf("  ...and decodes to the same bytes anyway\n");
	free(msg);
}

static void print_view(dr_view *v, int top)
{
	int i, k, n;
	int *ord;

	printf("sector    : index %d - track %d side %d, id %d, %d bytes\n",
	       v->sector_index, v->sect.track, v->sect.side, v->sect.sector_id,
	       v->sect.sector_size);
	printf("encoding  : %s, %d cells/byte, bitrate %d\n",
	       v->encoding == DR_ENC_ISO_MFM ? "ISO/IBM MFM" : "ISO/IBM FM",
	       v->stride, v->sect.bitrate);
	printf("field     : %s field, %d bytes of CRC-covered message "
	       "starting at cell %d\n", v->field, v->msg_len, v->base_cell);
	printf("crc       : stored %04X, computed %04X, syndrome %04X -> %s\n",
	       v->stored_crc, v->computed_crc, v->syndrome,
	       v->syndrome ? "INVALID" : "valid");
	if (v->crc_suspect && v->syndrome)
		printf("            ~%.1f of these 16 bits are themselves in "
		       "doubt, so the stored value is a\n"
		       "            guess too - a reading that matches it has "
		       "proved less than it looks\n",
		       v->crc_expected_errors);
	printf("evidence  : %s%s\n", v->model,
	       v->flux_available ? "" : "  (no flux stream in this image)");
	if (v->regions[0])
		printf("timing    : %s\n", v->regions);
	if (v->passes[0])
		printf("passes    : %s\n", v->passes);
	print_majority(v);

	/* --- decoded bytes with a confidence bar ---------------------- */
	printf("\ndecoded message (worst-bit confidence bar per byte)\n");
	for (i = 0; i < v->msg_len; i += 16) {
		printf("  %04X ", i);
		for (k = 0; k < 16 && i + k < v->msg_len; k++)
			printf("%02X ", v->msg[i + k]);
		for (; k < 16; k++)
			printf("   ");
		printf(" |");
		for (k = 0; k < 16 && i + k < v->msg_len; k++) {
			unsigned char ch = v->msg[i + k];
			putchar((ch >= 32 && ch < 127) ? ch : '.');
		}
		printf("|\n");
	}

	/* --- least trusted bits --------------------------------------- */
	if (top <= 0)
		return;
	n = v->msg_bits - v->first_bit;
	if (n <= 0)
		return;
	ord = malloc((size_t)n * sizeof(int));
	if (!ord)
		return;
	for (i = 0; i < n; i++)
		ord[i] = v->first_bit + i;
	for (i = 1; i < n; i++) {          /* insertion sort, n is small */
		int key = ord[i], j = i - 1;
		while (j >= 0 && v->bit_perr[ord[j]] < v->bit_perr[key]) {
			ord[j + 1] = ord[j];
			j--;
		}
		ord[j + 1] = key;
	}

	printf("\nleast trusted bits in this field (the search starts here)\n");
	printf("  bit    byte  role  bitpos  cell    state  interval      bin   "
	       "margin  p_err   evidence\n");
	for (i = 0; i < top && i < n; i++) {
		int p = ord[i];
		int byte = p >> 3, bit = p & 7;
		int cc, dc;
		dr_cell *dcell;

		dr_bit_cells(v->encoding, byte * v->stride, bit, &cc, &dc);
		dcell = &v->cells[dc];

		printf("  %-6d %-5d %-5s %-7d %-7d %-6d ", p, byte,
		       v->bytes[byte].role, bit, dcell->cell, dcell->state);
		if (dcell->interval_ticks >= 0)
			printf("%6.3fT/%-5d ", dcell->interval_cells,
			       dcell->interval_ticks);
		else
			printf("%-13s ", "-");
		if (dcell->bin >= 0)
			printf("%-5d ", dcell->bin);
		else
			printf("%-5s ", "-");
		if (dcell->margin >= 0.0f)
			printf("%-7.3f ", dcell->margin);
		else
			printf("%-7s ", "-");
		printf("%-7.2g %s %s\n", v->bit_perr[p],
		       dcell->evidence == DR_EV_FLUX ? "flux" :
		       dcell->evidence == DR_EV_WEAKBIT ? "weak" :
		       dcell->evidence == DR_EV_VIOLATION ? "cell-violation" : "prior",
		       bar(v->bit_perr[p]));
	}
	free(ord);
}

/* ------------------------------------------------------------------ */
/*
 * How far the best reading beats the next one.
 *
 * A runner-up whose relative likelihood has underflowed to zero is not a
 * close call - it is the most decisive result the ranking can produce,
 * and reporting it as a margin of zero gets the conclusion exactly
 * backwards.
 */
static double top_margin(const dr_repair_result *r)
{
	if (r->count <= 1)
		return HUGE_VAL;            /* nothing else to weigh it against */
	if (r->list[1].rel_likelihood <= 0.0)
		return HUGE_VAL;
	return 1.0 / r->list[1].rel_likelihood;
}

/*
 * Does the repair mend the disk, or the checksum?
 *
 * A CRC has 65536 values and a damaged sector offers far more readings
 * than that, so a search will always find something that matches. Where
 * it puts its bits is the tell: Disk 2's 41/1 s17 has its damage at
 * bytes 394-410 and 513-517 and was "repaired" by one bit at byte 85,
 * which the timings give a 1-in-2500 chance of being wrong.
 *
 * Reported, not enforced, and the distinction matters. A lost reversal
 * that runs two 2T intervals into one 4T is perfectly legal MFM: the
 * timings cannot see it at all, so the commonest failure on these disks
 * is invisible here by construction. Refusing repairs outside the
 * flagged span would reject exactly those. The per-bit cost already
 * prices this properly - a bit the timings call certain is expensive to
 * overrule - and this line is here so the reader can see what the score
 * is made of.
 */
static void print_locality(const dr_repair_result *r)
{
	if (r->damage_bytes <= 0 || !r->count || !r->list[0].weight)
		return;
	printf("locality  : the flux flags %d byte(s) as damaged, and the top "
	       "reading puts %d of\n"
	       "            its %d bit(s) there%s\n",
	       r->damage_bytes, r->list[0].in_damage, r->list[0].weight,
	       r->list[0].in_damage ? "" :
	       " - so it is mending the checksum, not the disk");
}

static void print_margin(const dr_repair_result *r, const char *what)
{
	double m = top_margin(r);

	print_locality(r);
	if (r->count <= 1)
		return;
	if (m == HUGE_VAL)
		printf("margin    : the top %s is overwhelmingly more likely "
		       "than the next; clear winner\n", what);
	else
		printf("margin    : the top %s is %.3g x more likely than the "
		       "next; %s\n", what, m,
		       m >= 100.0 ? "clear winner"
		                  : "NOT a clear winner - inspect before applying");
}

/*
 * What a CRC-valid reading is actually worth.
 *
 * A 16-bit CRC lets one reading in 65536 through by chance, so the
 * question is never "does it pass" but "how many readings were even on
 * offer". A model that narrows the field to a few hundred candidates
 * makes a single survivor decisive; a search that leaves thirty bytes
 * free will always find one, and it will mean nothing.
 */
static void print_budget(dr_repair_result *r)
{
	double chance;

	if (r->explored > 0) {
		/*
		 * Allowing k bits of the stored CRC to be wrong widens the
		 * target from one value to sum(C(16,j), j<=k) of them, so
		 * the coincidence it rules out shrinks accordingly. Saying
		 * "one in 65536" after correcting two of its bits would be
		 * off by a factor of 137.
		 */
		double accept = 1.0, term = 1.0;
		int j;

		for (j = 1; j <= r->crc_fixed && j <= 16; j++) {
			term = term * (16 - j + 1) / j;
			accept += term;
		}
		chance = (double)r->explored * accept / 65536.0;
		printf("budget    : %ld reading(s) were possible; the CRC "
		       "passes ~%.3g of them\n", r->explored, chance);
		if (r->count == 1 && chance < 0.2)
			printf("            by chance, so the single survivor is "
			       "~%.1f%% likely to be right\n",
			       100.0 * (1.0 - chance));
		else if (chance > 2.0)
			printf("            by chance - too many to trust any "
			       "single survivor\n");
	}
	if (r->uncertain_bits > 16)
		printf("            %d message bit(s) are in doubt; the CRC "
		       "pins down 16\n", r->uncertain_bits);
	/*
	 * Nothing protects the CRC bytes. When the dump's own passes read
	 * them differently, the number every candidate here was matched
	 * against is a guess, and matching a guess proves nothing at all.
	 */
	if (r->crc_contested)
		printf("            the passes disagree about the stored CRC "
		       "itself, so nothing below is\n"
		       "            evidence - they match a target that may "
		       "never have been on the disk\n");
}

/*
 * The zoomed view: for each byte, the sixteen cells it occupies, the bit
 * they decode to, and the flux interval that put each reversal where it
 * is. This is the picture the whole tool is built around - everything
 * else is a search over what these numbers could have meant.
 */
static void print_bytes(dr_view *v, int b0, int b1)
{
	int b, i, k;

	if (b0 < 0) b0 = 0;
	if (b1 >= v->msg_len) b1 = v->msg_len - 1;

	printf("\nzoom: message bytes %d..%d   ('|' = flux reversal, "
	       "'.' = none)\n", b0, b1);
	printf("      each byte spans %d cells; a data bit is a reversal in "
	       "its data cell\n      with none in its clock cell\n\n",
	       v->stride);

	for (b = b0; b <= b1; b++) {
		int base = b * v->stride;
		char cells[80], bits[80];
		int first_iv = 1;

		if (base + v->stride > v->ncells)
			break;

		memset(cells, ' ', sizeof(cells));
		memset(bits, ' ', sizeof(bits));

		for (i = 0; i < v->stride; i++)
			cells[i] = v->cells[base + i].state ? '|' : '.';
		for (k = 0; k < 8; k++) {
			int cc, dc, bit;

			dr_bit_cells(v->encoding, 0, k, &cc, &dc);
			bit = (v->encoding == DR_ENC_ISO_FM)
			        ? v->cells[base + dc].state
			        : (!v->cells[base + cc].state &&
			           v->cells[base + dc].state);
			bits[dc] = (char)('0' + bit);
		}
		cells[v->stride] = 0;
		bits[v->stride] = 0;

		printf(" %4d %-5s %02X %c  %s\n", b, v->bytes[b].role,
		       v->msg[b],
		       (v->msg[b] >= 32 && v->msg[b] < 127) ? v->msg[b] : '.',
		       cells);
		printf("                   %s", bits);

		for (i = 0; i < v->stride; i++) {
			dr_cell *c = &v->cells[base + i];

			if (!c->state || c->interval_ticks < 0)
				continue;
			if (first_iv) {
				printf("   ");
				first_iv = 0;
			} else {
				printf(", ");
			}
			printf("%.2fT->%dT", c->interval_cells, c->bin);
			if (c->best_bin > 0 && c->best_bin != c->bin)
				printf(" (fits %dT, p=%.2f)",
				       c->best_bin, c->p_bin);
		}
		printf("\n");
	}
}

static void print_pattern(dr_view *v, dr_repair_result *r, int limit)
{
	int i, k;

	printf("\ndata      : %s\n", r->note);
	/*
	 * A phase correction is not a detail. It says the bytes were never
	 * wrong - the decoder lost its place - and it rewrites every byte
	 * after it, so anyone reading the diff needs to know.
	 */
	if (r->slip)
		printf("phase     : the decoder was %d cell(s) out of step "
		       "from byte %d on; every byte after that\n"
		       "            was re-framed rather than corrupted\n",
		       r->slip < 0 ? -r->slip : r->slip, r->slip_byte);
	if (r->crc_fixed)
		printf("warning   : the damage reaches the stored CRC, and "
		       "%d of its bits had to be corrected\n"
		       "            too - so it confirms this reading with "
		       "%d bits, not 16\n", r->crc_fixed, 16 - r->crc_fixed);
	printf("search    : restoring the repeat - %d byte(s) break it; "
	       "%ld reading(s) tested\n", r->outliers, r->explored);

	if (!r->count) {
		printf("result    : restoring the pattern does not satisfy "
		       "the CRC.\n");
		return;
	}

	printf("result    : %d CRC-valid reading(s) from the data model\n",
	       r->count);
	print_margin(r, "reading");

	print_budget(r);

	printf("\n rank  bytes  bits  restore  remove  data evidence  "
	       "changed bytes\n");
	printf(" ----  -----  ----  -------  ------  -------------  "
	       "----------------------------\n");

	for (i = 0; i < r->count && i < limit; i++) {
		dr_candidate *cd = &r->list[i];
		uint8_t *m = dr_candidate_message(v, cd);
		int shown = 0, lastbyte = -1, nbytes = 0;

		for (k = 0; k < cd->weight; k++)
			if ((cd->bits[k] >> 3) != lastbyte) {
				lastbyte = cd->bits[k] >> 3;
				nbytes++;
			}
		lastbyte = -1;

		printf(" %4d  %5d  %4d  %7d  %6d  %8.1f nats  ", i, nbytes,
		       cd->weight, cd->restores, cd->removes, cd->data_prior);
		for (k = 0; k < cd->weight && m; k++) {
			int b = cd->bits[k] >> 3;
			if (b == lastbyte)
				continue;
			lastbyte = b;
			if (shown == 5) { printf(", ..."); break; }
			printf("%s%d:%02X->%02X", shown ? ", " : "",
			       b, v->msg[b], m[b]);
			shown++;
		}
		printf("\n");
		free(m);
	}
}

/* Re-binning results move whole bursts, so list the bytes that changed
 * rather than every individual bit. */
static void print_rebin(dr_view *v, dr_repair_result *r, int limit)
{
	int i, k;

	printf("\nsearch    : re-binning the flux - %d interval(s) left open by "
	       "the timings,\n"
	       "            %d that the timings re-read on their own; "
	       "%ld assignment(s) tested\n",
	       r->ambiguous, r->pinned_moves, r->explored);
	if (r->note[0])
		printf("            %s\n", r->note);

	if (!r->count) {
		printf("result    : no legal re-binning satisfies the CRC.\n");
		if (r->truncated)
			printf("            the search hit its cap - try a larger "
			       "--max-explore or --bin-budget.\n");
		return;
	}

	printf("result    : %d CRC-valid re-reading(s)%s\n", r->count,
	       r->truncated ? " (search capped)" : "");
	printf("            likeliest reading costs %.1f nats and %s the CRC\n",
	       r->floor_cost, r->floor_valid ? "satisfies" : "does NOT satisfy");
	if (r->count)
		printf("            best CRC-valid reading costs %.1f nats "
		       "(%.1f more than the likeliest)\n",
		       r->list[0].flux_cost,
		       r->list[0].flux_cost - r->floor_cost);
	print_margin(r, "re-reading");

	print_budget(r);
	if (r->current_cost > 0.0)
	{
		/*
		 * Say which way round it went. The best re-reading can cost
		 * *more* than the decoder's - that happens when a stretch has
		 * no legal binning that fits the timings at all - and
		 * announcing an improvement in that case contradicts the two
		 * numbers printed next to it.
		 */
		if (r->floor_cost < r->current_cost)
			printf("            re-binning explains the timings "
			       "better than the decoder did:\n"
			       "            %.0f nats -> %.0f nats over the "
			       "disturbed intervals\n",
			       r->current_cost, r->floor_cost);
		else
			printf("            no legal re-binning explains these "
			       "timings even as well as the\n"
			       "            decoder's own (%.0f nats -> %.0f); "
			       "the flux here fits no reading\n",
			       r->current_cost, r->floor_cost);
	}

	printf("\n rank  bits  re-bins  rel.likelihood  changed bytes\n");
	printf(" ----  ----  -------  --------------  "
	       "------------------------------------\n");

	for (i = 0; i < r->count && i < limit; i++) {
		dr_candidate *cd = &r->list[i];
		uint8_t *m = dr_candidate_message(v, cd);
		int shown = 0, lastbyte = -1;

		printf(" %4d  %4d  %7d  %14.6g  ", i, cd->weight, cd->rebins,
		       cd->rel_likelihood);
		for (k = 0; k < cd->weight && m; k++) {
			int b = cd->bits[k] >> 3;
			if (b == lastbyte)
				continue;
			lastbyte = b;
			if (shown == 6) {
				printf(", ...");
				break;
			}
			printf("%s%d:%02X->%02X", shown ? ", " : "",
			       b, v->msg[b], m[b]);
			shown++;
		}
		printf("\n");
		free(m);
	}
}

static void print_candidates(dr_view *v, dr_repair_result *r, int limit)
{
	int i, k;

	printf("\nsearch    : %d low-confidence bit(s) in the pool, "
	       "explored up to weight %d\n", r->npool, r->searched_weight);

	if (!r->count) {
		printf("result    : no CRC-valid correction found within the "
		       "search limits.\n"
		       "            try --max-weight 4 or a larger --max-pool / "
		       "higher --threshold.\n");
		return;
	}

	if (r->crc_contested)
		printf("warning   : the dump's passes disagree about the "
		       "stored CRC itself, so nothing\n"
		       "            below is evidence - every candidate "
		       "matches a target that may\n"
		       "            never have been on the disk\n");

	printf("result    : %d CRC-valid candidate(s) at weight %d%s\n",
	       r->count, r->list[0].weight,
	       r->truncated ? " (list truncated)" : "");

	/* A 16-bit CRC only pins the data down to 1 in 65536, and a field
	 * this long offers thousands of places to flip, so alternative
	 * readings are normal. Say how much the top one actually wins by. */
	print_margin(r, "candidate");
	if (!v->flux_available)
		printf("note      : no flux timing in this image, so every bit "
		       "carries the same prior.\n"
		       "            Candidates of equal weight cannot be told "
		       "apart; the lowest weight\n"
		       "            is not necessarily the true error. Re-dump "
		       "as flux (SCP/KryoFlux/A2R)\n"
		       "            to get a real ranking.\n");
	printf("\n rank  w  rel.likelihood  flips\n");
	printf(" ----  -  --------------  ---------------------------------\n");

	for (i = 0; i < r->count && i < limit; i++) {
		dr_candidate *cd = &r->list[i];
		printf(" %4d  %d  %14.6g  ", i, cd->weight, cd->rel_likelihood);
		for (k = 0; k < cd->weight; k++) {
			int p = cd->bits[k];
			printf("%sbyte %d bit %d: %d->%d (p=%.3g)",
			       k ? ", " : "", p >> 3, p & 7,
			       cd->before[k], !cd->before[k], v->bit_perr[p]);
		}
		printf("\n");
	}
}


/* ------------------------------------------------------------------ */
/* The filesystem above the sector                                     */
/* ------------------------------------------------------------------ */
/* Opened once and kept: assembling it re-reads every track. */
static dr_fs *g_fs;
static int    g_fs_tried;

static dr_fs *fs_of(dr_ctx *c)
{
	if (!g_fs_tried) {
		g_fs_tried = 1;
		g_fs = dr_fs_open(c);
	}
	return g_fs;
}

static const char *area_name(dr_fs_area a)
{
	switch (a) {
	case DR_AREA_BOOT:    return "boot";
	case DR_AREA_FAT:     return "FAT";
	case DR_AREA_ROOT:    return "root dir";
	case DR_AREA_FILE:    return "file";
	case DR_AREA_FREE:    return "free";
	case DR_AREA_OUTSIDE: return "outside";
	default:              return "?";
	}
}

/* What this sector is, in the terms the disk's owner would use. */
static void print_fs(dr_ctx *c, int idx)
{
	dr_fs *fs = fs_of(c);
	const dr_fs_info *in;
	dr_fs_loc loc;

	if (!fs)
		return;
	in = dr_fs_stat(fs);
	if (dr_fs_locate(fs, c, idx, &loc) != 0)
		return;

	printf("filesystem: %s, %d sectors/track, %d head(s), %d file(s) in the "
	       "root - this is LBA %ld\n", in->kind, in->spt, in->heads,
	       in->nfiles, loc.lba);
	printf("            %s\n", loc.note);
	{
		char detail[256];

		if (dr_fs_detail(fs, &loc, detail, (int)sizeof(detail)) == 0)
			printf("            %s\n", detail);
	}
}

/* Does the other copy of this FAT sector answer to this sector's own
 * stored CRC? If it does, the bytes are not a guess: two independent
 * things agree, and one of them is sixteen bits the search never got
 * to choose. */
static const uint8_t *mirror_for(dr_ctx *c, dr_view *v, int idx,
                                 dr_fs_loc *loc, int *proven, int *ndiff)
{
	dr_fs *fs = fs_of(c);
	const uint8_t *m;
	uint8_t *msg;
	uint16_t crc;
	int i;

	*proven = 0;
	*ndiff = 0;
	if (!fs || dr_fs_locate(fs, c, idx, loc) != 0)
		return NULL;
	m = dr_fs_mirror(fs, loc);
	if (!m || v->data_len != 512)
		return NULL;

	for (i = 0; i < v->data_len; i++)
		if (m[i] != v->msg[v->data_offset + i])
			(*ndiff)++;

	msg = malloc((size_t)v->msg_len);
	if (!msg)
		return m;
	memcpy(msg, v->msg, (size_t)v->msg_len);
	memcpy(msg + v->data_offset, m, (size_t)v->data_len);
	crc = dr_crc16(msg, v->msg_len - 2);
	free(msg);
	*proven = (crc == v->stored_crc);
	return m;
}

static void print_mirror(dr_ctx *c, dr_view *v, int idx)
{
	dr_fs_loc loc;
	const uint8_t *m;
	int proven = 0, ndiff = 0;

	m = mirror_for(c, v, idx, &loc, &proven, &ndiff);
	if (!m)
		return;
	printf("mirror    : the other FAT holds the same %d bytes and reads "
	       "clean; it differs\n            from this reading in %d byte(s)"
	       ".\n", v->data_len, ndiff);
	if (proven)
		printf("            Its CRC-16 is %04X - the value stored "
		       "here. That is not a ranking,\n"
		       "            it is a match: these are the sector's "
		       "bytes. Write them with\n"
		       "            --from-mirror.\n", v->stored_crc);
	else
		printf("            Its CRC-16 does not match the value "
		       "stored here, so either the\n"
		       "            two copies genuinely differ or the stored "
		       "CRC went with the data.\n"
		       "            --from-mirror writes it anyway, re-stamping "
		       "the CRC.\n");
}

/* Put every candidate reading to the check that lives above the sector.
 *
 * The sector CRC is sixteen bits, and a search that explores far enough
 * will find hundreds of readings that satisfy it - on one sector here,
 * 243 of them, the best only nine times likelier than the next. Sixteen
 * bits cannot separate those. The file they sit inside can: a deflate
 * stream carries a CRC-32 of what it unpacks to, and a reading that is
 * wrong by one bit does not unpack at all. So ask it about all of them,
 * and report what survives - including, usefully, "none of them did". */
static void print_referee(dr_ctx *c, dr_view *v, dr_repair_result *r,
                          int idx, int limit)
{
	dr_fs *fs = fs_of(c);
	dr_fs_loc loc;
	int i, tested = 0, kept = 0, shown = 0;
	char note[256], bestnote[256];
	double bestscore = -1.0;

	note[0] = 0;
	bestnote[0] = 0;
	if (!fs || dr_fs_locate(fs, c, idx, &loc) != 0)
		return;
	if (loc.area == DR_AREA_FREE || loc.area == DR_AREA_OUTSIDE) {
		dr_fs_verdict vd;
		dr_fs_score(fs, &loc, NULL, 0, &vd);
		printf("\nreferee   : %s\n", vd.how);
		return;
	}
	if (!r->count)
		return;

	printf("\nreferee   : what the file above this sector makes of each "
	       "reading\n");
	for (i = 0; i < r->count; i++) {
		uint8_t *msg = dr_candidate_message(v, &r->list[i]);
		dr_fs_verdict vd;

		if (!msg)
			continue;
		if (dr_fs_score(fs, &loc, msg + v->data_offset,
		                v->data_len, &vd) == 0 && vd.checked) {
			tested++;
			if (vd.proven || !vd.refuted) {
				kept++;
				if (shown < limit) {
					printf("  %2d  %-8s %s\n", i,
					       vd.proven ? "SURVIVES" : "-",
					       vd.how);
					shown++;
				}
			}
			if (!note[0])
				snprintf(note, sizeof(note), "%s", vd.how);
			if (vd.score > bestscore) {
				bestscore = vd.score;
				snprintf(bestnote, sizeof(bestnote), "%s",
				         vd.how);
			}
		} else if (!tested) {
			printf("            %s\n", vd.how);
			free(msg);
			return;
		}
		free(msg);
	}
	if (!tested)
		return;
	if (!kept) {
		printf("            %d reading(s) satisfied the sector's "
		       "16-bit CRC; not one of them\n"
		       "            survives the check the file itself "
		       "carries. The true reading is\n"
		       "            not in this pool - the damage is deeper "
		       "than the search can reach.\n", tested);
		if (bestnote[0] && bestscore > 0.0)
			printf("            The closest any of them came: "
			       "%s\n", bestnote);
		else if (note[0])
			printf("            %s\n", note);
	} else if (kept == tested)
		printf("            all %d of them do; this check cannot "
		       "separate them.\n", tested);
	else
		printf("            %d of %d CRC-valid reading(s) also satisfy "
		       "the file's own checksum.\n", kept, tested);
}

/* Write out one image per plausible reading, so they can be opened and
 * looked at. A CRC-valid reading is not the same thing as a correct
 * one; five files a person can browse settle in seconds what a margin
 * only ever estimates. */
static int write_variants(args *a, int idx, dr_view *v,
                          dr_repair_result *r, int n)
{
	const char *dot;
	char stem[1024], ext[64];
	int i, wrote = 0;

	if (!a->out) {
		fprintf(stderr, "--variants needs --out\n");
		return -1;
	}
	dot = strrchr(a->out, '.');
	if (dot && strlen(dot) < sizeof(ext)) {
		snprintf(stem, sizeof(stem), "%.*s", (int)(dot - a->out), a->out);
		snprintf(ext, sizeof(ext), "%s", dot);
	} else {
		snprintf(stem, sizeof(stem), "%s", a->out);
		snprintf(ext, sizeof(ext), ".hfe");
	}

	if (n > r->count)
		n = r->count;
	printf("\nvariants  : one image per reading, ranked as the search "
	       "ranks them\n");

	for (i = 0; i < n; i++) {
		uint8_t *msg = dr_candidate_message(v, &r->list[i]);
		char path[1200];
		dr_ctx *cc;
		dr_view *vv;
		dr_fs *vfs;
		dr_fs_loc loc;
		dr_fs_verdict vd;
		int scored = 0;

		if (!msg)
			continue;
		snprintf(path, sizeof(path), "%s_a%d%s", stem, i + 1, ext);

		cc = dr_open_ex(a->image, 0, a->sets, a->nsets);
		if (!cc || dr_scan(cc) < 0) {
			free(msg);
			if (cc)
				dr_close(cc);
			continue;
		}
		vv = dr_view_open(cc, idx, &a->opt);
		if (vv && dr_set_data(cc, vv, msg + v->data_offset,
		                      v->data_len) == 0 &&
		    dr_export(cc, path, a->format) == 0) {
			wrote++;
			vfs = dr_fs_open(cc);
			if (vfs && dr_fs_locate(vfs, cc, idx, &loc) == 0 &&
			    dr_fs_score(vfs, &loc, msg + v->data_offset,
			                v->data_len, &vd) == 0 && vd.checked)
				scored = 1;
			printf("  %s   %s\n", path,
			       scored ? (vd.proven ? "PROVEN by the file's own "
			                             "checksum" : vd.how)
			              : "written");
			if (vfs)
				dr_fs_free(vfs);
		} else {
			printf("  %s   could not be written\n", path);
		}
		if (vv)
			dr_view_free(vv);
		dr_close(cc);
		free(msg);
	}
	if (wrote)
		printf("            Open them in a disk browser: the one whose "
		       "files still make sense\n"
		       "            is the true reading.\n");
	return wrote;
}


/* How much of the sector is actually settled.
 *
 * "256 CRC-valid readings, the top one twice as likely as the next" is
 * true and almost useless. Those readings are not 256 different
 * sectors: each differs from the decoder's own in two or three bits,
 * and where they agree they agree completely. Weighting them by
 * likelihood and asking, byte by byte, how much of the mass sits on one
 * value turns an unusable margin into the statement a person wants -
 * this sector is known except for these twenty-four bytes.
 *
 * It is not a repair and it is not applied. It is a measure of what is
 * left in doubt, and when the stored CRC is itself damaged it measures
 * nothing at all, because then every reading in the set is agreeing
 * about a checksum that was never on the disk. */
static void print_consensus(dr_view *v, dr_repair_result *r)
{
	double *mass;
	int i, k, open = 0, len = v->data_len;
	int first[8], nfirst = 0;

	if (r->count < 2 || len <= 0 || len > 4096)
		return;
	mass = calloc((size_t)len * 256, sizeof(double));
	if (!mass)
		return;

	for (i = 0; i < r->count; i++) {
		uint8_t *m = dr_candidate_message(v, &r->list[i]);
		double w = r->list[i].rel_likelihood;

		if (!m)
			continue;
		if (w <= 0.0)
			w = 1e-300;
		for (k = 0; k < len; k++)
			mass[(size_t)k * 256 + m[v->data_offset + k]] += w;
		free(m);
	}

	for (k = 0; k < len; k++) {
		double tot = 0.0, best = 0.0;
		int b;

		for (b = 0; b < 256; b++) {
			double x = mass[(size_t)k * 256 + b];
			tot += x;
			if (x > best)
				best = x;
		}
		if (tot > 0.0 && best / tot < 0.99) {
			open++;
			if (nfirst < (int)(sizeof(first)/sizeof(first[0])))
				first[nfirst++] = k;
		}
	}
	free(mass);

	printf("consensus : weighted by likelihood, %d of %d data byte(s) are "
	       "settled to 99%%\n", len - open, len);
	if (open) {
		printf("            across the %d reading(s); %d remain open, "
		       "at byte", r->count, open);
		for (i = 0; i < nfirst; i++)
			printf("%s %d", i ? "," : "", first[i]);
		printf("%s\n", open > nfirst ? ", ..." : "");
	} else {
		printf("            across all %d reading(s) - they differ "
		       "only in which bits they flip,\n            not in "
		       "what the sector says\n", r->count);
	}
	if (v->crc_suspect)
		printf("            ...but every one of them matches a stored "
		       "CRC that is itself in\n            doubt, so this is "
		       "agreement about a checksum, not about the disk\n");
}

/* Run the engine cascade for one sector and report in one line.
 * Returns 1 if the sector was repaired and verified. */
/*
 * Does this candidate payload reproduce the sector's own stored CRC?
 *
 * A second copy found by inference - another build of the same program,
 * say - is evidence and not proof. The sector's sixteen bits, where the
 * damage spared them, turn it into proof: 512 bytes that happen to
 * carry the right CRC by chance is a one-in-65536 accident, and these
 * are not 512 arbitrary bytes but a specific file's.
 */
static int payload_matches_crc(dr_view *v, const uint8_t *data)
{
	uint8_t *msg;
	uint16_t k;

	if (!v || !v->msg || v->msg_len < 2 || v->crc_suspect)
		return 0;
	msg = malloc((size_t)v->msg_len);
	if (!msg)
		return 0;
	memcpy(msg, v->msg, (size_t)v->msg_len);
	memcpy(msg + v->data_offset, data, (size_t)v->data_len);
	k = dr_crc16(msg, v->msg_len - 2);
	free(msg);
	return k == v->stored_crc;
}

/* Does the file above this sector say the reading cannot be right? */
static int refuted_by_file(dr_ctx *c, dr_view *v, const dr_candidate *cand,
                           int idx)
{
	dr_fs *fs = fs_of(c);
	dr_fs_loc loc;
	dr_fs_verdict vd;
	uint8_t *msg;
	int no = 0;

	if (!fs || dr_fs_locate(fs, c, idx, &loc) != 0)
		return 0;
	if (loc.area != DR_AREA_FILE)
		return 0;
	msg = dr_candidate_message(v, cand);
	if (!msg)
		return 0;
	if (dr_fs_score(fs, &loc, msg + v->data_offset, v->data_len,
	                &vd) == 0 && vd.checked && vd.refuted)
		no = 1;
	free(msg);
	return no;
}

/*
 * Put the referee in front of the ranking.
 *
 * The likelihood ranking knows about flux and priors; it knows nothing
 * about what the bytes are *for*. Where the sector sits under a file
 * whose format carries a check of its own - an archive member's CRC-32,
 * a compound document's twin stream, a record framing - that check is
 * worth more than the ordering, because it is evidence rather than
 * preference. A reading it proves belongs first and a reading it
 * refutes belongs last, however the priors felt about them.
 *
 * Ties keep their original order, so where the referee has no opinion
 * (or no referee exists) this changes nothing at all.
 */
static void order_by_referee(dr_ctx *c, dr_view *v, dr_repair_result *r,
                             int idx)
{
	dr_fs *fs = fs_of(c);
	dr_fs_loc loc;
	dr_candidate *sorted;
	double *key;
	int i, j, n = r->count, any = 0;

	if (!fs || n < 2 || dr_fs_locate(fs, c, idx, &loc) != 0)
		return;
	if (loc.area == DR_AREA_FREE || loc.area == DR_AREA_OUTSIDE)
		return;
	key = malloc((size_t)n * sizeof(*key));
	sorted = malloc((size_t)n * sizeof(*sorted));
	if (!key || !sorted) {
		free(key);
		free(sorted);
		return;
	}
	for (i = 0; i < n; i++) {
		uint8_t *msg = dr_candidate_message(v, &r->list[i]);
		dr_fs_verdict vd;

		key[i] = 1.0;
		if (!msg)
			continue;
		if (dr_fs_score(fs, &loc, msg + v->data_offset, v->data_len,
		                &vd) == 0 && vd.checked) {
			key[i] = vd.proven ? 3.0 : vd.refuted ? 0.0
			                                      : 1.0 + vd.score;
			any = 1;
		}
		free(msg);
	}
	if (any) {
		int m = 0;

		/* Selection sort, descending, taking every reading that
		 * shares the best remaining key in its original order - so
		 * it is stable, which is the whole point: the likelihood
		 * ranking still decides everything the referee cannot. A
		 * list of at most a few hundred does not need better. */
		while (m < n) {
			double best = -1.0;

			for (i = 0; i < n; i++)
				if (key[i] > best)
					best = key[i];
			if (best < 0.0)
				break;
			for (i = 0; i < n; i++)
				if (key[i] == best) {
					sorted[m++] = r->list[i];
					key[i] = -1.0;
				}
		}
		for (j = 0; j < m; j++)
			r->list[j] = sorted[j];
	}
	free(key);
	free(sorted);
}

/*
 * A sector that came out of `--all` still unrepaired, together with the
 * readings the search liked best. Whole-disk variants are built from
 * these: variant k takes every such sector's k-th reading at once, so
 * each image is one coherent guess at the disk rather than a mixture.
 */
typedef struct {
	int      track, side, id;
	int      len;
	int      nguess;
	uint8_t *guess[DR_VARIANTS_MAX];
} openspot;

static int repair_one(dr_ctx *c, int idx, args *a, int apply,
                      openspot *spot)
{
	dr_view *v;
	dr_repair_result r;
	dr_mode engine;
	const dr_sector *sl;
	int n, rc = 0;
	double margin = 0.0;

	sl = dr_sectors(c, &n);
	v = dr_view_open(c, idx, &a->opt);
	if (!v) {
		printf(" %3d/%d s%-3d  %-8s  %s\n", sl[idx].track, sl[idx].side,
		       sl[idx].sector_id, "-", "no view (unsupported encoding)");
		return 0;
	}

	if (a->opt.mode == DR_MODE_AUTO) {
		dr_repair_auto(c, v, &a->opt, &r);
		engine = r.count ? r.list[0].origin : DR_MODE_BITS;
	} else {
		engine = a->opt.mode;
		memset(&r, 0, sizeof(r));
		if (engine == DR_MODE_PATTERN)
			dr_pattern_search(v, &a->opt, &r);
		else if (a->opt.mode == DR_MODE_REVS)
			dr_revs_search(v, &a->opt, &r);
		else if (engine == DR_MODE_REBIN)
			dr_rebin_search(v, &a->opt, &r);
		else
			dr_repair_search(v, &a->opt, &r);
		dr_rescore_data(c, v, &a->opt, &r);
	}

	margin = top_margin(&r);

	/* Before any of that is weighed: is there a second copy of these
	 * bytes on this disk that something independent confirms? A proof
	 * outranks every ranking, so it is taken first and the search's
	 * answer is not consulted at all. */
	if (apply) {
		dr_fs *fs = fs_of(c);
		dr_fs_loc loc;
		const uint8_t *m = NULL;
		uint8_t sis[512];
		char how[256];
		int proven = 0, ndiff = 0;

		how[0] = 0;
		m = mirror_for(c, v, idx, &loc, &proven, &ndiff);
		/*
		 * The other FAT's bytes are taken when this sector's own
		 * checksum confirms them - or when that checksum is itself
		 * inside the damage, because then it confirms nothing and
		 * the second copy is the only evidence there is. A FAT that
		 * disagrees with its twin is a broken filesystem either
		 * way; a FAT restored from a clean twin is at worst as old
		 * as the twin.
		 */
		if (!proven && !(m && v->crc_suspect))
			m = NULL;
		else if (!proven && m)
			proven = 3;
		if (!m && fs && dr_fs_locate(fs, c, idx, &loc) == 0 &&
		    v->data_len == (int)sizeof(sis)) {
			int sr = dr_fs_sister(fs, &loc, sis, v->data_len, how,
			                      (int)sizeof(how));

			/*
			 * 1 is a copy that proved itself - an archive
			 * member's own CRC-32, a compound document's twin
			 * stream. 2 is a copy found by inference, another
			 * build of the same program on the same disk, and
			 * that one has to satisfy the sector's own CRC
			 * before it is written.
			 */
			if (sr == 1 || (sr == 2 &&
			                payload_matches_crc(v, sis))) {
				m = sis;
				proven = 2;
			}
		}
		if (m && dr_set_data(c, v, m, v->data_len) == 0) {
			char *semi = strchr(how, ';');

			if (semi)
				*semi = 0;
			printf(" %3d/%d s%-3d  %-8s  ",
			       sl[idx].track, sl[idx].side, sl[idx].sector_id,
			       "2nd copy");
			printf("%s  -> APPLIED, %s\n",
			       proven == 2 ? how
			                   : "the other copy of this FAT",
			       proven == 3
			         ? "the stored CRC here is itself damaged"
			         : "proven not ranked");
			dr_repair_free(&r);
			dr_view_free(v);
			return 1;
		}
	}

	printf(" %3d/%d s%-3d  %-8s  ",
	       sl[idx].track, sl[idx].side, sl[idx].sector_id,
	       engine == DR_MODE_PATTERN ? "pattern" :
	       engine == DR_MODE_REVS ? "passes" :
	       engine == DR_MODE_REBIN ? "re-bin" : "bits");

	if (!r.count) {
		printf("no CRC-valid reading found");
		if (r.uncertain_bits > 16)
			printf(" (%d bits in doubt vs 16 of CRC)",
			       r.uncertain_bits);
		printf("\n");
	} else {
		int unique = (r.count == 1);

		printf("%d reading(s), ", r.count);
		if (unique)
			printf("unique");
		else if (margin == HUGE_VAL)
			printf("top overwhelms the next");
		else
			printf("top %.3g x next", margin);

		/*
		 * A margin is a ratio against the other readings that matched
		 * the stored CRC. When the damage reaches the CRC bytes
		 * themselves, the number every one of them matched is a guess
		 * too, and the ratio says nothing about the truth - it only
		 * says which fiction the priors preferred. So this refuses to
		 * apply on its own, however commanding the margin looks, and
		 * leaves it to be applied deliberately.
		 */
		if (apply && v->crc_suspect && !(r.slip || r.crc_fixed)) {
			printf("  -> not applied (~%.1f of the stored CRC's "
			       "16 bits are themselves in doubt)",
			       v->crc_expected_errors);
		} else if (apply && (unique || margin >= 100.0) &&
		           refuted_by_file(c, v, &r.list[0], idx)) {
			/*
			 * A commanding margin among readings that all match
			 * a 16-bit CRC still loses to a check the file
			 * itself carries. If the format above this sector
			 * says this reading cannot be right, it is not
			 * right, however far ahead of the rest it came.
			 */
			printf("  -> not applied (the file above it refutes "
			       "this reading)");
		} else if (apply && (unique || margin >= 100.0)) {
			if (dr_apply(c, v, &r.list[0]) == 0 &&
			    dr_verify(c, idx) == 1) {
				printf("  -> APPLIED, verifies clean");
				rc = 1;
			} else {
				printf("  -> apply failed");
			}
		} else if (apply) {
			printf("  -> not applied (ambiguous)");
		}
		printf("\n");
	}

	/* Nothing was written into this sector, so remember what the
	 * search would have written, for the variants. */
	if (!rc && spot && a->variants > 0 && r.count > 0 &&
	    v->data_len > 0) {
		int k;

		order_by_referee(c, v, &r, idx);

		spot->track = sl[idx].track;
		spot->side = sl[idx].side;
		spot->id = sl[idx].sector_id;
		spot->len = v->data_len;
		for (k = 0; k < r.count && k < a->variants; k++) {
			uint8_t *msg = dr_candidate_message(v, &r.list[k]);

			if (!msg)
				break;
			spot->guess[k] = malloc((size_t)v->data_len);
			if (!spot->guess[k]) {
				free(msg);
				break;
			}
			memcpy(spot->guess[k], msg + v->data_offset,
			       (size_t)v->data_len);
			free(msg);
			spot->nguess++;
		}
	}

	dr_repair_free(&r);
	dr_view_free(v);
	return rc;
}

/*
 * Put the counts in the name.
 *
 * A directory of repaired images is a pile of disks that all look
 * alike, and the one thing a person wants to know before opening one -
 * how much of it came back - is exactly what the filename does not say.
 * So `--out` may carry {fixed} and {bad}, e.g.
 *
 *     --out "Disk_Sand_{fixed}of{bad}.hfe"   ->  Disk_Sand_3of4.hfe
 *
 * Explicit rather than automatic: a name is the user's to choose, and a
 * tool that silently renames its output is a tool you cannot script.
 */
static void name_with_counts(char *dst, size_t dstsz, const char *pat,
                             int fixed, int bad)
{
	size_t o = 0;
	const char *p;

	for (p = pat; *p && o + 32 < dstsz; p++) {
		if (!strncmp(p, "{fixed}", 7)) {
			o += (size_t)snprintf(dst + o, dstsz - o, "%d", fixed);
			p += 6;
		} else if (!strncmp(p, "{bad}", 5)) {
			o += (size_t)snprintf(dst + o, dstsz - o, "%d", bad);
			p += 4;
		} else {
			dst[o++] = *p;
		}
	}
	dst[o] = 0;
}

/*
 * One image per guess at the whole disk.
 *
 * When some sectors will not settle, there is no single answer to
 * write - but there is a short list of coherent ones. Variant k takes
 * the k-th reading of *every* unsettled sector at once, so each image
 * is one self-consistent account of the disk rather than a mixture of
 * ranks. Open them in a disk browser and the one whose files still make
 * sense is the answer; that judgement is a person's to make, and this
 * is what makes it possible to make it.
 *
 * Each variant is built from the base image (the one carrying the
 * repairs that were applied), not from the one before it, so a variant
 * never inherits another variant's guesses.
 */
static int write_disk_variants(args *a, const char *base,
                               openspot *spots, int nspots, int n)
{
	const char *dot;
	char stem[1200], ext[64];
	int k, wrote = 0;

	dot = strrchr(base, '.');
	if (dot && strlen(dot) < sizeof(ext)) {
		snprintf(stem, sizeof(stem), "%.*s", (int)(dot - base), base);
		snprintf(ext, sizeof(ext), "%s", dot);
	} else {
		snprintf(stem, sizeof(stem), "%s", base);
		snprintf(ext, sizeof(ext), ".hfe");
	}

	for (k = 0; k < n; k++) {
		dr_ctx *cc = dr_open_ex(base, 0, a->sets, a->nsets);
		char path[1400];
		int set = 0, i, m, deep = 0;

		if (!cc || dr_scan(cc) < 0) {
			if (cc)
				dr_close(cc);
			break;
		}
		for (i = 0; i < nspots; i++) {
			const dr_sector *sl;
			int use, idx = -1, j;

			if (!spots[i].nguess)
				continue;
			use = k < spots[i].nguess ? k : spots[i].nguess - 1;
			if (k < spots[i].nguess)
				deep++;
			sl = dr_sectors(cc, &m);
			for (j = 0; j < m; j++)
				if (sl[j].track == spots[i].track &&
				    sl[j].side == spots[i].side &&
				    sl[j].sector_id == spots[i].id) {
					idx = j;
					break;
				}
			if (idx < 0)
				continue;
			{
				dr_view *vv = dr_view_open(cc, idx, &a->opt);

				if (vv && dr_set_data(cc, vv,
				                      spots[i].guess[use],
				                      spots[i].len) == 0)
					set++;
				if (vv)
					dr_view_free(vv);
			}
		}
		/* Past the point where any sector still has a reading of
		 * its own to offer, the images stop differing. */
		if (k > 0 && !deep) {
			dr_close(cc);
			break;
		}
		snprintf(path, sizeof(path), "%s_a%d%s", stem, k + 1, ext);
		if (dr_export(cc, path, a->format) == 0) {
			printf("  %s  (%d sector(s) set to reading #%d)\n",
			       path, set, k + 1);
			wrote++;
		}
		dr_close(cc);
	}
	return wrote;
}

static void free_spots(openspot *spots, int n)
{
	int i, k;

	for (i = 0; spots && i < n; i++)
		for (k = 0; k < spots[i].nguess; k++)
			free(spots[i].guess[k]);
	free(spots);
}

static int cmd_repair_all(dr_ctx *c, args *a)
{
	struct { int track, side, id; } *todo = NULL;
	openspot *spots = NULL;
	int n, i, ntodo = 0, fixed = 0, apply, nspots = 0;
	const dr_sector *sl = dr_sectors(c, &n);

	apply = (a->apply >= 0 || a->autoapply);

	for (i = 0; i < n; i++)
		if (sl[i].header_crc == DR_CRC_BAD ||
		    sl[i].data_crc == DR_CRC_BAD) {
			todo = realloc(todo, (size_t)(ntodo + 1) * sizeof(*todo));
			if (!todo)
				return 1;
			todo[ntodo].track = sl[i].track;
			todo[ntodo].side = sl[i].side;
			todo[ntodo].id = sl[i].sector_id;
			ntodo++;
		}

	if (!ntodo) {
		printf("no CRC errors on this disk.\n");
		free(todo);
		return 0;
	}

	printf("%d sector(s) with a CRC error%s\n\n", ntodo,
	       apply ? "; applying unambiguous repairs" : "");
	printf(" trk/s sect  engine    result\n");
	printf(" ----- ----  --------  "
	       "---------------------------------------------------\n");

	for (i = 0; i < ntodo; i++) {
		int idx = -1, j;

		/* Applying re-scans, so locate the sector by address. */
		sl = dr_sectors(c, &n);
		for (j = 0; j < n; j++)
			if (sl[j].track == todo[i].track &&
			    sl[j].side == todo[i].side &&
			    sl[j].sector_id == todo[i].id) {
				idx = j;
				break;
			}
		if (idx < 0)
			continue;

		if (a->variants > 0 && !spots) {
			spots = calloc((size_t)ntodo, sizeof(*spots));
			if (!spots) {
				free(todo);
				return 1;
			}
		}
		fixed += repair_one(c, idx, a, apply,
		                    spots ? &spots[nspots] : NULL);
		if (spots && spots[nspots].nguess)
			nspots++;
		fflush(stdout);
	}

	printf("\n%d of %d repaired and verified\n", fixed, ntodo);
	free(todo);
	if (g_fs) {
		dr_fs_free(g_fs);
		g_fs = NULL;
		g_fs_tried = 0;
	}

	if (a->out && (fixed || nspots)) {
		char path[1200];

		name_with_counts(path, sizeof(path), a->out, fixed, ntodo);
		if (dr_export(c, path, a->format) < 0) {
			fprintf(stderr, "%s\n", dr_last_error(c));
			free_spots(spots, nspots);
			return 1;
		}
		printf("wrote %s (%s)\n", path, a->format);

		if (nspots) {
			printf("\n%d sector(s) did not settle. One image per "
			       "guess at the whole disk,\nlikeliest first - "
			       "open them in a disk browser and see which "
			       "one's\nfiles still make sense:\n\n", nspots);
			write_disk_variants(a, path, spots, nspots,
			                    a->variants);
		}
	} else if (nspots && a->variants > 0) {
		fprintf(stderr, "--variants needs --out\n");
	}
	free_spots(spots, nspots);
	return 0;
}

/* ------------------------------------------------------------------ */
static int cmd_formats(void)
{
	HXCFE *h = hxcfe_init();
	HXCFE_IMGLDR *l;
	int i, n;

	if (!h)
		return 1;
	l = hxcfe_imgInitLoader(h);
	if (!l) {
		hxcfe_deinit(h);
		return 1;
	}

	n = hxcfe_imgGetNumberOfLoader(l);
	printf("%-24s %-10s %s\n", "NAME", "EXT", "DESCRIPTION");
	for (i = 0; i < n; i++) {
		if (!(hxcfe_imgGetLoaderAccess(l, i) & 2))
			continue;
		printf("%-24s %-10s %s\n",
		       hxcfe_imgGetLoaderName(l, i),
		       hxcfe_imgGetLoaderExt(l, i),
		       hxcfe_imgGetLoaderDesc(l, i));
	}
	hxcfe_imgDeInitLoader(l);
	hxcfe_deinit(h);
	return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	args a;
	dr_ctx *c;
	int rc = 0, idx;

	if (parse_args(argc, argv, &a) < 0) {
		fputs(usage_text, stderr);
		return 2;
	}

	if (!strcmp(a.cmd, "formats"))
		return cmd_formats();

	if (!strcmp(a.cmd, "help") || !strcmp(a.cmd, "--help")) {
		fputs(usage_text, stdout);
		return 0;
	}

	if (!a.image) {
		fputs(usage_text, stderr);
		return 2;
	}

	c = dr_open_ex(a.image, a.verbose, a.sets, a.nsets);
	if (!c)
		return 1;
	if (!c->floppy) {
		fprintf(stderr, "error: %s\n", dr_last_error(c));
		dr_close(c);
		return 1;
	}

	if (dr_scan(c) < 0) {
		fprintf(stderr, "error: %s\n", dr_last_error(c));
		dr_close(c);
		return 1;
	}

	if (!strcmp(a.cmd, "scan")) {
		if (a.json)
			dr_json_scan(c, stdout), printf("\n");
		else
			print_scan(c);
		if (a.fs && !a.json) {
			dr_fs *fs = fs_of(c);
			const dr_sector *sl;
			int n, i, shown = 0;
			int n_out = 0, n_free = 0, n_fat = 0;
			int n_mirror = 0, n_file = 0, n_other = 0;

			if (!fs) {
				printf("\nno filesystem recognised on this "
				       "disk.\n");
			} else {
				const dr_fs_info *in = dr_fs_stat(fs);

				if (in->nfats) {
					printf("\nfilesystem: %s, %d "
					       "sector(s)/track, %d head(s), "
					       "%d FAT(s) of %d sector(s), "
					       "%d file(s)\n", in->kind,
					       in->spt, in->heads, in->nfats,
					       in->fat_sectors, in->nfiles);
					printf("            the two FATs differ "
					       "in %ld byte(s)%s\n",
					       in->fat_mismatch,
					       in->fat_mismatch ? "" :
					       " - they agree exactly");
				} else {
					printf("\nfilesystem: %s (Macintosh), "
					       "volume '%s', %d file(s) and "
					       "folder(s),\n            %d "
					       "allocation block(s) of %d "
					       "byte(s)\n",
					       in->kind, in->oem, in->nfiles,
					       in->clusters, in->spc * 512);
				}
				sl = dr_sectors(c, &n);
				for (i = 0; i < n; i++) {
					dr_fs_loc loc;

					if (sl[i].data_crc != DR_CRC_BAD &&
					    sl[i].header_crc != DR_CRC_BAD)
						continue;
					if (!shown++)
						printf("\nwhere the bad "
						       "sectors land\n");
					if (dr_fs_locate(fs, c, i, &loc) != 0) {
						/* Not addressable by the
						 * filesystem at all: a track
						 * past the formatted area, or
						 * a sector whose size the
						 * format does not use. Noise
						 * read as a sector, not a
						 * fault in anyone's data. */
						printf("  %3d/%d s%-3d  %-8s  "
						       "not part of the "
						       "filesystem (%d-byte "
						       "%s sector%s)\n",
						       sl[i].track, sl[i].side,
						       sl[i].sector_id,
						       (n_out++, "outside"),
						       sl[i].sector_size,
						       sl[i].encoding == DR_ENC_ISO_FM
						         ? "FM" : "MFM",
						       sl[i].track >= in->total_sectors /
						           (in->spt * in->heads)
						         ? ", past the last formatted track"
						         : "");
						continue;
					}
					switch (loc.area) {
					case DR_AREA_FREE:    n_free++; break;
					case DR_AREA_OUTSIDE: n_out++; break;
					case DR_AREA_FILE:    n_file++; break;
					case DR_AREA_FAT:
						n_fat++;
						if (loc.mirror_clean)
							n_mirror++;
						break;
					default:              n_other++; break;
					}
					printf("  %3d/%d s%-3d  %-8s  %s\n",
					       sl[i].track, sl[i].side,
					       sl[i].sector_id,
					       area_name(loc.area), loc.note);
				}
				{
					dr_fs_file ff[64];
					int nf = dr_fs_files(fs, ff, 64), k;

					if (nf)
						printf("\nthe files on this "
						       "disk\n");
					for (k = 0; k < nf; k++)
						printf("  %-14s %8ld byte(s)"
						       "  %s%s%s\n",
						       ff[k].name, ff[k].size,
						       ff[k].bad
						         ? "damaged"
						         : "no bad sector",
						       ff[k].note[0] ? " - " : "",
						       ff[k].note);
				}
				if (shown) {
					/* The number that matters is not how
					 * many sectors failed but how much of
					 * anyone's data is actually at stake.
					 */
					printf("\nof %d bad sector(s): %d in a "
					       "FAT (%d with a clean second "
					       "copy on this disk),\n"
					       "%d in free space, %d outside "
					       "the filesystem, %d carrying "
					       "%d byte(s)\nof a file.\n",
					       shown, n_fat, n_mirror, n_free,
					       n_out, n_file, n_file * 512);
					if (!n_file && !n_fat)
						printf("Nothing anyone stored "
						       "on this disk is "
						       "missing.\n");
				}
			}
		}
		if (g_fs)
			dr_fs_free(g_fs);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "plot")) {
		dr_view *pv;
		FILE *f;
		const char *marker = "/*__DR_DATA__*/null";
		const char *split;

		idx = pick_sector(c, &a);
		if (idx < 0) {
			dr_close(c);
			return 1;
		}
		if (!a.out) {
			fprintf(stderr, "plot needs --out FILE.html\n");
			dr_close(c);
			return 2;
		}
		pv = dr_view_open(c, idx, &a.opt);
		if (!pv) {
			fprintf(stderr, "could not build a view for sector %d\n", idx);
			dr_close(c);
			return 1;
		}
		f = fopen(a.out, "wb");
		if (!f) {
			perror(a.out);
			dr_view_free(pv);
			dr_close(c);
			return 1;
		}
		split = strstr(dr_web_plot, marker);
		if (!split) {
			fprintf(stderr, "plot template is missing its data slot\n");
			fclose(f);
			dr_view_free(pv);
			dr_close(c);
			return 1;
		}
		fwrite(dr_web_plot, 1, (size_t)(split - dr_web_plot), f);
		dr_json_view(c, pv, f);
		fputs(split + strlen(marker), f);
		fclose(f);
		printf("wrote %s - sector index %d, track %d side %d id %d\n",
		       a.out, idx, pv->sect.track, pv->sect.side,
		       pv->sect.sector_id);
		dr_view_free(pv);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "extract")) {
		dr_fs *fs = fs_of(c);
		dr_fs_file ff[128];
		int nf, k, wrote = 0;

		if (!a.out) {
			fprintf(stderr, "extract needs --out DIR\n");
			dr_close(c);
			return 2;
		}
		if (!fs) {
			fprintf(stderr, "no filesystem recognised on this "
			                "disk\n");
			dr_close(c);
			return 1;
		}
		mkdir(a.out, 0777);
		nf = dr_fs_files(fs, ff, 128);
		if (!nf) {
			printf("nothing to write: this disk's filesystem "
			       "(%s) is recognised but its\ndirectory is not "
			       "read by this tool - only FAT12/16 "
			       "directories are.\n", dr_fs_stat(fs)->kind);
			if (g_fs)
				dr_fs_free(g_fs);
			dr_close(c);
			return 0;
		}
		printf("writing to %s\n\n", a.out);
		for (k = 0; k < nf; k++) {
			char path[2400], suffix[16];
			long len = 0;
			uint8_t *b;
			FILE *f;

			if (ff[k].deleted && !a.deleted)
				continue;
			b = dr_fs_read(fs, k, &len);
			if (!b || len <= 0) {
				free(b);
				continue;
			}
			/* Directories that have been churned hold the same
			 * name many times over, at different lengths; each
			 * one is a different moment in the disk's life and
			 * none of them should quietly overwrite another. */
			{
				int dup = 0, j;

				for (j = 0; j < k; j++)
					if (!strcmp(ff[j].name, ff[k].name) &&
					    !(ff[j].deleted && !a.deleted))
						dup++;
				if (dup)
					snprintf(suffix, sizeof(suffix),
					         ".%d", dup + 1);
				else
					suffix[0] = 0;
			}
			if (snprintf(path, sizeof(path), "%s/%s%s", a.out,
			             ff[k].name, suffix) >= (int)sizeof(path)) {
				fprintf(stderr, "path too long for %s\n",
				        ff[k].name);
				free(b);
				continue;
			}
			f = fopen(path, "wb");
			if (f) {
				fwrite(b, 1, (size_t)len, f);
				fclose(f);
				wrote++;
				printf("  %-16s %8ld byte(s)  %s%s\n",
				       path + strlen(a.out) + 1, len,
				       ff[k].bad ? "CONTAINS DAMAGE - "
				                 : "",
				       ff[k].note[0] ? ff[k].note : "ok");
			}
			/*
			 * An archive is a second filesystem in a file, with
			 * a CRC-32 per member. Unpacking it here means the
			 * members the damage missed come out whole and
			 * proven, and the one it hit still gives up
			 * everything the decompressor reached before it.
			 */
			if (a.salvage && ff[k].bad) {
				dr_fs_member mm[64];
				char sub[2600];
				int nm, q;

				snprintf(sub, sizeof(sub), "%s.d", path);
				mkdir(sub, 0777);
				nm = dr_fs_salvage(fs, k, sub, mm, 64);
				for (q = 0; q < nm; q++)
					printf("    %-24s %8ld  %s\n",
					       mm[q].name + strlen(a.out) + 1,
					       mm[q].size, mm[q].note);
				if (nm <= 0)
					rmdir(sub);
			}
			free(b);
		}
		printf("\n%d file(s) written%s\n", wrote,
		       a.deleted ? "" :
		       "; --deleted also writes the ones whose directory "
		       "entry was erased");
		if (g_fs)
			dr_fs_free(g_fs);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "convert")) {
		if (!a.out) {
			fprintf(stderr, "convert needs --out\n");
			dr_close(c);
			return 2;
		}
		if (dr_export(c, a.out, a.format) < 0) {
			fprintf(stderr, "%s\n", dr_last_error(c));
			dr_close(c);
			return 1;
		}
		printf("wrote %s (%s)\n", a.out, a.format);
		dr_close(c);
		return 0;
	}

	if (!strcmp(a.cmd, "serve")) {
		rc = dr_serve(c, a.bind, a.port, &a.opt);
		dr_close(c);
		return rc;
	}

	if (!strcmp(a.cmd, "repair") && a.all) {
		rc = cmd_repair_all(c, &a);
		dr_close(c);
		return rc;
	}

	idx = pick_sector(c, &a);
	if (idx < 0) {
		dr_close(c);
		return 1;
	}

	if (!strcmp(a.cmd, "damage")) {
		int bits[DR_MAX_WEIGHT * 4], nb = 0;
		char *dup, *tok;

		if (a.slip) {
			int at = 0, cells = 0;

			if (sscanf(a.slip, "%d:%d", &at, &cells) != 2 ||
			    !cells || !a.out) {
				fprintf(stderr, "damage --slip wants "
				                "BYTE:CELLS, and --out\n");
				dr_close(c);
				return 2;
			}
			if (dr_damage_slip(c, idx, at, cells) < 0 ||
			    dr_export(c, a.out, a.format) < 0) {
				fprintf(stderr, "%s\n", dr_last_error(c));
				dr_close(c);
				return 1;
			}
			printf("shifted sector %d by %+d cell(s) from byte %d; "
			       "wrote %s\n", idx, cells, at, a.out);
			dr_close(c);
			return 0;
		}
		if (!a.bits || !a.out) {
			fprintf(stderr, "damage needs --bits and --out\n");
			dr_close(c);
			return 2;
		}
		dup = strdup(a.bits);
		for (tok = strtok(dup, ","); tok && nb < (int)(sizeof(bits)/sizeof(bits[0]));
		     tok = strtok(NULL, ","))
			bits[nb++] = atoi(tok);
		free(dup);

		nb = dr_damage(c, idx, bits, nb, a.droponly);
		if (nb < 0) {
			fprintf(stderr, "damage failed: %s\n", dr_last_error(c));
			dr_close(c);
			return 1;
		}
		if (!nb) {
			fprintf(stderr, "no requested bit could be dropped "
			                "(they all read 0 already)\n");
			dr_close(c);
			return 1;
		}
		if (dr_export(c, a.out, a.format) < 0) {
			fprintf(stderr, "%s\n", dr_last_error(c));
			dr_close(c);
			return 1;
		}
		printf("flipped %d bit(s) in sector index %d; wrote %s\n",
		       nb, idx, a.out);
		dr_close(c);
		return 0;
	}

	{
		dr_view *v = dr_view_open(c, idx, &a.opt);

		if (!v) {
			fprintf(stderr, "could not build a view for sector %d "
			                "(unsupported encoding?)\n", idx);
			dr_close(c);
			return 1;
		}

		if (!strcmp(a.cmd, "inspect")) {
			if (a.json)
				dr_json_view(c, v, stdout), printf("\n");
			else if (a.bytes) {
				int b0 = 0, b1 = 0;
				char *colon;
				char *dup = strdup(a.bytes);

				colon = strchr(dup, ':');
				if (colon) {
					*colon = 0;
					b0 = atoi(dup);
					b1 = atoi(colon + 1);
				} else {
					b0 = b1 = atoi(dup);
				}
				free(dup);
				print_view(v, 0);
				print_bytes(v, b0, b1);
			} else
				print_view(v, 24);
			dr_view_free(v);
			dr_close(c);
			return 0;
		}

		if (!strcmp(a.cmd, "repair")) {
			dr_repair_result r;
			dr_mode engine;

			if (!a.json) {
				print_view(v, 12);
				if (a.fs || a.frommirror || a.variants) {
					print_fs(c, idx);
					print_mirror(c, v, idx);
				}
			}

			/* The other FAT's bytes are not a candidate to be
			 * ranked - they are the same data, read from a
			 * different place on the disk. */
			if (a.frommirror) {
				dr_fs_loc loc;
				const uint8_t *m;
				uint8_t sis[512];
				char how[256];
				int proven = 0, ndiff = 0, ff = -1;

				m = mirror_for(c, v, idx, &loc, &proven, &ndiff);
				if (!m && a.fromfile && fs_of(c) &&
				    dr_fs_locate(fs_of(c), c, idx, &loc) == 0 &&
				    v->data_len == (int)sizeof(sis) &&
				    (ff = dr_fs_from_file(fs_of(c), &loc,
				                          a.fromfile, sis,
				                          v->data_len, how,
				                          (int)sizeof(how))) >= 0) {
					uint8_t *msg = malloc((size_t)v->msg_len);
					int crcok = 0;

					printf("\nfrom file  : %s\n", how);
					if (msg) {
						uint16_t k;
						memcpy(msg, v->msg,
						       (size_t)v->msg_len);
						memcpy(msg + v->data_offset,
						       sis, (size_t)v->data_len);
						k = dr_crc16(msg, v->msg_len - 2);
						free(msg);
						crcok = (k == v->stored_crc);
					}
					if (crcok) {
						printf("             its bytes "
						       "reproduce this sector's "
						       "stored CRC - proven\n");
						m = sis;
						proven = 1;
					} else if (ff == 1) {
						/* The agreement either side is
						 * worth more than sixteen bits
						 * that did not survive. */
						printf("             its bytes "
						       "do NOT reproduce the "
						       "stored CRC, but the two "
						       "copies are\n"
						       "             identical "
						       "for thousands of bytes "
						       "either side of this "
						       "sector.\n"
						       "             The stored "
						       "CRC is the thing that "
						       "died here; taking the "
						       "file.\n");
						m = sis;
						proven = 1;
					} else {
						printf("             its bytes "
						       "do NOT reproduce this "
						       "sector's stored CRC, and "
						       "the\n             "
						       "agreement either side is "
						       "too short to override "
						       "it\n");
					}
				} else if (!m && a.fromfile) {
					printf("\nfrom file  : %s\n", how);
				}
				if (!m && fs_of(c) &&
				    dr_fs_locate(fs_of(c), c, idx, &loc) == 0 &&
				    v->data_len == (int)sizeof(sis)) {
					int sr = dr_fs_sister(fs_of(c), &loc,
					                      sis, v->data_len,
					                      how, (int)sizeof(how));
					if (sr >= 0) {
						printf("\nsecond copy: %s\n",
						       how);
						if (sr == 1) {
							m = sis;
							proven = 1;
						} else if (sr == 2 &&
						           payload_matches_crc(v, sis)) {
							printf("             and "
							       "they reproduce this "
							       "sector's stored CRC "
							       "- proven\n");
							m = sis;
							proven = 1;
						} else if (sr == 2) {
							printf("             but "
							       "they do not reproduce "
							       "this sector's stored "
							       "CRC, so the two builds "
							       "differ here too\n");
						}
					}
				}
				if (!m) {
					fprintf(stderr, "--from-copy: nothing "
					        "on this disk holds a second "
					        "copy of these bytes\n");
					rc = 1;
				} else if (dr_set_data(c, v, m, v->data_len) < 0) {
					fprintf(stderr, "--from-copy: write "
					        "failed\n");
					rc = 1;
				} else {
					printf("\nwrote the other copy's %d "
					       "bytes into this sector%s.\n",
					       v->data_len,
					       proven ? " - proven, not ranked"
					              : ", re-stamping the CRC");
					if (a.out && dr_export(c, a.out,
					                       a.format) < 0) {
						fprintf(stderr, "%s\n",
						        dr_last_error(c));
						rc = 1;
					} else if (a.out) {
						printf("wrote %s (%s)\n",
						       a.out, a.format);
					}
				}
				if (g_fs)
					dr_fs_free(g_fs);
				dr_view_free(v);
				dr_close(c);
				return rc;
			}

			/*
			 * Every engine that applies gets a say, and their
			 * candidates are ranked together on one scale. Taking
			 * whichever engine answered first is how a fifteen-bit
			 * re-reading gets applied while a one-bit dropout
			 * repair sits unexamined in another engine's list.
			 */
			if (a.opt.mode == DR_MODE_AUTO) {
				if (dr_repair_auto(c, v, &a.opt, &r) < 0) {
					fprintf(stderr, "search failed\n");
					dr_view_free(v);
					dr_close(c);
					return 1;
				}
				engine = r.count ? r.list[0].origin : DR_MODE_BITS;
			} else {
				int rs;

				engine = a.opt.mode;
				memset(&r, 0, sizeof(r));
				if (engine == DR_MODE_PATTERN)
					rs = dr_pattern_search(v, &a.opt, &r);
				else if (a.opt.mode == DR_MODE_REVS)
					rs = dr_revs_search(v, &a.opt, &r);
				else if (engine == DR_MODE_REBIN)
					rs = dr_rebin_search(v, &a.opt, &r);
				else
					rs = dr_repair_search(v, &a.opt, &r);
				if (rs < 0) {
					fprintf(stderr, "search failed\n");
					dr_view_free(v);
					dr_close(c);
					return 1;
				}
				dr_rescore_data(c, v, &a.opt, &r);
			}

			if (a.json) {
				dr_json_candidates(v, &r, 0, 0, stdout);
				printf("\n");
			} else if (engine == DR_MODE_PATTERN) {
				print_pattern(v, &r, 16);
			} else if (engine == DR_MODE_REBIN) {
				print_rebin(v, &r, 16);
			} else {
				print_candidates(v, &r, 16);
			}

			if (!a.json)
				print_consensus(v, &r);
			if (!a.json && (a.fs || a.variants))
				print_referee(c, v, &r, idx, 5);

			if (a.variants > 0 && !a.json) {
				write_variants(&a, idx, v, &r, a.variants);
				if (g_fs)
					dr_fs_free(g_fs);
				dr_repair_free(&r);
				dr_view_free(v);
				dr_close(c);
				return 0;
			}

			/* --auto only fires when the reading is not in doubt:
			 * a lone candidate, or one that beats the runner-up
			 * by two orders of magnitude. */
			if (a.apply < 0 && a.autoapply && r.count >= 1) {
				if (r.count == 1 ||
				    r.list[1].rel_likelihood <= 0.01)
					a.apply = 0;
				else if (!a.json)
					printf("\n--auto declined: the top "
					       "candidate is not a clear "
					       "winner.\n");
			}

			if (a.apply >= 0) {
				if (a.apply >= r.count) {
					fprintf(stderr, "candidate %d does not exist\n",
					        a.apply);
					rc = 1;
				} else if (dr_apply(c, v, &r.list[a.apply]) < 0) {
					fprintf(stderr, "apply failed\n");
					rc = 1;
				} else {
					int ok = dr_verify(c, idx);
					if (!a.json)
						printf("\napplied candidate %d; "
						       "libhxcfe re-decode says the "
						       "sector is now %s\n",
						       a.apply,
						       ok == 1 ? "CLEAN" :
						       ok == 0 ? "still bad" : "gone");
					if (ok != 1)
						rc = 1;
				}
			}

			if (a.out && rc == 0) {
				if (dr_export(c, a.out, a.format) < 0) {
					fprintf(stderr, "%s\n", dr_last_error(c));
					rc = 1;
				} else if (!a.json) {
					printf("wrote %s (%s)\n", a.out, a.format);
				}
			}

			if (g_fs)
				dr_fs_free(g_fs);
			dr_repair_free(&r);
			dr_view_free(v);
			dr_close(c);
			return rc;
		}

		dr_view_free(v);
	}

	fprintf(stderr, "unknown command '%s'\n", a.cmd);
	dr_close(c);
	return 2;
}
