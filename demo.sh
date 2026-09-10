#!/bin/sh
# DisketteRecover - a guided tour.
#
# Builds the tool, breaks a known-good disk image in two different ways,
# and repairs both - checking each time that the recovered data is
# byte-identical to what was there before. Everything it needs is in
# this directory; nothing is downloaded.
#
#   sh demo.sh
set -e

here=$(cd "$(dirname "$0")" && pwd)
out="$here/demo-out"
dr="$here/disketterecover"
zip="$here/third_party/HxCFloppyEmulator/tests/data/disks_images.zip"

say()  { printf '\n\033[1;36m== %s\033[0m\n' "$1"; }
run()  { printf '\033[2m$ %s\033[0m\n' "$*"; "$@"; }
good() { printf '\033[1;32m   %s\033[0m\n' "$1"; }

say "building (this takes a minute the first time)"
make -C "$here" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" >/dev/null
good "built $dr"

rm -rf "$out"; mkdir -p "$out"
unzip -o -q "$zip" FAT_720kB.hfe -d "$out"
src="$out/FAT_720kB.hfe"
"$dr" convert "$src" --out "$out/original.img" --format RAW_LOADER >/dev/null 2>&1
good "sample disk: FAT_720kB.hfe (a 720K PC floppy, 1440 sectors)"

# ---------------------------------------------------------------- 1 --
say "PART 1 - a single flipped bit in a sector image"

run "$dr" damage "$src" --sector 5 --bits 803 --out "$out/broken.hfe" 2>/dev/null

say "1a. where is the damage?"
"$dr" scan "$out/broken.hfe" 2>/dev/null | sed -n '1,2p;/<--/p;$p'

say "1b. what does that sector look like?"
"$dr" inspect "$out/broken.hfe" 2>/dev/null | sed -n '1,5p'

say "1c. what would make the CRC valid again?"
"$dr" repair "$out/broken.hfe" 2>/dev/null | sed -n '/^search/,$p'

say "1d. apply it and let HxC's own decoder check the result"
run "$dr" repair "$out/broken.hfe" --apply 0 \
        --out "$out/repaired.img" --format RAW_LOADER 2>/dev/null \
        | tail -2
if cmp -s "$out/original.img" "$out/repaired.img"; then
	good "recovered image is byte-identical to the original"
else
	echo "   MISMATCH"; exit 1
fi

# ---------------------------------------------------------------- 2 --
if ! command -v python3 >/dev/null 2>&1; then
	echo; echo "(skipping the flux demo - needs python3)"; exit 0
fi

say "PART 2 - a real flux defect: one transition landing between cells"
echo "   This is the interesting case. Instead of flipping a decoded bit,"
echo "   we displace a single magnetic reversal by 1.3 cell periods in a"
echo "   SuperCard Pro flux dump, and let HxC's PLL mis-read it."
echo "   (Converting to flux takes ~30s and writes a 37MB file.)"

run "$dr" convert "$src" --out "$out/clean.scp" \
        --format SCP_FLUX_STREAM 2>/dev/null
run python3 "$here/tests/tools/scp_jitter.py" \
        "$out/clean.scp" "$out/flux-broken.scp" \
        --sigma 90 --seed 5 --shift 0:3000:1.3

say "2a. one sector now fails its CRC"
"$dr" scan "$out/flux-broken.scp" 2>/dev/null | tail -1

say "2b. zoom in - the flux timings say exactly where it went wrong"
"$dr" inspect "$out/flux-broken.scp" 2>/dev/null \
        | sed -n '1,5p;/least trusted/,+9p'
echo
echo "   Read the 'interval' column: a 0.738T gap had to be called a 4T,"
echo "   and a 4.161T gap had to be called a 3T. Those are not"
echo "   measurements, they are guesses - margin 0.000."

say "2c. the ranked corrections"
"$dr" repair "$out/flux-broken.scp" --threshold 5e-3 2>/dev/null \
        | sed -n '/^search/,$p' | head -12

say "2d. apply the top candidate"
run "$dr" repair "$out/flux-broken.scp" --threshold 5e-3 --apply 0 \
        --out "$out/flux-repaired.img" --format RAW_LOADER 2>/dev/null \
        | tail -2
if cmp -s "$out/original.img" "$out/flux-repaired.img"; then
	good "recovered image is byte-identical to the original"
else
	echo "   MISMATCH"; exit 1
fi

# ---------------------------------------------------------------- 3 --
say "the same thing, in a browser"
echo "   $dr serve $out/flux-broken.scp"
echo "   then open http://127.0.0.1:842/"
echo
echo "   Everything lives in $out - poke at it with:"
echo "     $dr scan|inspect|repair|serve <image>"
