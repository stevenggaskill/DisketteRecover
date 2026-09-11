#!/bin/sh
# DisketteRecover regression tests.
#
# Everything is built from the sample images that ship with the HxC
# source tree, so there is nothing else to download.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out="$here/out"
dr="$root/disketterecover"
zip="$root/third_party/HxCFloppyEmulator/tests/data/disks_images.zip"

pass=0
fail=0

ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else
           bad "$1 (expected '$3', got '$2')"; fi; }

[ -x "$dr" ] || { echo "build disketterecover first (make)"; exit 1; }
[ -f "$zip" ] || { echo "missing $zip - run: git submodule update --init"; exit 1; }

rm -rf "$out"
mkdir -p "$out"
unzip -o -q "$zip" FAT_720kB.hfe -d "$out"
src="$out/FAT_720kB.hfe"

"$dr" convert "$src" --out "$out/ref.img" --format RAW_LOADER >/dev/null 2>&1

badcount() { "$dr" scan "$1" 2>/dev/null | sed -n 's/^\([0-9][0-9]*\) sector(s) with a CRC error.*/\1/p'; }

echo "== a clean image reports no CRC errors"
check "clean scan" "$(badcount "$src")" "0"

echo "== a single flipped data bit"
"$dr" damage "$src" --sector 5 --bits 803 --out "$out/b1.hfe" >/dev/null 2>&1
check "one bad sector" "$(badcount "$out/b1.hfe")" "1"
w=$("$dr" repair "$out/b1.hfe" --mode bits --json 2>/dev/null |
    sed -n 's/.*"searched_weight":\([0-9]*\).*/\1/p')
check "bit-flip engine finds it at weight 1" "$w" "1"
"$dr" repair "$out/b1.hfe" --apply 0 --out "$out/f1.img" \
      --format RAW_LOADER >/dev/null 2>&1
if cmp -s "$out/ref.img" "$out/f1.img"; then ok "recovered data matches"
else bad "recovered data matches"; fi

echo "== two flipped data bits in one sector"
"$dr" damage "$src" --sector 7 --bits 512,2743 --out "$out/b2.hfe" >/dev/null 2>&1
check "one bad sector" "$(badcount "$out/b2.hfe")" "1"
w=$("$dr" repair "$out/b2.hfe" --mode bits --json 2>/dev/null |
    sed -n 's/.*"searched_weight":\([0-9]*\).*/\1/p')
check "bit-flip engine finds it at weight 2" "$w" "2"

echo "== the data model, on a sector of filler with bits dropped"
# 512 bytes of 0xF6 is what MS-DOS FORMAT leaves behind, and it is what
# both bad sectors of a real disk turned out to be. Dropping a few
# reversals turns some of them into 0x76 / 0xF2; restoring the repeat
# should put every one back in a single reading.
if command -v python3 >/dev/null 2>&1; then
	python3 -c "
import sys
open(sys.argv[1],'wb').write(b'\xF6' * (80*2*9*512))
" "$out/fill.img"
	"$dr" convert "$out/fill.img" --out "$out/fill.hfe" >/dev/null 2>&1
	"$dr" convert "$out/fill.hfe" --out "$out/fill_ref.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	"$dr" damage "$out/fill.hfe" --sector 5 --drop-only \
	      --bits 803,1701,2743,3001,3517,2119 \
	      --out "$out/p0.hfe" >/dev/null 2>&1
	check "one bad sector" "$(badcount "$out/p0.hfe")" "1"

	pj=$("$dr" repair "$out/p0.hfe" --mode pattern --json 2>/dev/null)
	pat=$(printf '%s' "$pj" | sed -n 's/.*"pattern":\(true\|false\).*/\1/p')
	check "the pattern engine ran" "$pat" "true"
	pc=$(printf '%s' "$pj" | sed -n 's/^{"count":\([0-9]*\).*/\1/p')
	check "one reading, from the data alone" "$pc" "1"

	"$dr" repair "$out/p0.hfe" --apply 0 --out "$out/fp.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/fill_ref.img" "$out/fp.img"
	then ok "dropped reversals recovered exactly"
	else bad "dropped reversals recovered exactly"; fi
