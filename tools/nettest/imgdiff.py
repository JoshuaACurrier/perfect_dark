#!/usr/bin/env python3
"""Compare two screenshots (PNG, same size) without any image library.

usage: imgdiff.py a.png b.png [--grid N]

Prints the average brightness of each, the average absolute difference, and an
N x N grid (default 8) of how much brighter (+) or darker (-) b is than a, in
percent of full brightness. For checking what a video option changed.
"""

import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6), "8-bit RGB or RGBA only"
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(line)
        prev = line
    # brightness per pixel
    lum = [[(r[x * bpp] + r[x * bpp + 1] + r[x * bpp + 2]) / 3 for x in range(w)] for r in rows]
    return w, h, lum


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    grid = 8
    if "--grid" in sys.argv:
        grid = int(sys.argv[sys.argv.index("--grid") + 1])
        args = [a for a in args if a != str(grid)]
    wa, ha, a = load(args[0])
    wb, hb, b = load(args[1])
    assert (wa, ha) == (wb, hb), "different sizes"
    n = wa * ha
    print(f"mean brightness: a {sum(map(sum, a)) / n / 2.55:.1f}%  b {sum(map(sum, b)) / n / 2.55:.1f}%")
    print(f"mean absolute difference: {sum(abs(a[y][x] - b[y][x]) for y in range(ha) for x in range(wa)) / n / 2.55:.2f}%")
    print(f"b minus a, {grid}x{grid} cells, % of full brightness:")
    for gy in range(grid):
        cells = []
        for gx in range(grid):
            y0, y1 = gy * ha // grid, (gy + 1) * ha // grid
            x0, x1 = gx * wa // grid, (gx + 1) * wa // grid
            d = sum(b[y][x] - a[y][x] for y in range(y0, y1) for x in range(x0, x1)) / ((y1 - y0) * (x1 - x0))
            cells.append(f"{d / 2.55:+6.1f}")
        print("  " + " ".join(cells))


if __name__ == "__main__":
    main()
