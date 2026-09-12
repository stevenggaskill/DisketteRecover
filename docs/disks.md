# The disks, and what was on them

A running tally. Every disk here is a KryoFlux dump of a real 3.5"
floppy, five revolutions per track, handed over with no history.

The table used to count repaired sectors. It now counts two things,
because the first one turned out to be the wrong question. `--fs` reads
the filesystem on the disk and says what each bad sector *is*; more than
half of them, across these eight disks, hold no file's data at all. A
sector in free space does not need repairing, and reporting it as a
failure was misleading.

| disk | sectors | bad | of those, in a file | file sectors recovered | what was wrong |
|---|---:|---:|---:|---:|---|
| Disk 1   | 1440 | 2 | **0** | - | sector 6 on tracks 72-73: `0xF6` filler in **free space**. The data model repairs it outright; nothing was lost either way |
| Disk 2   | 2882 | 9 | 5 | **2** | 3 in FAT copy 2 - all recovered from FAT copy 1, one of them *proved* by the sector's own stored CRC; 1 in free space; 5 in QuickBooks backups, of which 2 repaired and 3 still ambiguous |
| GasAcc   | 2916 | 1 | **0** | - | band collapse plus a 4-cell phase slip, on filler, in **free space**; see [the one wrong answer](../README.md#the-one-wrong-answer-and-what-it-cost-to-find-it) |
| Zeus     | 2881 | 2 | 2 | **1** | both inside `ZEUSNO~1.PPT`. One is in the preview thumbnail, which a PowerPoint 97 file stores **twice** - recovered exactly from the other copy. The other is in the PowerPoint 95 compatibility copy of the deck, over a stretch where the flux bands have collapsed |
| LGTC0    | 2880 | 2 | 2 | **1** | sector 12 on tracks 9-10, inside `IFSMGR.VXD` |
| Sand     | 2916 | 4 | 4 | **3** | sector 10 on tracks 24-27, inside two ZIP archives - and the same entries are archived *twice on the same disk*, so three of them are recovered exactly and proved by the archive's CRC-32 |
| Ron      | 2944 | 22 | **0** | - | every one is on track 80, past the last formatted track: unformatted noise decoded as 16 KB FM sectors. **Not a damaged disk at all** |
| Scott    | 2881 | 1 | 1 | **1** | a menu string table inside a Word temp file; the repair turns `Move( fro?t )` into `Move( front )` and `Gut Info` into `Get Info` |
| SLAT     | 2916 | 11 | 11 | 0 | every one inside `TRAVEL~1.ZIP`, a Palm "TravelPal" package, including its central directory. 6 of its 9 members still extract; `TESTPL~1.XLS` is intact |
| SLAW     | 2880 | 2 | **0** | - | both in **free space**; all 8 documents intact |
| Teres    | 2880 | 4 | 3 | 0 | one in free space; three in Quicken's `QDATA.QDB`/`.QSD`/`.QEL`, which carry no checksum and have no second copy. The other 7 files are intact |
| DisComp  | 2881 | 0 | - | - | **a Macintosh HFS disk** (volume "Gradebook"), not a PC disk - which is why nothing could read it as one. No CRC errors at all |
| **total** | **29417** | **60** | **28** | **8** | |

Six of the twelve disks - Disk 1, GasAcc, Scott, Ron, SLAW and DisComp
- lost nothing at all. That is not visible from the CRC; it is visible
from the filesystem.

And the count above still flatters the damage. `scan --fs` now surveys
each file rather than each sector, because a bad sector inside an
archive does not cost you the archive:

```
the files on this disk
  TESTPL~1.XLS      53760 byte(s)  no bad sector - intact
  TRAVEL~1.ZIP      87514 byte(s)  damaged - 6 of 9 archive member(s) still extract
```

Eleven bad sectors on SLAT, and two thirds of what is in them comes out
anyway - because a ZIP's members are checked one at a time, by their own
local headers, rather than through a central directory that a mark near
the outside of the disk has already eaten.

## Where the recovered bytes came from

| disk | sector | how | proof |
|---|---|---|---|
| Disk 2 | `0/0 s15` | the other copy of the FAT | its CRC-16 is `7D26`, the value stored on the damaged sector |
| Disk 2 | `0/0 s16`, `0/0 s18` | the other copy of the FAT | redundancy only - the damage took the stored CRC with it |
| Sand | `24/1 s10` | `slsdtai5.rep`, archived twice on this disk | inflates, CRC-32 `6C6F80FC` |
| Sand | `25/1 s10` | `faxcover.adt`, archived twice | inflates, CRC-32 `3C4E1290` |
| Sand | `26/1 s10` | `faxcover.tpl`, archived twice | inflates, CRC-32 `C486A5AA` |
| Zeus | `9/0 s9` | the same stream, stored twice inside the same `.ppt` | the twin matches for 8581 bytes either side of the damage, and with its 512 in place the preview metafile's 727 records tile exactly |
| Disk 2, LGTC0, Scott | 4 sectors | the search, applied on a clear margin | libhxcfe re-decode reads them clean |

After those three, `CONTAC~1.ZIP` verifies **101 of 101** entries -
every file in it inflates and matches its recorded CRC-32. The archive
is whole.

Sand's fourth, `27/1 s10`, is inside `Contacts.adx` in the *other*
archive, and that one's counterpart is a different version of the file,
so there is nothing to copy from. The search finds **245**
CRC-valid readings there; the referee refutes every one of them. That
verdict is worth more than a guess would have been.

## The pattern that keeps recurring

Six of the eight disks fail as *the same sector id on consecutive
tracks*:

```
Disk 1    sector 6  on tracks 72-73
Disk 2    sector 17 on tracks 41,42,43,44,49  (side 1)
Zeus      sector 9  on tracks 8-9             (side 0)
LGTC0     sector 12 on tracks 9-10            (side 0)
Sand      sector 10 on tracks 24-27           (side 1)
Ron       sector 255 across track 80 both sides (unformatted)
```

A sector id is an angular position, and consecutive tracks are radially
adjacent, so this is one physical mark on the medium crossing several
tracks - a scratch, a contact transfer, a speck. It is not a coincidence
of six disks: it is what floppy damage *is*. Isolated single-bit errors
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

And the sister archives settle the argument, because they say exactly
which bytes were wrong rather than which the timings suspected:

```
24/1 s10   bytes 414-511 wrong
25/1 s10   bytes 408-511 wrong
26/1 s10   bytes 403-511 wrong
```

Every one runs to the *end* of the sector. That is why the stored CRC is
never any use on these: the mark eats it on the way past.

`scan` says so, because it changes what the reader should do: a mark
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
   All errors here with a verifiable truth are of this kind, and none is
   a spurious extra reversal. Half of them are *invisible*: a lost
   reversal that runs two 2T intervals into one 4T is legal MFM, so the
   timings have nothing to object to.
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
   target that was never on the disk. Measured on these disks, this is
   not an edge case: it is what happens whenever the mark runs off the
   end, which is most of the time.
6. **Nothing at all** - 32 of the 60 bad sectors here hold no file's
   data. Six disks of the twelve were whole the whole time.

## The CRC can be wrong without the flux noticing

Zeus's `9/0 s9` is the clearest case on any of these disks, because for
once there is an answer to check against. The recovered bytes are not in
doubt: the same thumbnail is stored twice in the file, the two copies
agree on all 8581 bytes outside the damaged sector, and with the twin's
512 bytes in place the metafile's 727 records tile it exactly to the
last byte.

Those bytes give a sector CRC-16 of `239E`. The disk has `473C` stored.
Six of those sixteen bits are wrong - and the flux model put the
expected number of bad CRC bits at **0.006**.

That is a real blind spot, and it is specific to band collapse. The
model scores a reversal by how far it sits from the boundary between
bins; when a whole stretch of the track has its bands pulled together,
every interval lands comfortably near a bin centre and scores as
certain, while being wrong. Margin measures *ambiguity*, not *bias*, and
a collapsed band is pure bias.

So `crc_suspect` catches the common case - a mark that runs off the end
of the sector and visibly chews the last bytes - and misses this one.
Where the damage is a collapse rather than a dropout, nothing in the
timings will tell you the checksum is a fiction; only something above
the sector can.
