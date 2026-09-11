# The disks, and what was on them

A running tally. Every disk here is a KryoFlux dump of a real 3.5" floppy,
five revolutions per track. "Repaired" means a unique or overwhelming
reading was applied and libhxcfe's own decoder then read the sector
clean; everything else is reported as ambiguous rather than guessed.

| disk | sectors | bad | repaired | what was wrong |
|---|---:|---:|---:|---|
| Disk 1   | 1440 | 2 | **2** | burst on tracks 72-73, sector 6; both sectors are `0xF6` filler, so the data model settles them outright |
| Disk 2   | 2882 | 9 | **3** | a counter table (repaired), two text/binary sectors whose damage reaches the CRC, and five where the CRC cannot arbitrate |
| GasAcc   | 2916 | 1 | **1** | band collapse plus a 4-cell phase slip, on filler; see [the one wrong answer](../README.md#the-one-wrong-answer-and-what-it-cost-to-find-it) |
| Zeus     | 2881 | 2 | 0 | band collapse over the first 100 bytes of `ZEUSNO~1.PPT`; 34 intervals open, ~20M readings, ~300 pass the CRC |
| LGTC0    | 2916 | 2 | **1** | sector 12 on tracks 9-10 |
| Sand     | 2916 | 4 | **1** | sector 10 on tracks 24-27 |
| **total** | **13951** | **20** | **8** | |

## The pattern that keeps recurring

Five of the six disks fail as *the same sector id on consecutive tracks*:

```
Disk 1    sector 6  on tracks 72-73
Disk 2    sector 17 on tracks 41,42,43,44,49  (side 1)
Zeus      sector 9  on tracks 8-9             (side 0)
LGTC0     sector 12 on tracks 9-10            (side 0)
Sand      sector 10 on tracks 24-27           (side 1)
```

A sector id is an angular position, and consecutive tracks are radially
adjacent, so this is one physical mark on the medium crossing several
tracks - a scratch, a contact transfer, a speck. It is not a coincidence
of five disks: it is what floppy damage *is*. Isolated single-bit errors
in the middle of a healthy track are the exception, and every engine in
this tool exists because of it.

The flux confirms it rather than just suggesting it. Sand's four sectors
are damaged in the same byte range of each - the same angular span, on
four radially adjacent tracks:

```
24/1 s10   starts at cell 100530   damaged bytes 418-502
25/1 s10   starts at cell 100516   damaged bytes 412-513
26/1 s10   starts at cell 100516   damaged bytes 385-515
27/1 s10   starts at cell 100486   damaged bytes 387-507
```

`scan` now says so, because it changes what the reader should do: a mark
that crosses four tracks has almost certainly grazed the neighbours too,
and their sectors may be marginal even where the CRC still passes.

```
4 sector(s) with a CRC error; first is index 891 (track 24 side 1 sector 10)
            sector 10 on side 1 fails across tracks 24-27 (4 of them) - one
            physical mark, not 4 faults
```

## What the failures actually are

Ranked by how often they turned up, not by how interesting they are:

1. **Dropped reversals** - a weak pulse misses the detector's threshold.
   All 27 errors with a verifiable truth are of this kind, and none is a
   spurious extra reversal. Half of them are *invisible*: a lost reversal
   that runs two 2T intervals into one 4T is legal MFM, so the timings
   have nothing to object to.
2. **Band collapse** - peak shift tripling over a stretch, so 2T and 4T
   are both pulled toward 3T. Handled by fitting the timing model per
   region rather than per sector.
3. **Phase slip** - a burst leaves the decoder a whole cell out of step,
   and every byte after it is re-framed rather than corrupted. No number
   of bit flips can express the repair.
4. **Weak bits** - the read amplifier firing on noise over unmagnetised
   media. Only visible by comparing the dump's own revolutions: on Disk
   2's `0/1 s18` the five passes disagree about 239 reversals, where a
   healthy sector's passes disagree about none.
5. **A damaged CRC** - the two checksum bytes are the last two bytes of
   the sector and nothing protects them. Damage that reaches the tail
   takes them with it, and then every "CRC-valid" reading matches a
   target that was never on the disk.
