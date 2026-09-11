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

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