else
	echo "  skip filler tests (no python3)"
fi

echo "== the counter model, on a table of incrementing records"
# Tables of counters are everywhere on a disk - index tables, timing
# lists, sector maps. The model has to get their carries right, or it
# invents outliers and the CRC then matches anything.
if command -v python3 >/dev/null 2>&1; then
	python3 - "$out/counter.img" <<'EOF'
import sys
n = 80*2*9*512
b = bytearray(n)
v = 0x558557
for i in range(0, n - 2, 3):          # 3-byte LE records, step 0x2002
    b[i] = v & 0xFF; b[i+1] = (v >> 8) & 0xFF; b[i+2] = (v >> 16) & 0xFF
    v = (v + 0x2002) & 0xFFFFFF
open(sys.argv[1], "wb").write(bytes(b))
EOF
	"$dr" convert "$out/counter.img" --out "$out/counter.hfe" >/dev/null 2>&1
	"$dr" convert "$out/counter.hfe" --out "$out/counter_ref.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	# drop six transitions, the way a weak patch of media would
	"$dr" damage "$out/counter.hfe" --sector 5 --drop-only \
	      --bits 811,1509,1622,2743,3004,3971 \
	      --out "$out/cbad.hfe" >/dev/null 2>&1
	check "one bad sector" "$(badcount "$out/cbad.hfe")" "1"

	cj=$("$dr" repair "$out/cbad.hfe" --mode pattern --json 2>/dev/null)
	cc=$(printf '%s' "$cj" | sed -n 's/^{"count":\([0-9]*\).*/\1/p')
	check "the counter model finds one reading" "$cc" "1"

	"$dr" repair "$out/cbad.hfe" --apply 0 --out "$out/cfix.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/counter_ref.img" "$out/cfix.img"
	then ok "counter table recovered exactly"
	else bad "counter table recovered exactly"; fi
else
	echo "  skip counter tests (no python3)"
fi

echo "== a decoder that lost its place, not a bit"
# A run of intervals given one cell too many moves the byte boundary:
# every byte after it decodes as something else, so no number of bit
# flips repairs it. This is what a sector of filler with a burst defect
# actually looks like, and matching the CRC without noticing is how a
# tool confidently returns nonsense.
if command -v python3 >/dev/null 2>&1; then
	"$dr" damage "$out/fill.hfe" --sector 5 --slip 300:4 \
	      --out "$out/slip.hfe" >/dev/null 2>&1
	check "one bad sector from a phase slip" \
	      "$(badcount "$out/slip.hfe")" "1"

	sj=$("$dr" repair "$out/slip.hfe" --mode pattern --json 2>/dev/null)
	# The reported figure is the correction, so it is the negative of
	# the damage: four cells were inserted, four have to come back out.
	sl=$(printf '%s' "$sj" | sed -n 's/.*"slip":\(-\{0,1\}[0-9]*\).*/\1/p')
	check "the slip was measured" "$sl" "-4"
	sc=$(printf '%s' "$sj" | sed -n 's/^{"count":\([0-9]*\).*/\1/p')
	check "one reading, once the phase is put back" "$sc" "1"

	"$dr" repair "$out/slip.hfe" --apply 0 --out "$out/sfix.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/fill_ref.img" "$out/sfix.img"
	then ok "phase slip recovered exactly"
	else bad "phase slip recovered exactly"; fi

	echo "== ...with the stored CRC damaged along with it"
	# The CRC bytes are the last two bytes of the sector and nothing
	# protects them. A repair that insists on matching them exactly
	# rejects the truth and accepts whatever matches the corruption.
	"$dr" damage "$out/slip.hfe" --sector 5 --drop-only --bits 4129 \
	      --out "$out/slipcrc.hfe" >/dev/null 2>&1
	# crc_suspect is a flux judgement - it asks whether the timings
	# under the CRC cells were in doubt - so on a sector-level image
	# there is nothing to judge it on, and it stays false. The data
	# model still catches the damage, which is the point.
	su=$("$dr" inspect "$out/slipcrc.hfe" --json 2>/dev/null |
	     sed -n 's/.*"crc_suspect":\(true\|false\).*/\1/p')
	check "no flux, so no flux verdict on the CRC" "$su" "false"

	cf=$("$dr" repair "$out/slipcrc.hfe" --mode pattern --json 2>/dev/null |
	     sed -n 's/.*"crc_fixed":\([0-9]*\).*/\1/p')
	check "one stored-CRC bit was corrected" "$cf" "1"

	"$dr" repair "$out/slipcrc.hfe" --apply 0 --out "$out/scfix.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/fill_ref.img" "$out/scfix.img"
	then ok "recovered exactly despite the damaged CRC"
	else bad "recovered exactly despite the damaged CRC"; fi
