#!/usr/bin/env python3
"""How smoothly chrs move in a --net-dump file (dump with --net-dump-interval 1).

usage: smooth.py dump.csv [dump2.csv ...]

For every living chr, looks at how far it moved between consecutive frames,
per real time (the ms column), and reports:
  speed p50/p99/max  units per 60 Hz tick while moving
  spikes             frames moving faster than 2x that chr's own p90 speed
  reversals          frames where it moved back against the previous frame's
                     direction (the forward-then-snap-back look of a warp)
Run it on the host's dump and a client's: the client's numbers should look
like the host's. A client that guesses ahead and snaps back has many more
spikes and reversals.
"""

import csv
import math
import sys
from collections import defaultdict


def load(path):
    chrs = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["type"] not in ("3", "6") or r["dead"] == "1" or "ms" not in r:
                continue
            chrs[int(r["syncid"])].append((float(r["ms"]), float(r["x"]), float(r["y"]), float(r["z"])))
    return chrs


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def analyse(path):
    chrs = load(path)
    tot_frames = tot_spikes = tot_rev = 0
    tot_path = 0.0
    allspeeds = []
    for sid, s in sorted(chrs.items()):
        s.sort()
        speeds = []
        moves = []
        for a, b in zip(s, s[1:]):
            dt = b[0] - a[0]
            if dt <= 0.5:
                continue
            d = (b[1] - a[1], b[3] - a[3])
            dist = math.hypot(*d)
            if dist > 250:  # respawn / teleport
                moves.append(None)
                continue
            sp = dist / dt * (1000 / 60)
            speeds.append(sp)
            moves.append((d, sp, dist))
        moving = [m for m in moves if m and m[1] > 0.2]
        if len(moving) < 20:
            continue
        p90 = pct([m[1] for m in moving], 0.9)
        spikes = sum(1 for m in moving if m[1] > max(2 * p90, 1.0))
        rev = 0
        for m1, m2 in zip(moves, moves[1:]):
            if not m1 or not m2 or m1[1] < 0.5 or m2[1] < 0.5:
                continue
            (ax, az), (bx, bz) = m1[0], m2[0]
            cos = (ax * bx + az * bz) / (math.hypot(ax, az) * math.hypot(bx, bz))
            if cos < -0.5:
                rev += 1
        tot_frames += len(moving)
        tot_path += sum(m[2] for m in moves if m)
        tot_spikes += spikes
        tot_rev += rev
        allspeeds += [m[1] for m in moving]
    print(f"{path}")
    if not tot_frames:
        print("  no moving chrs")
        return
    print(f"  moving frames {tot_frames}  speed p50 {pct(allspeeds, .5):.2f} p99 {pct(allspeeds, .99):.2f} max {max(allspeeds):.2f}")
    print(f"  distance travelled {tot_path / 1000:.1f}k units (a client that overshoots and snaps back travels further than the host)")
    print(f"  spikes {tot_spikes} ({100 * tot_spikes / tot_frames:.2f}%)  reversals {tot_rev} ({100 * tot_rev / tot_frames:.2f}%)")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        analyse(p)
