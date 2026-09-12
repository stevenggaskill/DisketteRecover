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
| LGTC0    | 2880 | 2 | 2 | **2** | sector 12 on tracks 9-10, inside `IFSMGR.VXD`. **Fully recovered** from a copy of the same Windows Me build supplied from outside: the repaired file is byte-identical to it across all 185,910 bytes |
| Sand     | 2916 | 4 | 4 | **3** | sector 10 on tracks 24-27, inside two ZIP archives - and the same entries are archived *twice on the same disk*, so three of them are recovered exactly and proved by the archive's CRC-32 |
| Ron      | 2944 | 22 | **0** | - | every one is on track 80, past the last formatted track: unformatted noise decoded as 16 KB FM sectors. **Not a damaged disk at all** |
| Scott    | 2881 | 1 | 1 | **1** | a menu string table inside a Word temp file; the repair turns `Move( fro?t )` into `Move( front )` and `Gut Info` into `Get Info` |
| SLAT     | 2916 | 11 | 3 | **1** | every bad sector is inside `TRAVEL~1.ZIP`, a Palm "TravelPal" package - but eight of the eleven land in 38 KB of the file that no member occupies, left over from an earlier version of the archive. 8 of 11 members extract, one of them recovered through the *stale* copy of the archive's own directory that the same leftover contains. Exactly one file, `TransportImg.gif`, is lost to the disk damage; `TESTPL~1.XLS` is intact |
| SLAW     | 2880 | 2 | **0** | - | both in **free space**; all 8 documents intact |
| Teres    | 2880 | 4 | 3 | 0 | one in free space; three in Quicken's `QDATA.QDB`/`.QSD`/`.QEL`. Uncompressed and structured - the QDB's 56-byte record framing locates the damage to the last 124 bytes of its sector and refutes all 561 re-readings that satisfy the CRC. The other 7 files are intact |
| DisComp  | 2881 | 0 | - | - | **a Macintosh HFS disk** (volume "Gradebook"), not a PC disk - which is why nothing could read it as one. No CRC errors at all |
| ALXPPT   | 2944 | 20 | 10 | 0 | one mark down sector 12, tracks 32-45. The damaged FAT sector was restored from its twin; 7 of the rest are in free space. Its root directory also still holds four **deleted** entries, and three of those - `RPN.PRC`, `TEALLOCK.PRC` and RPN's scripts database - come back whole, names and all. The live filesystem is confused independently of the damage: `RESUME.TXT`'s recorded start cluster is three clusters past where its text actually begins |
| **total** | **32361** | **80** | **30** | **10** | |

Six of the thirteen disks - Disk 1, GasAcc, Scott, Ron, SLAW and
DisComp - lost nothing at all, and LGTC0 joins them once the outside
copy of its driver is applied. That is not visible from the CRC; it is visible
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
| LGTC0 | `9/0 s12`, `10/0 s12` | the same build of the file, from outside the disk | anchored on the sector's clean neighbours: 65,536 bytes of agreement before and 17,958 / 61,494 after, and the repaired file then matches the known-good copy byte for byte |
| Zeus | `9/0 s9` | the same stream, stored twice inside the same `.ppt` | the twin matches for 8581 bytes either side of the damage, and with its 512 in place the preview metafile's 727 records tile exactly |
| SLAT | `RotatingGlobeAnimation.gif` | the archive stored the same animation twice, under two names | the stale copy of the archive's own central directory is the only thing that knows the second name; its packed bytes inflate to CRC-32 `E16CA998` |
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
6. **Nothing at all** - 50 of the 80 bad sectors here hold no file's
   data: free space, unformatted noise past the last track, or - on
   SLAT - parts of a file that the file itself no longer uses. Six disks
   of the thirteen were whole the whole time, and LGTC0 is whole now.

## What a deleted file still tells you

Erasing a file on a FAT disk overwrites one byte - the first character of
its name - and frees its clusters. Everything else stays: the length,
where it started, and, until something else claims them, the clusters
themselves. ALXPPT's root directory still holds four such entries, and
three of them are PalmOS databases whose data is untouched.