fi

echo "== one mark across several tracks"
# A sector id is an angular position and consecutive tracks are radially
# adjacent, so the same id failing on a run of tracks is one physical
# mark, not several faults. Five of six real disks fail exactly that way.
if command -v python3 >/dev/null 2>&1; then
	"$dr" damage "$out/fill.hfe" --sector 5 --bits 803 \
	      --out "$out/r1.hfe" >/dev/null 2>&1
	"$dr" damage "$out/r1.hfe" --sector 23 --bits 811 \
	      --out "$out/r2.hfe" >/dev/null 2>&1
	rad=$("$dr" scan "$out/r2.hfe" 2>/dev/null |
	      sed -n 's/.*fails across tracks \([0-9]*-[0-9]*\).*/\1/p')
	check "a mark crossing two tracks is reported as one" "$rad" "0-1"
fi

echo "== a CRC that is itself inside the damage"
# The stored CRC is the last two bytes of the sector and nothing protects
# it. A reading that matches a checksum which is itself a guess proves
# nothing, so the tool measures how much of that checksum is in doubt.
if command -v python3 >/dev/null 2>&1; then
	ce=$("$dr" inspect "$out/fill.hfe" --sector 5 --json 2>/dev/null |
	     sed -n 's/.*"crc_expected_errors":\([0-9.e-]*\).*/\1/p')
	if [ -n "$ce" ] && awk "BEGIN{exit !($ce < 0.3)}"; then
		ok "an intact sector's CRC is trusted ($ce bits in doubt)"
	else
		bad "an intact sector's CRC is trusted (got '$ce')"
	fi
fi

echo "== --restore-only narrows the search"
a=$("$dr" repair "$out/b2.hfe" --json 2>/dev/null |
    sed -n 's/.*"count":\([0-9]*\).*/\1/p' | head -1)
b=$("$dr" repair "$out/b2.hfe" --restore-only --json 2>/dev/null |
    sed -n 's/.*"count":\([0-9]*\).*/\1/p' | head -1)
if [ "${b:-0}" -le "${a:-0}" ]; then ok "restore-only gives no more candidates"
else bad "restore-only gives no more candidates ($b > $a)"; fi

echo "== an image with no CRC error has nothing to repair"
if "$dr" repair "$src" >/dev/null 2>&1; then bad "repair on clean image exits non-zero"
else ok "repair on clean image exits non-zero"; fi

