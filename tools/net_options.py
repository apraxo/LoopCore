#!/usr/bin/env python3
"""Renders six neural-network logo directions to /tmp/net_options.png.

Constraint driving all of these: a full 3-layer net has too many elements to
survive 16px. The ones that work keep the node count low and the edges thick.
"""

import math
from PIL import Image, ImageDraw, ImageFont

EDGE = (0x35, 0x8C, 0x79, 255)   # dimmer, so nodes read first
WIRE = (0x4F, 0xC7, 0xAC, 255)
NODE = (0x8C, 0xE8, 0xD4, 255)
SS = 6


def canvas(n):
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def dot(d, p, r, fill=NODE):
    d.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=fill)


def edges(d, pairs, w, fill=EDGE):
    for a, b in pairs:
        d.line([a, b], fill=fill, width=w)


# ------------------------------------------------------------- A: full net
def fullnet(n):
    """Textbook 2-3-2. The most literal, and the most crowded."""
    img, d = canvas(n)
    w = max(SS, int(n * 0.035))
    r = n * 0.075
    cols = [
        [(n * 0.18, n * 0.33), (n * 0.18, n * 0.67)],
        [(n * 0.50, n * 0.20), (n * 0.50, n * 0.50), (n * 0.50, n * 0.80)],
        [(n * 0.82, n * 0.33), (n * 0.82, n * 0.67)],
    ]
    for a, b in ((0, 1), (1, 2)):
        edges(d, [(p, q) for p in cols[a] for q in cols[b]], w)
    for i, col in enumerate(cols):
        for p in col:
            dot(d, p, r, NODE if i == 1 else WIRE)
    return img


# ----------------------------------------------------------- B: radial hub
def radial(n):
    """One core, six satellites. Symmetric, so it stays balanced when small."""
    img, d = canvas(n)
    w = max(SS, int(n * 0.05))
    R = n * 0.33
    c = (n / 2, n / 2)
    pts = [(c[0] + R * math.cos(math.radians(a - 90)),
            c[1] + R * math.sin(math.radians(a - 90))) for a in range(0, 360, 60)]
    edges(d, [(c, p) for p in pts], w, WIRE)
    for p in pts:
        dot(d, p, n * 0.085, WIRE)
    dot(d, c, n * 0.135, NODE)
    return img


# -------------------------------------------------------------- C: lattice
def lattice(n):
    """Three outer nodes fully connected through a core. Four elements only."""
    img, d = canvas(n)
    w = max(SS, int(n * 0.055))
    c = (n / 2, n / 2)
    R = n * 0.34
    pts = [(c[0] + R * math.cos(math.radians(a - 90)),
            c[1] + R * math.sin(math.radians(a - 90))) for a in (0, 120, 240)]
    edges(d, [(pts[0], pts[1]), (pts[1], pts[2]), (pts[2], pts[0])], w, EDGE)
    edges(d, [(c, p) for p in pts], w, WIRE)
    for p in pts:
        dot(d, p, n * 0.105, WIRE)
    dot(d, c, n * 0.13, NODE)
    return img


# ------------------------------------------------------------------ D: fan
def fan(n):
    """One input fanning to three. Shows direction, which a symmetric net does not."""
    img, d = canvas(n)
    w = max(SS, int(n * 0.055))
    a = (n * 0.20, n * 0.50)
    outs = [(n * 0.78, n * 0.22), (n * 0.78, n * 0.50), (n * 0.78, n * 0.78)]
    edges(d, [(a, p) for p in outs], w, WIRE)
    for p in outs:
        dot(d, p, n * 0.10, WIRE)
    dot(d, a, n * 0.14, NODE)
    return img


# ------------------------------------------------------- E: net in a crop
def cropnet(n):
    """Corner brackets around a small net. Detection and inference together."""
    img, d = canvas(n)
    bw = max(SS, int(n * 0.075))
    w = max(SS, int(n * 0.05))
    m = n * 0.10
    L = n * 0.20
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        cx = m if sx > 0 else n - m
        cy = m if sy > 0 else n - m
        d.line([(cx, cy), (cx + sx * L, cy)], fill=WIRE, width=bw)
        d.line([(cx, cy), (cx, cy + sy * L)], fill=WIRE, width=bw)
    left = [(n * 0.34, n * 0.36), (n * 0.34, n * 0.64)]
    right = [(n * 0.66, n * 0.36), (n * 0.66, n * 0.64)]
    edges(d, [(p, q) for p in left for q in right], w, EDGE)
    for p in left + right:
        dot(d, p, n * 0.075, NODE)
    return img


# ---------------------------------------------------------- F: constellation
def constellation(n):
    """Asymmetric graph with weighted nodes. Least generic, hardest to shrink."""
    img, d = canvas(n)
    w = max(SS, int(n * 0.045))
    s = n / 100.0
    P = {
        "a": (20 * s, 30 * s), "b": (20 * s, 72 * s),
        "c": (50 * s, 50 * s), "d": (76 * s, 24 * s),
        "e": (80 * s, 68 * s),
    }
    edges(d, [(P["a"], P["c"]), (P["b"], P["c"]), (P["c"], P["d"]),
              (P["c"], P["e"]), (P["a"], P["d"])], w, WIRE)
    for k, r in (("a", .075), ("b", .065), ("d", .085), ("e", .07)):
        dot(d, P[k], n * r, WIRE)
    dot(d, P["c"], n * 0.135, NODE)
    return img


OPTIONS = [
    ("A  full net",     fullnet),
    ("B  radial hub",   radial),
    ("C  lattice",      lattice),
    ("D  fan",          fan),
    ("E  net in crop",  cropnet),
    ("F  constellation", constellation),
]


def render(fn, px):
    return fn(px * SS).resize((px, px), Image.LANCZOS)


def main():
    cw, ch = 240, 260
    cols, rows = 3, 2
    sheet = Image.new("RGBA", (cw * cols, ch * rows), (0x14, 0x16, 0x1A, 255))
    d = ImageDraw.Draw(sheet)
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15)
    except OSError:
        f = ImageFont.load_default()

    for i, (name, fn) in enumerate(OPTIONS):
        ox, oy = (i % cols) * cw, (i // cols) * ch
        sheet.alpha_composite(render(fn, 150), (ox + 45, oy + 18))
        d.text((ox + 20, oy + 180), name, fill=(0xE6, 0xE9, 0xEE, 255), font=f)
        x = ox + 20
        for s in (32, 24, 16):
            sheet.alpha_composite(render(fn, s), (x, oy + 216 - s // 2))
            x += s + 16

    sheet.convert("RGB").save("/tmp/net_options.png")
    print("wrote /tmp/net_options.png")


if __name__ == "__main__":
    main()
