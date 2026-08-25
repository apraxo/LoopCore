#!/usr/bin/env python3
"""Generates res/loopcore.ico.

The mark: three nodes fully connected through a core -- the smallest graph
that still reads as a network. A textbook 2-3-2 net has seven nodes and
twelve edges, which is unreadable at 16px; four elements survive.

Rendered per-size rather than downsampled from one master, so the small
versions keep their stroke weight instead of turning to mush. These
proportions are mirrored in ui::DrawLogo (src/theme.cpp) -- change one,
change both, or the taskbar icon and the in-app mark drift apart.

Alternatives that were considered live in tools/net_options.py.
"""

import math
from PIL import Image, ImageDraw

EDGE = (0x35, 0x8C, 0x79, 255)   # outer triangle, dimmer so nodes read first
WIRE = (0x4F, 0xC7, 0xAC, 255)   # spokes and outer nodes
CORE = (0x8C, 0xE8, 0xD4, 255)   # centre node

SIZES = [256, 128, 64, 48, 32, 24, 16]
SS = 8          # supersample factor


def render(px: int) -> Image.Image:
    n = px * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    stroke = max(SS, int(n * 0.055))
    c = (n / 2, n / 2)
    R = n * 0.34

    # One node up, two below: a stable silhouette at any rotation of the eye.
    pts = [(c[0] + R * math.cos(math.radians(a - 90)),
            c[1] + R * math.sin(math.radians(a - 90))) for a in (0, 120, 240)]

    for a, b in ((0, 1), (1, 2), (2, 0)):
        d.line([pts[a], pts[b]], fill=EDGE, width=stroke)
    for p in pts:
        d.line([c, p], fill=WIRE, width=stroke)

    for p in pts:
        r = n * 0.105
        d.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=WIRE)
    r = n * 0.13
    d.ellipse((c[0] - r, c[1] - r, c[0] + r, c[1] + r), fill=CORE)

    return img.resize((px, px), Image.LANCZOS)


def main() -> None:
    frames = [render(s) for s in SIZES]
    frames[0].save(
        "res/loopcore.ico",
        format="ICO",
        sizes=[(s, s) for s in SIZES],
        append_images=frames[1:],
    )
    frames[0].save("res/loopcore_preview.png")
    print("wrote res/loopcore.ico with sizes", SIZES)


if __name__ == "__main__":
    main()