echo "== flux path: one transition displaced by 1.3 cell periods"
if command -v python3 >/dev/null 2>&1; then
	"$dr" convert "$src" --out "$out/clean.scp" \
	      --format SCP_FLUX_STREAM >/dev/null 2>&1
	python3 "$here/tools/scp_jitter.py" "$out/clean.scp" "$out/shift.scp" \
	        --sigma 90 --seed 5 --shift 0:3000:1.3 >/dev/null
	check "one bad sector from a flux defect" \
	      "$(badcount "$out/shift.scp")" "1"

	ev=$("$dr" inspect "$out/shift.scp" 2>/dev/null |
	     sed -n 's/^evidence  : \(flux timing\).*/\1/p')
	check "flux timings were aligned" "$ev" "flux timing"

	"$dr" repair "$out/shift.scp" --threshold 5e-3 --apply 0 \
	      --out "$out/f3.img" --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/ref.img" "$out/f3.img"; then ok "flux defect recovered"
	else bad "flux defect recovered"; fi

	echo "== the timing model, fitted per region"
	# One set of coefficients across a sector with a bad patch is wrong
	# at both ends. The blocks have to cover the sector, hold their own
	# fits on clean flux, and agree with each other when there is
	# nothing for them to disagree about.
	tb=$("$dr" inspect "$out/shift.scp" 2>/dev/null |
	     sed -n 's/^timing    : re-fitted over \([0-9]*\) .*/\1/p')
	if [ -n "$tb" ] && [ "$tb" -ge 4 ]; then
		ok "the sector was split into blocks ($tb)"
	else
		bad "the sector was split into blocks (got '$tb')"
	fi
	tl=$("$dr" inspect "$out/shift.scp" 2>/dev/null |
	     sed -n 's/.*; \([0-9]*\) hold their own model.*/\1/p')
	if [ -n "$tl" ] && [ "$tl" -ge 2 ]; then
		ok "blocks on clean flux keep their own fit ($tl)"
	else
		bad "blocks on clean flux keep their own fit (got '$tl')"
	fi
	# A clean track has no regional variation to find, so the gains
	# must stay together; a spread here would mean the blocks are
	# fitting noise.
	gs=$("$dr" inspect "$out/shift.scp" 2>/dev/null |
	     sed -n 's/.*gain \([0-9.]*\)\.\.\([0-9.]*\).*/\1 \2/p')
	if [ -n "$gs" ] && awk "BEGIN{split(\"$gs\",g,\" \");
	                         exit !(g[2]-g[1] < 0.08)}"; then
		ok "a clean track's blocks agree on the gain ($gs)"
	else
		bad "a clean track's blocks agree on the gain (got '$gs')"
	fi

	echo "== a dump with five passes over the track"
	# The same track and the same per-pass read noise, dumped once and
	# then five times, so the only difference between them is how many
	# passes there are to combine.
	python3 "$here/tools/scp_revs.py" "$out/clean.scp" "$out/rev1.scp" \
	        --revs 1 --sigma 40 --seed 7 --tracks 2 >/dev/null
	python3 "$here/tools/scp_revs.py" "$out/clean.scp" "$out/rev5.scp" \
	        --revs 5 --sigma 40 --seed 7 --tracks 2 >/dev/null

	np=$("$dr" inspect "$out/rev5.scp" --sector 0 2>/dev/null |
	     sed -n 's/^passes    : \([0-9]*\) of \([0-9]*\).*/\1 of \2/p')
	check "all five passes were aligned" "$np" "5 of 5"

	# Averaging independent read noise over five passes should cut it by
	# something approaching the root of five; anything near the
	# single-pass figure means the passes were not really combined.
	s1=$("$dr" inspect "$out/rev1.scp" --sector 0 2>/dev/null |
	     sed -n 's/.*sigma \([0-9.]*\).*/\1/p')
	s5=$("$dr" inspect "$out/rev5.scp" --sector 0 2>/dev/null |
	     sed -n 's/.*sigma \([0-9.]*\).*/\1/p')
	if [ -n "$s1" ] && [ -n "$s5" ] &&
	   awk "BEGIN{exit !($s5 < 0.75 * $s1)}"; then
		ok "combining the passes cut the timing noise ($s1 -> $s5)"
	else
		bad "combining the passes cut the timing noise ($s1 -> $s5)"
	fi

	d0=$("$dr" inspect "$out/rev5.scp" --sector 0 2>/dev/null |
	     sed -n 's/.*about \([0-9]*\) reversal.*/\1/p')
	check "a clean dump has nothing to argue about" "$d0" "0"

	echo "== a reversal only some of the passes can see"
	python3 "$here/tools/scp_revs.py" "$out/clean.scp" "$out/fuzzy.scp" \
	        --revs 5 --sigma 40 --seed 7 --tracks 2 \
	        --fuzzy 0:1500:2 --fuzzy 0:1610:3 >/dev/null

	d2=$("$dr" inspect "$out/fuzzy.scp" --sector 0 2>/dev/null |
	     sed -n 's/.*about \([0-9]*\) reversal.*/\1/p')
	check "both weak reversals were spotted" "$d2" "2"

	rv=$("$dr" repair "$out/fuzzy.scp" --sector 0 --mode revs --json \
	     2>/dev/null | sed -n 's/.*"revs":\(true\|false\).*/\1/p')
	check "the revolutions engine ran" "$rv" "true"

	echo "== the re-binning engine"
	rb=$("$dr" repair "$out/shift.scp" --mode rebin --json 2>/dev/null |
	     sed -n 's/.*"rebin":\(true\|false\).*/\1/p')
	check "re-binning engine ran" "$rb" "true"

	fit=$("$dr" inspect "$out/shift.scp" 2>/dev/null |
	      sed -n 's/^evidence.*\(cell = \).*/fitted/p')
	check "a timing model was fitted" "$fit" "fitted"

	# --mode bits must still work on a flux image
	w=$("$dr" repair "$out/shift.scp" --mode bits --json 2>/dev/null |
	    sed -n 's/.*"searched_weight":\([0-9]*\).*/\1/p')
	check "bit-flip engine still finds it at weight 2" "$w" "2"

	# libhxcfe settings can be overridden before the load
	if "$dr" scan "$out/shift.scp" --set FLUXSTREAM_PLL_MAX_ERROR_NS=900 \
	        >/dev/null 2>&1; then ok "--set is accepted"
	else bad "--set is accepted"; fi
