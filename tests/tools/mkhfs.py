#!/usr/bin/env python3
"""Build a minimal Macintosh HFS volume.

Only the parts the tool actually reads: the master directory block and
the volume bitmap. The catalogue is a B-tree and is not walked, so
there is nothing here to build it from - what is tested is the question
that turned out to matter most on a damaged disk, which is whether
anything is stored in a block at all.

usage: mkhfs.py OUT.img [USED_BLOCKS]
"""
import struct
import sys

SECS = 1440
ABSIZE = 512
NALBLKS = 1000
VBMST = 3
ALBST = 4


def build(used):
    img = bytearray(SECS * 512)

    mdb = bytearray(512)
    struct.pack_into('>H', mdb, 0x00, 0x4244)          # 'BD'
    struct.pack_into('>H', mdb, 0x0C, 3)               # files in the root
    struct.pack_into('>H', mdb, 0x0E, VBMST)
    struct.pack_into('>H', mdb, 0x12, NALBLKS)
    struct.pack_into('>I', mdb, 0x14, ABSIZE)
    struct.pack_into('>H', mdb, 0x1C, ALBST)
    struct.pack_into('>H', mdb, 0x22, NALBLKS - used)  # free blocks
    name = b'TESTVOL'
    mdb[0x24] = len(name)
    mdb[0x25:0x25 + len(name)] = name
    struct.pack_into('>I', mdb, 0x54, 3)               # files
    struct.pack_into('>I', mdb, 0x58, 1)               # folders
    img[2 * 512:3 * 512] = mdb

    bits = bytearray(512)
    for b in range(used):
        bits[b // 8] |= 0x80 >> (b & 7)
    img[VBMST * 512:(VBMST + 1) * 512] = bits

    # something recognisable in the used blocks, so the image is not
    # one long run of zeroes
    for b in range(used):
        off = (ALBST + b) * 512
        img[off:off + 512] = bytes(((b + i) & 0xFF) for i in range(512))
    return bytes(img)


def main():
    used = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    open(sys.argv[1], 'wb').write(build(used))


if __name__ == '__main__':
    main()
