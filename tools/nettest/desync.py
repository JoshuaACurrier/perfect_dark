#!/usr/bin/env python3
"""Compare chr state dumps (--net-dump) from a host and a client.

usage: desync.py host.csv client.csv [--verbose]

For every chr (keyed by syncid) and every host sample, finds the client sample
with the nearest tick and reports how far apart they are. A chr is "in sync"
when its position error stays small and its alive/dead state matches.
"""

import csv
import math
import sys
from collections import defaultdict

POS_OK = 30.0  # world units; a player is ~ 50 units wide
TICK_WINDOW = 12  # how far apart host and client ticks may be to be compared

TYPES = {1: "obj", 2: "door", 3: "chr", 4: "weapon", 5: "eyespy", 6: "player"}


def load(path):
    rows = defaultdict(dict)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows[int(r["syncid"])][int(r["tick"])] = r
    return rows


def nearest(samples, tick):
    if tick in samples:
        return samples[tick]
    best = None
    for dt in range(1, TICK_WINDOW + 1):
        for t in (tick - dt, tick + dt):
            if t in samples:
                return samples[t]
    return best


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    verbose = "--verbose" in sys.argv
    host = load(sys.argv[1])
    client = load(sys.argv[2])

    total_bad = 0
    total = 0
    print(f"{'syncid':>6} {'type':>7} {'samples':>7} {'meanerr':>8} {'maxerr':>8} {'%off':>6} {'deadmis':>7} {'actmis%':>7}")
    for sid in sorted(host):
        hs = host[sid]
        cs = client.get(sid, {})
        errs = []
        deadmis = 0
        actmis = 0
        ptype = "?"
        for tick, h in sorted(hs.items()):
            ptype = TYPES.get(int(h["type"]), h["type"])
            c = nearest(cs, tick)
            if c is None:
                continue
            d = math.dist((float(h["x"]), float(h["y"]), float(h["z"])),
                          (float(c["x"]), float(c["y"]), float(c["z"])))
            errs.append(d)
            if h["dead"] != c["dead"]:
                deadmis += 1
            if h["act"] != c["act"]:
                actmis += 1
            if verbose and d > POS_OK:
                print(f"   tick {tick}: sid {sid} host ({h['x']},{h['y']},{h['z']}) act {h['act']} "
                      f"client ({c['x']},{c['y']},{c['z']}) act {c['act']} err {d:.1f}")
        if not errs:
            print(f"{sid:>6} {ptype:>7}  (no matching client samples)")
            continue
        off = sum(1 for e in errs if e > POS_OK)
        total_bad += off
        total += len(errs)
        print(f"{sid:>6} {ptype:>7} {len(errs):>7} {sum(errs)/len(errs):>8.1f} {max(errs):>8.1f} "
              f"{100*off/len(errs):>5.1f}% {deadmis:>7} {100*actmis/len(errs):>6.1f}%")

    if total:
        print(f"\noverall: {100*total_bad/total:.1f}% of samples more than {POS_OK} units off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