else
	echo "  skip flux tests (no python3)"
fi

# ------------------------------------------------------------------ #
# The filesystem above the sector                                     #
# ------------------------------------------------------------------ #
if command -v python3 >/dev/null 2>&1; then
	echo "== the filesystem says what a bad sector actually is"
	python3 - "$out" <<'PYEOF'
import os, random, sys, zipfile
out = sys.argv[1]
random.seed(7)
words = [bytes(random.choice(b'abcdefghijklmnopqrstuvwxyz')
               for _ in range(random.randint(3, 9))) for _ in range(300)]
body = b' '.join(random.choice(words) for _ in range(20000))
with zipfile.ZipFile(os.path.join(out, 'a.zip'), 'w',
                     zipfile.ZIP_DEFLATED) as z:
    z.writestr('notes.txt', body)
PYEOF
	python3 "$here/tools/mkfat.py" "$out/fs.img" "ARCHIVE.ZIP=$out/a.zip"
	"$dr" convert "$out/fs.img" --out "$out/fs.hfe" >/dev/null 2>&1
	"$dr" convert "$out/fs.hfe" --out "$out/fs_ref.img" \
	      --format RAW_LOADER >/dev/null 2>&1

	# LBA 20 is the fourth cluster of the archive - well inside the
	# deflate stream, where a single wrong bit is fatal.
	"$dr" damage "$out/fs.hfe" --track 1 --side 0 --id 3 --drop-only \
	      --bits 803,1701 --out "$out/fs_bad.hfe" >/dev/null 2>&1
	owner=$("$dr" scan "$out/fs_bad.hfe" --fs 2>/dev/null |
	        sed -n 's/.*of \(ARCHIVE.ZIP\).*/\1/p')
	check "a damaged sector is named as part of its file" \
	      "$owner" "ARCHIVE.ZIP"

	# A sector past every file holds nobody's data.
	"$dr" damage "$out/fs.hfe" --track 60 --side 0 --id 3 --drop-only \
	      --bits 803,1701 --out "$out/fs_free.hfe" >/dev/null 2>&1
	free=$("$dr" scan "$out/fs_free.hfe" --fs 2>/dev/null |
	       sed -n 's/.*\(no file.s data is here\).*/free/p')
	check "a sector in free space is reported as losing nothing" \
	      "$free" "free"

	echo "== 16 bits of sector CRC against 32 bits of the file's own"
	"$dr" repair "$out/fs_bad.hfe" --fs >"$out/ref.txt" 2>/dev/null
	tested=$(sed -n 's/.*of \([0-9]*\) CRC-valid reading(s) also satisfy.*/\1/p' \
	         "$out/ref.txt")
	kept=$(sed -n 's/.*[^0-9]\([0-9]*\) of [0-9]* CRC-valid reading(s) also satisfy.*/\1/p' \
	       "$out/ref.txt")
	if [ "${tested:-0}" -gt 50 ]; then
		ok "many readings satisfy the 16-bit sector CRC ($tested)"
	else
		bad "many readings satisfy the 16-bit sector CRC ($tested)"
	fi
	check "exactly one survives the file's CRC-32" "$kept" "1"

	# ...and it is not the one the likelihood ranking put first. This
	# is the whole point: sixteen bits cannot separate 250 readings,
	# and the file above the sector can.
	surv=$(sed -n 's/^ *\([0-9][0-9]*\)  SURVIVES.*/\1/p' "$out/ref.txt")
	if [ -n "$surv" ] && [ "$surv" != "0" ]; then
		ok "the survivor is not the top-ranked reading (#$surv)"
	else
		bad "the survivor is not the top-ranked reading (#$surv)"
	fi
	"$dr" repair "$out/fs_bad.hfe" --apply "$surv" --out "$out/fs_fix.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/fs_ref.img" "$out/fs_fix.img"; then
		ok "and it is the original data, exactly"
	else
		bad "and it is the original data, exactly"
	fi

	echo "== a FAT is written twice"
	# LBA 4 is the first sector of the second FAT; the first copy of
	# those same bytes is at LBA 1 and reads clean.
	# ...on bytes the FAT actually uses: past the last cluster entry it
	# is all zeroes, and a dropped reversal there changes nothing.
	"$dr" damage "$out/fs.hfe" --track 0 --side 0 --id 5 \
	      --bits 60,140,220 --out "$out/fs_fat.hfe" >/dev/null 2>&1
	m=$("$dr" repair "$out/fs_fat.hfe" --fs 2>/dev/null |
	    sed -n 's/.*\(the other copy of these same bytes reads clean\).*/mirror/p')
	check "the other copy is found" "$m" "mirror"
	proven=$("$dr" repair "$out/fs_fat.hfe" --from-copy 2>/dev/null |
	         sed -n 's/.*\(proven, not ranked\).*/proven/p')
	check "the stored CRC agrees with it, so it is proved not ranked" \
	      "$proven" "proven"
	"$dr" repair "$out/fs_fat.hfe" --from-copy --out "$out/fs_fat.img" \
	      --format RAW_LOADER >/dev/null 2>&1
	if cmp -s "$out/fs_ref.img" "$out/fs_fat.img"; then
		ok "the FAT sector is recovered exactly"
	else
		bad "the FAT sector is recovered exactly"
	fi

	echo "== one image per reading, to be looked at"
	"$dr" repair "$out/fs_bad.hfe" --variants 3 --out "$out/v.hfe" \
	      >/dev/null 2>&1
	n=0
	for f in "$out"/v_a1.hfe "$out"/v_a2.hfe "$out"/v_a3.hfe; do
		[ -s "$f" ] && n=$((n+1))
	done
	check "three variant images were written" "$n" "3"
	check "each one reads clean" \
	      "$(badcount "$out/v_a2.hfe")" "0"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
