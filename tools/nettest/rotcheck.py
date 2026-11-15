#!/usr/bin/env python3
"""Do projectiles point the same way on the host and on a client?

usage: rotcheck.py host_dump.csv client_dump.csv [weaponnum ...]

Needs --net-dump files made with --net-dump-interval 1. For weapon props the dump's theta
and frame columns hold the heading and pitch of the prop's z axis. For each client sample
this finds the host sample of the same prop that is nearest in position and reports how far
apart the two directions are, per weapon number (default: 83 rocket, 84 homing rocket).
It also says how far each side's direction is from the direction the prop is travelling in.
"""

import csv
import math
import sys
from collections import defaultdict


def load(path, wanted):
    props = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["type"] != "4" or int(r["weapon"]) not in wanted:
                continue
            props[int(r["syncid"])].append((float(r["x"]), float(r["y"]), float(r["z"]),
                                           float(r["theta"]), float(r["frame"]), int(r["weapon"])))
    return props


def vec(yaw, pitch):
    c = math.cos(pitch)
    return (math.sin(yaw) * c, math.sin(pitch), math.cos(yaw) * c)


def angle(a, b):
    d = max(-1.0, min(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]))
    return math.degrees(math.acos(d))


def travel(samples):
    """angle between the z axis and the direction of travel, per moving sample"""
    out = []
    for p, q in zip(samples, samples[1:]):
        d = (q[0] - p[0], q[1] - p[1], q[2] - p[2])
        n = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
        if n < 2.0:
            continue
        out.append(angle(vec(q[3], q[4]), (d[0] / n, d[1] / n, d[2] / n)))
    return out


def main():
    wanted = set(int(a) for a in sys.argv[3:]) or {83, 84}
    host = load(sys.argv[1], wanted)
    client = load(sys.argv[2], wanted)
    diffs = defaultdict(list)
    htravel, ctravel = [], []
    matched = 0
    for sid, cs in client.items():
        hs = host.get(sid)
        if not hs:
            continue
        matched += 1
        htravel += travel(hs)
        ctravel += travel(cs)
        for c in cs:
            best = min(hs, key=lambda h: (h[0] - c[0]) ** 2 + (h[1] - c[1]) ** 2 + (h[2] - c[2]) ** 2)
            if (best[0] - c[0]) ** 2 + (best[1] - c[1]) ** 2 + (best[2] - c[2]) ** 2 > 60 ** 2:
                continue
            diffs[c[5]].append(angle(vec(c[3], c[4]), vec(best[3], best[4])))
    print(f"props on host {len(host)}, on client {len(client)}, on both {matched}")
    for wn, d in sorted(diffs.items()):
        d.sort()
        print(f"weapon {wn}: {len(d)} samples, direction differs by mean {sum(d) / len(d):.1f} deg, "
              f"median {d[len(d) // 2]:.1f}, worst {d[-1]:.1f}")
    for name, t in (("host", htravel), ("client", ctravel)):
        if t:
            t.sort()
            print(f"{name}: z axis vs direction of travel: median {t[len(t) // 2]:.1f} deg, worst {t[-1]:.1f}")


if __name__ == "__main__":
    main()
