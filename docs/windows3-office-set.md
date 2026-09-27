# The Windows 3 Office Set

A second collection, kept separately from [the first twenty
disks](disks.md). Nine KryoFlux dumps of retail Windows 3.x
application media, five revolutions per track, arriving two zips per
disk - tracks 0-40 in the `a` half and 41-83 in the `b`.

The goal is the same as before: make the disks viable. That a copy of
some of this software exists elsewhere in the world is a welcome
referee when one turns up, and no part of the exercise.

| disk | sectors | bad | of those, in a file | file sectors recovered | what was wrong |
|---|---:|---:|---:|---:|---|
| ClarisWorks Disk1 | 2880 | 1 | **0** | - | one FAT sector, and its twin on the same disk is clean. Restored outright; all 37 files intact |
| MSVBPro Disk2 | 2880 | 0 | - | - | **no CRC errors at all.** One file, `WIN31WH.H9_`, 1.4 MB |
| MSWord6p0 Disk2 | 3360 | 3 | 3 | 0 | a 1.68 MB disk holding one file: `WORD2.CAB`, the second cabinet of Word 6.0's install set. See below - this is the disk that taught the tool the most |
| WinFaxLite Disk1 | 2880 | 4 | **0** | - | sector 15 across tracks 64-67, all of it in **free space**. Nothing lost |
| XTree4Win Disk1 | 2881 | 1 | **0** | - | track 80, past the last formatted track. Not damage at all |
| XTree4Win Disk2 | 2881 | 1 | **0** | - | the same |
| XTree4Win Disk3 | 2881 | 3 | **0** | - | two in free space, one on track 80. Nothing lost |
| XTree4WinX Disk1 | 2881 | 1 | **0** | - | track 80 again |
| XTree4WinX Disk2 | 2881 | 15 | 14 | **6** | the damaged one. Four radial marks; eleven files hit. Three files recovered *exactly* from the other disks in the box - see below |
| **total** | **25405** | **29** | **17** | **6** | |

Seven of the nine disks lost nothing: ClarisWorks and MSVBPro are
whole, and on WinFaxLite and all three XTree4Win disks every bad sector
is free space or unformatted track.

All five XTree disks - both editions - carry the same oddity: one bad
128-byte FM sector at track 80, side 0, sector 1, past the end of the
filesystem. Five disks from two different products failing in the same
place is not five failures; it is a signature of the run that
duplicated them.

## Software did not arrive on one floppy

The useful thing about a box of disks is that it is a box. An install
set shipped the same runtime on several of its disks, and two editions
of the same product shipped much of the same code, so a file one disk
has lost is often sitting intact on the disk beside it. Nothing about
that is visible from inside a single dump, which is why the tool could
not see it before.

`--from-dir` points it at the rest of the set. Every file under the
directory is offered to the same anchoring test `--from-file` uses -
find the damaged sector's known-good neighbours inside the candidate,
then measure how far the agreement runs either side - so the match is
made on bytes, not names. A renamed copy is found; a same-named file
from a different build is rejected.

On XTree4WinX's disk 2, with the other eight disks extracted into a
library of 337 files:

```
  7/0 s1    from set  the same bytes from WINHELP.EX_  -> APPLIED, proven not ranked
  8/0 s1    from set  the same bytes from WINHELP.EX_  -> APPLIED, proven not ranked
  9/0 s1    from set  the same bytes from WINHELP.HL_  -> APPLIED, proven not ranked
 22/0 s5    from set  the same bytes from XTW519F.DL_  -> APPLIED, proven not ranked
```

`WINHELP.EX_` and `WINHELP.HL_` came off **ClarisWorks' disk 1** - a
different product entirely, shipping the same Windows 3.1 help viewer.
`XTW519F.DL_` came off XTree4Win's disk 3, the other edition. All three
files are now byte-identical to those copies across their whole length:
147,911, 20,991 and 20,690 bytes with nothing left over.

Two guards matter here. The first is that a candidate must not simply
hand the damage back - and the disk's own copy of the file, sitting in
the same library as its neighbours', is exactly such a candidate. It
agrees with itself for the whole length of the file either side of the
damage, which looks like the strongest match there could possibly be.
The search now compares what each candidate proposes against what the
disk already reads there and skips it if they are the same.

The second is where the agreement has to reach. The old rule asked for
4,096 bytes on each side, which is the wrong test near the ends of a
file: damage 1,024 bytes in cannot have 4,096 bytes before it however
perfect the copy. It now also counts a side as satisfied when the run
reaches the file's own edge. `WINHELP.HL_` needed precisely that -
1,024 bytes before, all there are, and 19,455 after.

## A cabinet checks its own blocks

Word 6.0 shipped as `WORD1.CAB`..`WORDn.CAB`, one cabinet per floppy.
A CAB holds its data in blocks of up to 32 KB and every block carries a
**32-bit checksum of its own compressed bytes** - four times the sector
CRC, and it says exactly which block the damage is in.

On `WORD2.CAB`, 95 of its 99 blocks check out. The three that fail are
blocks 15, 16 and 17, and the three bad sectors on the disk fall
exactly inside them. (The 99th stores a zero checksum, which means the
writer stored none.)

That is a referee, and the first thing it did was overrule the tool:

```
 12/1 s18   bits   256 reading(s), top 251 x next  -> not applied (the file above it refutes this reading)
```

Before the cabinet was understood, that sector was **repaired and
reported clean**. A reading 251 times likelier than its nearest rival,
matching the sector's stored sixteen bits, re-decoding without error -
and wrong. The cabinet's 32 bits say so, and the tally for this disk
went from "1 of 3 repaired" to "0 of 3", which is the honest number.

It is worth being plain about what that means for the rest of this
project. Sixteen bits cannot arbitrate 512 bytes; that has been the
premise from the start. This is the first case where a stronger check
was available and disagreed, and it disagreed with a repair that looked
as safe as they come.

The other two sectors are beyond reach: the checksum deltas have
popcounts of 18, 10 and 19, so the damage is many bits and not a flip
or two, and every one of the 245 and 256 CRC-valid readings is refuted.
Word 6.0's cabinets use Quantum compression, so there is no
decompressor here to salvage a prefix with either. `WORD2.CAB` is
short three blocks of `WINWORD.EXE`, and this tool cannot supply them -
`WORD2.CAB` from another copy of the same release could.

## What is still missing

| file | disk | what is left |
|---|---|---|
| `WORD2.CAB` | MSWord6p0 Disk2 | 3 blocks of 99, inside `WINWORD.EXE` |
| `XTW601F.DL_` | XTree4WinX Disk2 | 1 sector; the disk-3 copy is a different build |
| `XTW605F.DL_` | XTree4WinX Disk2 | 1 sector; same |
| `XTWGCFS.DL_` | XTree4WinX Disk2 | 2 sectors; no copy anywhere in the set |
| `XTWGTSY.DL_`, `XTWHDWN.DL_`, `XTWHTWN.DL_`, `XTWHVWN.DL_` | XTree4WinX Disk2 | 1 sector each; no copy in the set |

Every one of those is a KWAJ-compressed file - 308 of the set's 337
files are - and KWAJ method 3 stores no uncompressed length and no
checksum, so the format offers nothing to check a reading against. A
decoder for its LZH stream would be a referee, because a wrong byte
makes Huffman decoding fail; that is the obvious next thing to build
for media of this vintage.