A `.prc` or `.pdb` describes itself, which makes the recovery checkable
rather than hopeful:

```
RPN.PRC        36099 bytes  'RPN'             appl/Yrpn  12 resources
RPN-scripts.PDB 2277 bytes  'Yrpn_R_WScripts' r000/Yrpn   8 records
TEALLOCK.PRC   26056 bytes  'TealLock'        appl/TlLk  38 resources
```

In every one the offsets rise, land inside the file, and the last record
ends *exactly* on the last byte - so the directory's recorded length and
the contiguous run of clusters both check out, at once.

And the name inside puts back the letter the directory lost, where the
two agree on everything else: `?PN.PRC` is `RPN.PRC` and `?EALLOCK.PRC`
is `TEALLOCK.PRC`, and are not guesses. The third file's internal name is
`Yrpn_R_WScripts`, which does not agree with what is left of its DOS
name, so it keeps its question mark. Its *creator* is `Yrpn`, the same as
the application, which is how a person can tell it is RPN's scripts
database - but that is an inference, and the tool does not make it.

One of the three carries damage: 512 bytes of `RPN.PRC` fall in a bad
sector, inside its `code#1` resource. The structure is unaffected, so it
will still install; whether it runs is another matter, and a `.prc`
carries no checksum to settle it.

## The QuickBooks backups, and why they stay broken

Disk 2's three remaining sectors are in `FHP2_97.QBB` and `FHP5_97.QBB`,
and the disk carries **six** monthly backups of the same company file -
`FHP.QBB`, `FHP12_96`, `FHP2_97`, `FHP5_97`, `FHP7_97`, `FHP8_97`. Six
copies of one accounting database, four of them undamaged. It looks like
the sister-archive case on Sand all over again.

It is not, and the reason is worth recording. The QBB body is
compressed: 7.90 bits per byte, all 256 values present. Compression
destroys correspondence - the same ledger entry in February and in
August comes out as different bytes in different places - so what the
backups share is only where the compressor happened to re-synchronise.
Across the whole pair, `FHP2_97` and `FHP8_97` share a 4,416-byte run
and `FHP12_96` a 1,482-byte one, but nothing anywhere near the damage.
Anchoring each damaged sector's clean neighbours against all five
siblings finds **no alignment at all**:

```
FHP8_97.QBB does not line up here - the best match agrees for only
0 byte(s) before and 0 after
```

Nor is there a checksum to referee with. The container is three records
marked `45 86`, ending at byte 82, and they do carry one real invariant
- a 32-bit field that equals the file length minus 82, which would catch
a damaged header. All three damaged sectors are in the body.

So the honest answer is what is left in doubt, and that is now measured
rather than guessed:

```
43/1 s17   492 of 512 data bytes settled to 99%; 20 remain open
49/1 s17   507 of 512 data bytes settled to 99%;  5 remain open
41/1 s17   512 of 512 - but every reading matches a stored CRC that is
           itself in doubt, so this is agreement about a checksum
```

Twenty bytes and five bytes. In an uncompressed file that would be a
small loss; inside a compressed stream it is the end of it from that
point on. The backups either side of each - four of the six - are whole.

## Teres's Quicken files, and what the records say

Teres's three remaining sectors are in `QDATA.QDB`, `QDATA.QSD` and
`QDATA.QEL`. Unlike Disk 2's QuickBooks backups these are **not**
compressed - 3.47, 1.88 and 0.18 bits per byte - so there is structure
to work with, and the QDB has a great deal of it: 4,821 records of 56
bytes, every one carrying the same marker.

Measured from the clean parts of the file, that framing says exactly
where the record starts must fall inside the damaged sector, and it
locates the damage precisely:

```
1138228 (+ 52): cdab000000000000  OK
1138284 (+108): cdab000000000000  OK
...
1138564 (+388): c480ffffffffffff  <-- broken
1138620 (+444): 2000ffffffffffff  <-- broken
1138676 (+500): 2000ffffffffffff  <-- broken
```

