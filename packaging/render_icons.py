#!/usr/bin/env python3
"""Renders packaging/icons/dev.workbench.app.svg to the hicolor PNG sizes.

Hand-rolled rather than shelling out to rsvg-convert/inkscape/ImageMagick
so `./package.sh` works on a machine with none of them installed. The
shapes are duplicated from the SVG as signed-distance functions, which is
also what gives the 1px analytic antialiasing.
"""

import math
import struct
import zlib
from pathlib import Path

SIZES = [16, 24, 32, 48, 64, 128, 256]
OUT_DIR = Path(__file__).resolve().parent / "icons"

BG = (0x1F, 0x24, 0x30)
BORDER = (0x39, 0x42, 0x5A)
CHEVRON = (0x4A, 0xDE, 0x80)
CURSOR = (0xE2, 0xE8, 0xF0)


def sdf_rounded_rect(px, py, x, y, w, h, r):
    cx, cy = x + w / 2, y + h / 2
    qx = abs(px - cx) - (w / 2 - r)
    qy = abs(py - cy) - (h / 2 - r)
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sdf_capsule(px, py, ax, ay, bx, by, r):
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    denom = bax * bax + bay * bay
    h = 0.0 if denom == 0 else max(0.0, min(1.0, (pax * bax + pay * bay) / denom))
    return math.hypot(pax - bax * h, pay - bay * h) - r


def coverage(distance, scale):
    """Distances are in 256-space; scale converts the 1px AA band."""
    return max(0.0, min(1.0, 0.5 - distance * scale))


def blend(dst, src, alpha):
    return tuple(int(round(d + (s - d) * alpha)) for d, s in zip(dst, src))


def render(size):
    scale = size / 256.0
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            # Sample at pixel centres, expressed back in 256-space.
            x = (px + 0.5) / scale
            y = (py + 0.5) / scale

            outer = sdf_rounded_rect(x, y, 8, 8, 240, 240, 48)
            a_bg = coverage(outer, scale)
            if a_bg <= 0.0:
                row += b"\x00\x00\x00\x00"
                continue

            rgb = BG

            # Inset stroke: the band between the outer edge and 3px in.
            a_border = coverage(abs(outer + 1.5) - 1.5, scale)
            if a_border > 0.0:
                rgb = blend(rgb, BORDER, a_border)

            chevron = min(
                sdf_capsule(x, y, 78, 84, 126, 128, 11),
                sdf_capsule(x, y, 126, 128, 78, 172, 11),
            )
            a_chevron = coverage(chevron, scale)
            if a_chevron > 0.0:
                rgb = blend(rgb, CHEVRON, a_chevron)

            cursor = sdf_rounded_rect(x, y, 144, 158, 56, 22, 11)
            a_cursor = coverage(cursor, scale)
            if a_cursor > 0.0:
                rgb = blend(rgb, CURSOR, a_cursor)

            row += bytes(rgb) + bytes([int(round(a_bg * 255))])
        rows.append(bytes(row))
    return rows


def write_png(path, size, rows):
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for size in SIZES:
        path = OUT_DIR / f"dev.workbench.app-{size}.png"
        write_png(path, size, render(size))
        print(f"  {path.relative_to(OUT_DIR.parent.parent)}")


if __name__ == "__main__":
    main()
