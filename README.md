# DisketteRecover

CRC-guided, flux-level repair of floppy disk images, built on the
[HxC Floppy Emulator library](https://github.com/jfdelnero/HxCFloppyEmulator)
(the same `libhxcfe` that the
[Flathub HxCFloppyEmulator app](https://github.com/flathub/fr.free.hxc2001.HxCFloppyEmulator)
ships).

A floppy sector ends in a 16-bit CRC. When that CRC fails, every existing
tool tells you the same thing: *bad sector*. But the disk usually is not
badly wrong - it is slightly wrong, in one or two bit cells, and the raw
flux dump often still says exactly where. DisketteRecover finds that
place, shows it to you at the level of individual flux transitions, and
searches for the smallest, most plausible correction that makes the CRC
come out clean again.

Everything is done through libhxcfe, so the ~100 image formats it reads
and writes come for free, and every repair is verified by re-running
HxC's own decoder over the patched track.

```
$ disketterecover scan   dump.scp        # where are the CRC errors?
$ disketterecover inspect dump.scp       # zoom into the first one
$ disketterecover repair dump.scp        # rank the plausible corrections
$ disketterecover serve  dump.scp        # ...or do it in a browser
```

---

## What it actually does

### 1. Find the first invalid CRC

`scan` walks every track and side, decodes ISO/IBM MFM and FM sectors
through libhxcfe, and reports the address-mark CRC and the data-field CRC
for each one.

```
 idx  trk sd  ord   id  size  enc      hdr-crc  data-crc  data-cell
 ---- --- --  ---  ---  ----  -------  -------  --------  ---------
    4   0  0    4    5   512  ISO/MFM  ok       ok            45344
    5   0  0    5    6   512  ISO/MFM  ok       BAD           55872  <--
    6   0  0    6    7   512  ISO/MFM  ok       ok            66400

1 sector(s) with a CRC error; first is index 5 (track 0 side 0 sector 6)
```

### 2. Show the stream data for that sector

`inspect` (and the browser view) opens the sector's CRC-covered message -
the sync bytes, the data mark, the data, and the stored CRC - and lines
it up against the bit cells and the raw flux transitions underneath.

For every flux interval you get the measured length in cell periods, the
bin the PLL put it in, and how close it came to the boundary between
bins:

```
  bit    byte  role  bitpos  cell    state  interval      bin   margin  p_err   evidence
  2124   265   data  4       7481    0       3.013T/1506  2     0.000   0.49    flux #####
  2127   265   data  7       7487    0       4.161T/2081  3     0.000   0.49    flux #####
  2128   266   data  0       7489    0       0.738T/369   4     0.000   0.49    flux #####
  2130   266   data  2       7493    0       2.012T/1006  3     0.000   0.49    flux #####
```

A 0.738T interval that had to be called a 4T is not a measurement - it is
a guess, and that is where the sector broke.

### 3. Bin the transitions and assign probabilities

Each flux interval is divided by the local cell period and compared with
the cell count the decoder actually assigned it. The distance to the
nearest decision boundary is the *margin*; assuming Gaussian timing
jitter with standard deviation `--jitter` (in cell periods), the
probability that the interval was binned wrongly is

```
p_err = 0.5 * erfc(margin / (jitter * sqrt(2)))
```

That probability is spread across every cell the interval covers - any of
them could have carried the reversal - and each decoded bit inherits the
combined probability of the two cells that produce it. Cells libhxcfe
already flagged as weak are floored at a high probability, unless the
flag fires so widely that it carries no information. Anything below
`--threshold` is **assumed good** and is left out of the search.

### 4. Cycle through the most likely corrections

A CRC is affine over GF(2): flipping message bit *p* always XORs a fixed
mask into the CRC, whatever the data is. So repairing a sector is finding
a set of bits whose masks XOR to the current syndrome - which turns into
a hash lookup rather than a re-computation per trial.

* weight 1 and 2 are searched **exhaustively** over the whole field, so
  the search works even on images with no timing information at all;
* weight 3 and up enumerate the least trustworthy bits and let the hash
  supply the remaining one;
* the search stops at the first weight that yields a CRC-valid reading;
* every survivor is re-verified by actually recomputing the CRC;
* candidates are ranked by the product of their bits' error
  probabilities, so the most plausible reading comes first.

Press `next candidate` (or `n`) in the browser to step through them; the
flux strip and the hex dump update to show the reading each one implies.

### 5. Write the corrected image back

`apply` patches the bit cells in the track - re-encoding the affected
bytes so the MFM clock cells stay legal - then re-runs libhxcfe's decoder
over the patched track and reports whether the sector now reads clean.
`--out` exports through any libhxcfe writer (`disketterecover formats`),
so you can go back out to HFE, IMG, SCP, IPF, ...

---

## The honest limitations

A 16-bit CRC pins a 512-byte sector down to one part in 65536. A data
field offers a few thousand places to flip a bit, so **alternative
readings that also satisfy the CRC are normal, not exceptional** - a
three-bit error will quite often have a one-bit alias.

DisketteRecover therefore does two things rather than one: it tells you
how much the top candidate wins by, and it tells you when the ranking is
meaningless.

```
margin    : the top candidate is 1.75e+03 x more likely than the next; clear winner
```

```
note      : no flux timing in this image, so every bit carries the same prior.
            Candidates of equal weight cannot be told apart; the lowest weight
            is not necessarily the true error. Re-dump as flux (SCP/KryoFlux/A2R)
            to get a real ranking.
```

The ranking is only as good as the evidence. On a sector-level image
(IMG, HFE, ADF) there is no timing to bin, every bit gets the same prior,
and all the tool can offer is "here are the minimum-weight readings".
On a flux dump (SCP, KryoFlux stream, A2R, DFI, MFI, HxC stream, FDX) the
margins are real and the top candidate is usually right by a wide margin.

Currently supported encodings for repair: **ISO/IBM MFM** (System 34, the
PC/Atari/Amstrad family) and **ISO/IBM FM** (System 3740). Amiga MFM uses
a checksum rather than a CRC-16 and is not handled yet; nor are the GCR
formats.

---

## Building

```sh
git clone --recursive https://github.com/stevenggaskill/DisketteRecover
cd DisketteRecover
make
```

`make` builds the pinned HxC submodule (`libhxcadaptor` + `libhxcfe`) and
then the tool. Only a C compiler and make are needed; the browser view is
compiled into the binary.

```sh
make test        # regression suite, builds its images from the HxC samples
```

## Usage

```
disketterecover <command> [options] <image>

commands:
  scan     <image>              list every sector and flag CRC failures
  inspect  <image>              zoomed view of one sector's cells+bytes
  repair   <image>              search for the most likely CRC-valid fix
  damage   <image>              flip data bits on purpose (test images)
  serve    <image>              browser UI for inspect + repair
  convert  <image>              write the image out in another format
  formats                       list libhxcfe export formats

selection:
  --sector N        sector index from `scan` (default: first bad CRC)
  --track T --side S --id R     select by physical address instead

model options:
  --threshold P     p_err below this is 'assumed good' (default 1e-3)
  --base-perr P     prior error rate with no timing evidence (2e-4)
  --jitter S        flux jitter sigma in cell periods (0.16)
  --max-weight N    deepest error weight to search (3, max 6)
  --max-pool N      how many low-confidence bits to consider (96)
  --max-results N   cap on returned candidates (256)

repair options:
  --apply K         apply candidate K (0 = most likely) and verify
  --auto            apply candidate 0 when the search is unambiguous
  --out FILE        write the patched image
  --format NAME     export format for --out (default: HXC_HFE)
```

### Worked example

```sh
# a flux dump with one transition landing on a cell boundary
disketterecover scan   dump.scp                 # -> sector 0 has a bad data CRC
disketterecover inspect dump.scp                # -> the 0.74T interval at cell 7489
disketterecover repair dump.scp --threshold 5e-3
#   result : 256 CRC-valid candidate(s) at weight 2
#   margin : top candidate 1.75e+03 x more likely than the next; clear winner
#      #0  byte 266 bit 1: 0->1 (p=0.49), byte 266 bit 2: 0->1 (p=0.49)
disketterecover repair dump.scp --threshold 5e-3 --apply 0 --out fixed.hfe
#   applied candidate 0; libhxcfe re-decode says the sector is now CLEAN
```

### Browser view

```sh
disketterecover serve dump.scp          # http://127.0.0.1:842/
```

![the zoomed view](docs/zoomed-view.png)

The strip shows, top to bottom: the bin and measured length of each flux
interval, the reconstructed read waveform, the bit cells shaded by how
confidently they were binned (green under the threshold, red where the
decoder had to guess), the decoded bits, and the byte boundaries. The
candidate currently selected is outlined in blue, with its flipped bits
picked out in the bit row and in the hex dump.

## Making test images

`damage` flips decoded bits on purpose and writes the result out, which
is the quickest way to produce a broken image:

```sh
disketterecover damage good.hfe --sector 5 --bits 803 --out broken.hfe
```

For a defect that is genuinely at the flux level - a transition sitting
between two cells, so the PLL has to guess - `tests/tools/scp_jitter.py`
adds position jitter to a SuperCard Pro dump and can displace a chosen
transition:

```sh
disketterecover convert good.hfe --out good.scp --format SCP_FLUX_STREAM
python3 tests/tools/scp_jitter.py good.scp broken.scp \
        --sigma 90 --shift 0:3000:1.3
```

## Layout

```
src/dr_core.c     image load/save, sector scan, cell encode/decode
src/dr_view.c     the zoomed view: cells, flux bins, confidence model
src/dr_flux.c     realigning the cell stream with the raw pulse list
src/dr_repair.c   the GF(2) CRC search and the patcher
src/dr_crc.c      CRC-16/CCITT and its per-bit linear masks
src/dr_json.c     JSON for the CLI and the viewer
src/dr_http.c     the built-in HTTP server
web/index.html    the browser view (compiled into the binary)
```

## Licence

GPL-2.0-or-later, matching libhxcfe. HxC Floppy Emulator is
copyright (C) 2006-2026 Jean-François DEL NERO.
