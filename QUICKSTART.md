# Quick start

This archive is self-contained: the DisketteRecover source, the pinned
HxC Floppy Emulator sources it builds against, and the sample disk
images the demo uses. Nothing is downloaded.

## Requirements

A C compiler, `make`, `unzip`, and (for the flux half of the demo)
`python3`. On Linux or macOS that is usually all already there:

```sh
# Debian / Ubuntu
sudo apt install build-essential unzip python3

# macOS
xcode-select --install
```

On Windows use WSL, or MSYS2/MinGW.

## Run the tour

```sh
cd DisketteRecover
sh demo.sh
```

It builds the tool, then walks through two repairs, checking each
recovered image against the original byte for byte:

1. **a flipped bit in a sector image** - found at weight 1, applied,
   verified;
2. **a real flux defect** - one magnetic reversal displaced by 1.3 cell
   periods in a SuperCard Pro dump, so HxC's PLL has to guess. The flux
   binning points straight at it (a 0.738T gap called a 4T), and the top
   candidate wins by ~1750x.

Part 2 writes a 37 MB flux file and takes about a minute.

## Then try the browser view

```sh
./disketterecover serve demo-out/flux-broken.scp
# open http://127.0.0.1:842/
```

Press **search**, then **next candidate** (or `n`) to step through the
readings that would make the CRC valid. The strip at the top shows each
flux interval's measured length and the bin it was forced into, the
reconstructed read waveform, and the bit cells shaded by how confidently
they were binned - green below the "assumed good" threshold, red where
the decoder had to guess. **apply & verify** patches the cells and
re-runs HxC's own decoder over the result.

## On your own disks

```sh
./disketterecover scan    mydisk.scp     # where are the CRC errors?
./disketterecover inspect mydisk.scp     # zoom into the first one
./disketterecover repair  mydisk.scp     # rank the corrections
./disketterecover serve   mydisk.scp     # ...in a browser
./disketterecover formats                # what it can write back out
```

It reads whatever libhxcfe reads - HFE, IMG, ADF, DSK, IPF, SCP,
KryoFlux streams, A2R, DFI, MFI, WOZ and a hundred others. The ranking
is only meaningful on **flux** dumps (SCP, KryoFlux, A2R, DFI, MFI,
HxC stream, FDX); on a sector-level image there is no timing to bin and
the tool says so rather than pretending otherwise.

Repair currently covers ISO/IBM MFM (System 34 - PC, Atari ST, Amstrad,
MSX...) and ISO/IBM FM (System 3740). Amiga MFM and the GCR formats are
listed by `scan` but not repaired.

See `README.md` for how the confidence model and the CRC search work,
and for the limitations that matter.

## Everything is GPL-2.0-or-later

HxC Floppy Emulator is copyright (C) 2006-2026 Jean-François DEL NERO.
