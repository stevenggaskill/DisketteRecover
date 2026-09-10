#!/usr/bin/env python3
"""Add realistic transition-position jitter to a SuperCard Pro flux dump.

Real drives do not place reversals exactly on the cell grid: mechanical
speed variation, media noise and head/amplifier bandwidth move each
transition a little. This tool reproduces that by displacing every
transition by a Gaussian amount and letting the neighbouring intervals
absorb it, so the track's total length is unchanged and only the local
timing becomes ambiguous - which is exactly the signal DisketteRecover
bins on.

A single transition can also be displaced deliberately with --shift, to
plant the kind of defect that actually breaks a sector: a reversal that
lands close to a cell boundary, so the PLL has to guess and the decoded
bit stream comes out one bit wrong.

  usage: scp_jitter.py IN.scp OUT.scp [--sigma NS] [--seed N]
                       [--shift TRACK:INDEX:CELLS]
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
    ap.add_argument("--sigma", type=float, default=0.0,
                    help="transition position jitter, in nanoseconds")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--shift", default=None, metavar="TRACK:INDEX:CELLS",
                    help="displace one transition by CELLS cell periods")
    ap.add_argument("--cell-ns", type=float, default=2000.0,
                    help="cell period for --shift (default 2us, MFM DD)")
    args = ap.parse_args()

    src, dst = args.src, args.dst
    sigma_ns, seed = args.sigma, args.seed

    shift_track = shift_index = None
    shift_cells = 0.0
    if args.shift:
        parts = args.shift.split(":")
        if len(parts) != 3:
            sys.exit("--shift wants TRACK:INDEX:CELLS")
        shift_track = int(parts[0])
        shift_index = int(parts[1])
        shift_cells = float(parts[2])

    buf = bytearray(open(src, "rb").read())
    if buf[:3] != b"SCP":
        sys.exit("not an SCP file")

    resolution = buf[0x0B]
    tick_ns = 25.0 * (resolution + 1)
    sigma = sigma_ns / tick_ns
    rng = random.Random(seed)

    offsets = struct.unpack_from("<168I", buf, 0x10)
    out = bytearray(buf)
    jittered = 0
    shifted = False

    for tno, toff in enumerate(offsets):
        if toff == 0 or toff + 4 > len(buf) or buf[toff:toff + 3] != b"TRK":
            continue
        # Revolution table length is not stored; walk it until an entry
        # points outside the file or back into the header.
        revs = []
        p = toff + 4
        while p + 12 <= len(buf):
            idx_time, length, doff = struct.unpack_from("<III", buf, p)
            if length == 0 or doff < 4 or toff + doff + 2 * length > len(buf):
                break
            revs.append((length, toff + doff))
            p += 12
            if len(revs) >= 5:
                break

        for rno, (length, doff) in enumerate(revs):
            vals, end = read_flux(buf, doff, length)
            span = end - doff

            if sigma > 0.0:
                prev = 0.0
                for i in range(len(vals)):
                    d = rng.gauss(0.0, sigma)
                    vals[i] = max(1, int(round(vals[i] + d - prev)))
                    prev = d

            # Move one transition towards a cell boundary. The next
            # interval absorbs the move, so nothing downstream shifts.
            if (shift_track == tno and shift_index is not None
                    and 0 < shift_index < len(vals) - 1):
                d = int(round(shift_cells * args.cell_ns / tick_ns))
                vals[shift_index] = max(1, vals[shift_index] + d)
                vals[shift_index + 1] = max(1, vals[shift_index + 1] - d)
                shifted = True

            enc = write_flux(vals)
            if len(enc) != span:          # keep every offset valid
                continue
            out[doff:doff + span] = enc
            jittered += 1

    chk = sum(out[0x10:]) & 0xFFFFFFFF
    struct.pack_into("<I", out, 0x0C, chk)
    open(dst, "wb").write(bytes(out))
    print("rewrote %d revolution(s), sigma %.0f ns (%.2f ticks)%s -> %s"
          % (jittered, sigma_ns, sigma,
             ", shifted one transition by %+.2f cell" % shift_cells
             if shifted else "", dst))


if __name__ == "__main__":
    main()
