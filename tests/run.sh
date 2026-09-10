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
w=$("$dr" repair "$out/b1.hfe" --json 2>/dev/null |
    sed -n 's/.*"searched_weight":\([0-9]*\).*/\1/p')
check "found at weight 1" "$w" "1"
"$dr" repair "$out/b1.hfe" --apply 0 --out "$out/f1.img" \
      --format RAW_LOADER >/dev/null 2>&1
if cmp -s "$out/ref.img" "$out/f1.img"; then ok "recovered data matches"
else bad "recovered data matches"; fi

echo "== two flipped data bits in one sector"
"$dr" damage "$src" --sector 7 --bits 512,2743 --out "$out/b2.hfe" >/dev/null 2>&1
check "one bad sector" "$(badcount "$out/b2.hfe")" "1"
w=$("$dr" repair "$out/b2.hfe" --json 2>/dev/null |
    sed -n 's/.*"searched_weight":\([0-9]*\).*/\1/p')
check "found at weight 2" "$w" "2"

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
else
	echo "  skip flux tests (no python3)"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
