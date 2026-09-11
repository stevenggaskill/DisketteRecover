#!/usr/bin/env python3
"""Build a minimal OLE compound document that keeps a stream twice.

A PowerPoint 97 file saved for backwards compatibility holds the same
preview thumbnail in two places: once live, once inside the storage
carrying the PowerPoint 95 copy. That is the redundancy the recovery
uses, so the tests need a file shaped the same way - a property set with
a metafile thumbnail, stored once at the root and once under a storage.

usage: mkcfb.py OUT.bin [RECORDS]
"""
import struct
import sys

SSZ = 512
FREE, END = -1, -2


def metafile(nrec):
    """A Windows metafile whose records tile it exactly."""
    recs = []
    for i in range(nrec):
        words = 3 + (i % 7)                      # size incl. the header
        params = bytes(((i + j) & 0xFF) for j in range((words - 3) * 2))
        recs.append(struct.pack('<IH', words, 0x0201 + (i % 5)) + params)
    recs.append(struct.pack('<IH', 3, 0))        # the terminator
    body = b''.join(recs)
    total = (18 + len(body)) // 2
    head = struct.pack('<HHHIHIH', 1, 9, 0x0300, total, 4, 64, 0)
    return head + body


def propset(wmf):
    """A property set whose only property is the thumbnail."""
    value = struct.pack('<II', len(wmf) + 12, 0xFFFFFFFF) + \
        b'\x03\x00\x00\x00' + struct.pack('<II', 0, 0) + wmf
    value = struct.pack('<I', 0x47) + value
    props = struct.pack('<II', 0x11, 8 + 8)      # one (id, offset) pair
    section = struct.pack('<II', 8 + 8 + len(value), 1) + props + value
    head = struct.pack('<HHIII', 0xFFFE, 0, 0, 0, 1) + b'\0' * 16
    head += struct.pack('<I', len(head) + 4)
    return head + section


def dirent(name, typ, left, right, child, start, size):
    e = bytearray(128)
    nm = name.encode('utf-16-le') + b'\0\0'
    e[0:len(nm)] = nm
    struct.pack_into('<H', e, 0x40, len(nm))
    e[0x42] = typ
    e[0x43] = 1
    struct.pack_into('<iii', e, 0x44, left, right, child)
    struct.pack_into('<i', e, 0x74, start)
    struct.pack_into('<I', e, 0x78, size)
    return bytes(e)


def build(nrec):
    stream = propset(metafile(nrec))
    n = (len(stream) + SSZ - 1) // SSZ

    # sector 0: FAT, sector 1: directory, then the two copies
    a0, b0 = 2, 2 + n
    total = 2 + 2 * n

    fat = [END] * (SSZ // 4)
    fat[0] = -3                                  # FATSECT
    fat[1] = END
    for k in range(n):
        fat[a0 + k] = a0 + k + 1 if k < n - 1 else END
        fat[b0 + k] = b0 + k + 1 if k < n - 1 else END
    for k in range(total, SSZ // 4):
        fat[k] = FREE

    d = bytearray()
    d += dirent('Root Entry', 5, FREE, FREE, 1, END, 0)
    d += dirent('\x05SummaryInformation', 2, FREE, 2, FREE, a0, len(stream))
    d += dirent('DUALSTORAGE', 1, FREE, FREE, 3, END, 0)
    d += dirent('\x05SummaryInformation', 2, FREE, FREE, FREE, b0, len(stream))
    d += bytes(SSZ - len(d) % SSZ if len(d) % SSZ else 0)

    head = bytearray(SSZ)
    head[0:8] = bytes([0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1])
    struct.pack_into('<HH', head, 0x18, 0x3E, 3)
    struct.pack_into('<HHH', head, 0x1C, 0xFFFE, 9, 6)
    struct.pack_into('<III', head, 0x28, 0, 1, 1)
    struct.pack_into('<I', head, 0x38, 4096)
    struct.pack_into('<iI', head, 0x3C, END, 0)
    struct.pack_into('<iI', head, 0x44, END, 0)
    struct.pack_into('<i', head, 0x4C, 0)
    for k in range(1, 109):
        struct.pack_into('<i', head, 0x4C + k * 4, FREE)

    img = bytearray(head)
    img += b''.join(struct.pack('<i', x) for x in fat)
    img += d[:SSZ]
    body = stream.ljust(n * SSZ, b'\0')
    img += body + body
    return bytes(img)


def main():
    nrec = int(sys.argv[2]) if len(sys.argv) > 2 else 700
    open(sys.argv[1], 'wb').write(build(nrec))


if __name__ == '__main__':
    main()
