#!/usr/bin/env python3
"""Turn a one-revolution SuperCard Pro dump into a multi-pass one.

A real flux dump holds the same track read several times round, and the
whole point of that is that the passes are not identical: the reversals
are in the same place on the medium, but each pass measures them with its
own noise, and where the medium is weak a reversal may be detected on one
pass and missed on the next.

This reproduces both, so the revolutions engine has something to chew on:

  --revs N       write N passes instead of one
  --sigma NS     independent read jitter per pass, in nanoseconds
  --fuzzy T:I:K  make the reversal at index I of track T weak - only K of
                 the N passes see it, the rest run the two intervals it
                 separates together (repeatable)

  usage: scp_revs.py IN.scp OUT.scp [--revs N] [--sigma NS] [--seed S]
                     [--fuzzy TRACK:INDEX:SEEN]
"""
import argparse
import random
import struct
import sys


def read_flux(buf, off, count):
    vals, i, acc = [], off, 0
    while len(vals) < count:
        v = struct.unpack_from(">H", buf, i)[0]
        i += 2
        if v == 0:
            acc += 0x10000
            continue
        vals.append(acc + v)
        acc = 0
    return vals, i


def write_flux(vals):
    out = bytearray()
    for v in vals:
        while v > 0xFFFF:
            out += b"\x00\x00"
            v -= 0x10000
        out += struct.pack(">H", max(1, v))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--revs", type=int, default=5)
    ap.add_argument("--sigma", type=float, default=40.0,
                    help="per-pass read jitter, in nanoseconds")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--fuzzy", action="append", default=[],
                    metavar="TRACK:INDEX:SEEN")
    ap.add_argument("--tracks", type=int, default=0,
                    help="keep only the first N tracks (0 = all). Five "
                         "passes over a whole disk is five times the "
                         "file, and a test that only looks at track 0 "
                         "has no use for the rest.")
    args = ap.parse_args()

    buf = bytearray(open(args.src, "rb").read())
    if buf[:3] != b"SCP":
        sys.exit("not an SCP file")

    tick_ns = 25.0 * (buf[0x0B] + 1)
    sigma = args.sigma / tick_ns
    rng = random.Random(args.seed)

    fuzzy = {}
    for f in args.fuzzy:
        t, i, k = (int(x) for x in f.split(":"))
        fuzzy.setdefault(t, []).append((i, k))

    offsets = list(struct.unpack_from("<168I", buf, 0x10))
    header = bytearray(buf[:0x10 + 168 * 4])
    header[0x05] = args.revs          # number_of_revolution, not 0x0A
    body = bytearray()
    base = len(header)
    newoff = [0] * 168

    for tno, toff in enumerate(offsets):
        if args.tracks and tno >= args.tracks:
            break
        if toff == 0 or toff + 4 > len(buf) or buf[toff:toff + 3] != b"TRK":
            continue
        idx_time, length, doff = struct.unpack_from("<III", buf, toff + 4)
        if length == 0:
            continue
        vals, _ = read_flux(buf, toff + doff, length)

        newoff[tno] = base + len(body)
        thead = bytearray(b"TRK" + bytes([tno]))
        data = bytearray()
        entries = []

        for r in range(args.revs):
            v = list(vals)

            # Weak reversals: only `seen` of the passes detect them. A
            # missed reversal does not shorten the track - the interval
            # simply runs on into the next one.
            for idx, seen in fuzzy.get(tno, []):
                if r >= seen and 0 < idx < len(v) - 1:
                    v[idx + 1] += v[idx]
                    v[idx] = 0

            # Independent read noise on each pass, applied to transition
            # positions so it cancels between neighbouring intervals.
            if sigma > 0.0:
                prev = 0.0
                for i in range(len(v)):
                    if v[i] == 0:
                        continue
                    d = rng.gauss(0.0, sigma)
                    v[i] = max(1, int(round(v[i] + d - prev)))
                    prev = d

            v = [x for x in v if x > 0]
            enc = write_flux(v)
            entries.append((idx_time, len(v)))
            data += enc

        # The revolution table sits between the TRK magic and the flux,
        # so every data offset is relative to the start of the header.
        tbl_len = 12 * args.revs
        off = 4 + tbl_len
        tbl = bytearray()
        for idx_time, n in entries:
            tbl += struct.pack("<III", idx_time, n, off)
            off += 2 * n
        body += thead + tbl + data

    out = bytearray(header) + body
    struct.pack_into("<168I", out, 0x10, *newoff)
    open(args.dst, "wb").write(out)
    print("wrote %s: %d revolution(s) per track" % (args.dst, args.revs))


if __name__ == "__main__":
    main()
