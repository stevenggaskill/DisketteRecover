#!/usr/bin/env python3
"""Build a 720 kB FAT12 floppy image holding the given files.

The filesystem tests need a disk with real structure on it - a FAT with
its second copy, a directory, and a file whose format carries a checksum
of its own. This writes one, so the tests do not depend on any image
that happens to be lying around.

usage: mkfat.py OUT.img NAME=PATH [NAME=PATH ...]
"""
import struct
import sys

BPS, SPC, RES, NFAT, RDE, TOT, FATSZ, SPT, HEADS = \
    512, 2, 1, 2, 112, 1440, 3, 9, 2
ROOT = RES + NFAT * FATSZ
DATA = ROOT + (RDE * 32 + BPS - 1) // BPS
CLUSTER = SPC * BPS


def build(names):
    img = bytearray(TOT * BPS)

    boot = bytearray(BPS)
    boot[0:3] = b'\xEB\x3C\x90'
    boot[3:11] = b'DRTEST  '
    struct.pack_into('<HBHBHHBHHH', boot, 11, BPS, SPC, RES, NFAT, RDE,
                     TOT, 0xF9, FATSZ, SPT, HEADS)
    boot[510:512] = b'\x55\xAA'
    img[0:BPS] = boot

    fat = bytearray(FATSZ * BPS)
    fat[0:3] = b'\xF9\xFF\xFF'

    def set_entry(n, v):
        i = (n * 3) // 2
        if n & 1:
            fat[i] = (fat[i] & 0x0F) | ((v << 4) & 0xF0)
            fat[i + 1] = (v >> 4) & 0xFF
        else:
            fat[i] = v & 0xFF
            fat[i + 1] = (fat[i + 1] & 0xF0) | ((v >> 8) & 0x0F)

    root = bytearray(RDE * 32)
    cl = 2
    for slot, (name, data) in enumerate(names):
        first = cl
        n = max(1, (len(data) + CLUSTER - 1) // CLUSTER)
        for k in range(n):
            off = (DATA + (cl - 2) * SPC) * BPS
            img[off:off + CLUSTER] = \
                data[k * CLUSTER:(k + 1) * CLUSTER].ljust(CLUSTER, b'\0')
            set_entry(cl, 0xFFF if k == n - 1 else cl + 1)
            cl += 1
        base, _, ext = name.partition('.')
        e = root[slot * 32:slot * 32 + 32]
        e[0:11] = (base[:8].ljust(8) + ext[:3].ljust(3)).upper().encode()
        e[11] = 0x20
        struct.pack_into('<H', e, 26, first)
        struct.pack_into('<I', e, 28, len(data))
        root[slot * 32:slot * 32 + 32] = e

    for k in range(NFAT):
        off = (RES + k * FATSZ) * BPS
        img[off:off + len(fat)] = fat
    img[ROOT * BPS:ROOT * BPS + len(root)] = root
    return img


def main():
    out = sys.argv[1]
    files = []
    for arg in sys.argv[2:]:
        name, _, path = arg.partition('=')
        files.append((name, open(path, 'rb').read()))
    open(out, 'wb').write(build(files))


if __name__ == '__main__':
    main()