Six of the nine records in that sector are intact; the damage is the
last 124 bytes. And it is a referee no checksum could be: restoring
three two-byte markers is far beyond any weight-3 bit search, so all 256
CRC-valid readings are refuted, and so are all **561** found by a
40-million-assignment re-binning of the flux. The closest any of them
comes is 7 of 9.

The other two files have no such framing to offer - `QDATA.QEL` is 98.6%
zeros and `QDATA.QSD` is a report definition padded with `41`s - so for
those the account is what is left in doubt:

```
69/1 s9   502 of 512 data bytes settled to 99%; 10 remain open
70/1 s9   505 of 512 settled; 7 open - but the stored CRC is in doubt too
71/1 s9   489 of 512 settled; 23 open - likewise
```

Seven of Teres's ten files are untouched, including `QDATA.QMD` and both
QuickBooks company files.

## What the deleted entries hold, disk by disk

Run over all thirteen, `extract --deleted` finds rather more than the
three PalmOS databases. What matters is not whether an entry reads but
whether a live file has since taken its clusters:

| disk | deleted, and untouched | what it is |
|---|---|---|
| GasAcc | `?RCL-IMI.DOC`, `?WFTMDSF.DOC`, `?YS-REQT.DOC`, `?RM-RQ11.DOC`, `?SDSBAP.DOC` | five Word documents, **693 KB**, all parsing, none overwritten. GasAcc's live filesystem holds one 2 KB file; everything else on the disk is this |
| SLAW | `?LDCON97.PPT` | a 683 KB PowerPoint 4.0 deck (`PP40` stream), untouched |
| Teres | `?ALTDI~1.PPT` | 13.8 KB, a compound document with a `Pictures` stream |
| ALXPPT | `RPN.PRC`, `TEALLOCK.PRC`, `?PN.PDB` | the three PalmOS databases |

And what is *not* recoverable, which the same report makes clear:

| disk | entry | |
|---|---|---|
| ALXPPT | `?LEX.PPT` | 469 of its 1,395 clusters have been given to other files - it parses, but as the *workbook* that took them |
| Ron, Zeus | `?WRD0002.TMP`, `?WRD0000.TMP` | every cluster reused |
| Scott | 83 entries | a Word editing history - the same documents saved over and over, most of them partly overwritten by the next save |
| Sand | 7 × `?ONTACTS.ZIP`, 22 bytes each | 22 bytes is an empty ZIP, just the end-of-directory record. Somebody tried to copy that archive seven times before it took |

A deleted file's chain is gone from the FAT - only the first cluster is
recorded - so the clusters are read consecutively from there. On a disk
written once that is right, and the structural parse is the check on it;
on a churned disk a file may have been fragmented, and then the middle
is wrong even where the header reads.

## The repair that was wrong, and how we know

LGTC0 is the first sector on any of these disks where an outside copy
settled a repair the tool had already made. It did not survive.

`9/0 s12` was repaired in an earlier pass: 166 CRC-valid readings at
weight 3, the top one 102 times likelier than the next - "clear winner"
by every measure the tool had. It was applied and libhxcfe's own decoder
read the sector clean.

The known-good build says the truth was four bytes, five bits:

```
  offset  as read  what was applied   truth
  105517       CB                CB      8B
  105545       82                82      86
  105546       2C                2C      A8
  105613       A4                A0      AC
  105924       00                20      00
```

Two of the three bits it flipped were in bytes that really were damaged
- and it flipped the wrong bit in each. The third it invented. It left
two damaged bytes untouched. And the result satisfied the stored CRC,
because the stored CRC was intact and a sixteen-bit checksum has 65,536
values: at weight 3 over a 4,144-bit message there are billions of
readings and millions of them match.

The true reading is at weight 5. The search stops at the first weight
that yields any CRC-valid candidate, and at weight 3 it always will. So
the rule "take the lowest weight that satisfies the CRC" is only sound
when the true error is at or below that weight, and nothing in the flux
said it was not. The margin of 102x was a ratio between wrong answers.

This is the same lesson as the rest of this file, with a witness: a
sixteen-bit checksum cannot settle a 512-byte sector, and where a disk
carries no stronger check, an outside copy is worth more than any amount
of ranking.

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
