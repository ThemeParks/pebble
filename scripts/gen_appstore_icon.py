#!/usr/bin/env python3
"""
gen_appstore_icon.py — render an appstore icon for a destination.

Pebble / Rebble appstore listings want a square PNG (144×144 menu icon,
512×512 for the store banner). The .pbw doesn't need this — it's
uploaded separately at submission time. We keep the design simple and
aligned with what the app itself looks like:

    * Oxford-blue rounded-square background (same as the in-app header).
    * The destination's flagship park icon (first entry in `parks`),
      drawn large in that park's theme colour.

The icon shapes are ports of the small ~14px shapes drawn in
src/c/park_picker.c — same silhouettes, scaled up.

Usage:
    python3 scripts/gen_appstore_icon.py wdw
    # writes assets/appstore/wdw-144.png and wdw-512.png

Requires Pillow (`pip install pillow`).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parent.parent
DESTINATIONS_DIR = REPO_ROOT / "destinations"
OUT_DIR = REPO_ROOT / "assets" / "appstore"

# Oxford Blue — Pebble GColorOxfordBlue approximation (the in-app header).
HEADER_BG = (0, 25, 85)

# Brighter, friendlier blue for the uber-app icon — theme-park signage
# feel rather than the formal dark navy the per-destination icons use.
MULTI_BG = (50, 130, 220)

# Park-colour tokens → RGB. Matches colour_from_token() in park_picker.c.
PALETTE = {
    "pink":   (255, 85, 170),   # ShockingPink
    "cyan":   (0, 170, 255),    # VividCerulean
    "blue":   (0, 85, 255),     # BlueMoon
    "red":    (255, 0, 0),
    "green":  (0, 128, 0),      # IslamicGreen
    "orange": (255, 170, 0),
    "yellow": (255, 255, 0),
    "purple": (170, 0, 170),
}


def load_destination(name: str) -> dict:
    path = DESTINATIONS_DIR / f"{name}.json"
    if not path.exists():
        sys.exit(f"destination not found: {path}")
    return json.loads(path.read_text())


def rounded_square(size: int, bg: tuple[int, int, int]) -> Image.Image:
    """Oxford-blue rounded square on a transparent canvas."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Pebble-esque corner radius — ~18% of size is similar to iOS/Pebble.
    d.rounded_rectangle((0, 0, size - 1, size - 1),
                        radius=int(size * 0.18), fill=bg + (255,))
    return img


# ── Icon shapes (scaled-up ports of src/c/park_picker.c) ──────────────

def draw_castle(d: ImageDraw.ImageDraw, cx: int, cy: int, s: float,
                col: tuple[int, int, int]) -> None:
    """Hi-res castle silhouette. Deliberately *not* a scaled copy of
    the 14px watch icon — at large sizes the tiny version's three
    vertical battlements read as… not a castle. This redraw has a
    proper wide base with crenellations, a central keep with its own
    crenellations, and a pennant, so the silhouette actually registers
    as a castle at 144px+."""
    def rect(x0, y0, x1, y1):
        d.rectangle((cx + x0 * s, cy + y0 * s,
                     cx + x1 * s - 1, cy + y1 * s - 1), fill=col)

    # Base wall
    rect(-7, 1, 7, 6)
    # Base crenellations — 5 merlons with gaps, split around the keep
    for mx in (-7, -5, -3, 3, 5):
        rect(mx, 0, mx + 1, 1)
    # Central keep rising above the base
    rect(-2, -4, 3, 1)
    # Keep crenellations — 3 merlons
    for mx in (-2, 0, 2):
        rect(mx, -5, mx + 1, -4)
    # Flagpole
    rect(0.25, -8, 0.75, -5)
    # Pennant (little right-pointing triangle)
    d.polygon([(cx + 0.75 * s, cy - 8 * s),
               (cx + 3.25 * s, cy - 7.25 * s),
               (cx + 0.75 * s, cy - 6.5 * s)], fill=col)


def draw_sphere(d: ImageDraw.ImageDraw, cx: int, cy: int, s: float,
                col: tuple[int, int, int]) -> None:
    r = 6 * s
    d.ellipse((cx - r, cy - s - r, cx + r, cy - s + r), fill=col)


def draw_star(d: ImageDraw.ImageDraw, cx: int, cy: int, s: float,
              col: tuple[int, int, int]) -> None:
    """Mathematically-exact pentagram (no pixel rounding at large sizes)."""
    R, r = 7 * s, 3 * s
    pts = []
    for i in range(10):
        theta = math.radians(i * 36)
        rr = R if i % 2 == 0 else r
        pts.append((cx + rr * math.sin(theta),
                    cy - rr * math.cos(theta)))
    d.polygon(pts, fill=col)


def draw_tree(d: ImageDraw.ImageDraw, cx: int, cy: int, s: float,
              col: tuple[int, int, int]) -> None:
    d.polygon([(cx, cy - 7 * s),
               (cx + 6 * s, cy + 3 * s),
               (cx - 6 * s, cy + 3 * s)], fill=col)
    d.rectangle((cx - 1 * s, cy + 3 * s,
                 cx + 1 * s - 1, cy + 7 * s - 1), fill=col)


DISPATCH = {
    "castle": draw_castle, "sphere": draw_sphere,
    "star":   draw_star,   "tree":   draw_tree,
}


# ── Multi-destination icon ────────────────────────────────────────────

def draw_ferris_wheel(d: ImageDraw.ImageDraw, size: int,
                      bg: tuple[int, int, int]) -> None:
    """Ferris wheel + rollercoaster track silhouette, centred on the
    icon. Universal theme-park symbol — no brand baggage. `bg` must
    match the icon background so the wheel's inner cutout reads as
    transparent."""
    fg = (255, 210, 40)   # warm yellow for the structure
    cx = size // 2
    cy = int(size * 0.44)
    R  = int(size * 0.32)            # outer wheel radius (before gondolas)
    rim = max(2, int(size * 0.022))  # thinner rim — structure, not object
    hub = max(3, int(size * 0.05))
    spokes = 8
    gondola_r = int(size * 0.065)    # size of each colourful car

    # Rollercoaster track behind the wheel — two wavy rails + crossties,
    # passing across the bottom of the icon. Drawn first so the wheel
    # disk hides the middle section, leaving the track peeking out each
    # side.
    track_y0 = cy + R + gondola_r + int(size * 0.02)
    amp      = int(size * 0.06)
    tx0, tx1 = int(size * 0.04), int(size * 0.96)
    period   = int(size * 0.42)
    rail_gap = max(4, int(size * 0.05))
    rail_w   = max(2, int(size * 0.022))
    tie_w    = max(1, int(size * 0.016))

    N = 96
    top_rail, bot_rail = [], []
    for i in range(N):
        x  = tx0 + (tx1 - tx0) * i / (N - 1)
        yc = track_y0 - amp * math.sin(2 * math.pi * (x - tx0) / period)
        top_rail.append((x, yc - rail_gap / 2))
        bot_rail.append((x, yc + rail_gap / 2))
    d.line(top_rail, fill=fg, width=rail_w, joint="curve")
    d.line(bot_rail, fill=fg, width=rail_w, joint="curve")

    # Crossties every ~8 samples.
    for i in range(2, N - 2, 8):
        (tx, ty) = top_rail[i]
        (bx, by) = bot_rail[i]
        d.line((tx, ty, bx, by), fill=fg, width=tie_w)

    # Wheel rim (outer - inner).
    d.ellipse((cx - R, cy - R, cx + R, cy + R), fill=fg)
    d.ellipse((cx - R + rim, cy - R + rim,
               cx + R - rim, cy + R - rim), fill=bg)

    # Spokes hub → just inside rim.
    spoke_w = max(2, int(size * 0.018))
    inner = R - rim - 1
    for i in range(spokes):
        theta = math.radians(i * 360 / spokes + 22.5)  # rotate so gondolas sit between 12 o'clock and sides
        x = cx + inner * math.sin(theta)
        y = cy - inner * math.cos(theta)
        d.line((cx, cy, x, y), fill=fg, width=spoke_w)

    # Gondolas: circles sitting centred on the rim at each spoke tip,
    # in palette colours. (Real gondolas hang below the rim — at icon
    # scale that reads as mis-alignment, so snap them onto the rim.)
    gondola_cols = [
        PALETTE["red"], PALETTE["orange"], PALETTE["yellow"], PALETTE["green"],
        PALETTE["cyan"], PALETTE["blue"], PALETTE["purple"], PALETTE["pink"],
    ]
    for i in range(spokes):
        theta = math.radians(i * 360 / spokes + 22.5)
        gx = cx + R * math.sin(theta)
        gy = cy - R * math.cos(theta)
        d.ellipse((gx - gondola_r, gy - gondola_r,
                   gx + gondola_r, gy + gondola_r),
                  fill=gondola_cols[i])

    # Hub on top of spokes.
    d.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=fg)


def render(dest: dict, size: int) -> Image.Image:
    multi = bool(dest.get("multiDestination"))
    bg = MULTI_BG if multi else HEADER_BG
    img = rounded_square(size, bg)
    d = ImageDraw.Draw(img)

    if multi:
        # Uber-app: no single flagship park. Draw a ferris wheel —
        # universal theme-park silhouette, brand-agnostic (no
        # castle/sphere Disney baggage).
        draw_ferris_wheel(d, size, bg)
        return img

    park = dest["parks"][0]
    shape = DISPATCH.get(park.get("icon", ""))
    col = PALETTE.get(park.get("color", ""), (180, 180, 180))

    # Fit the ~14px shape inside ~70% of the icon. scale = size*0.7 / 14.
    scale = size * 0.7 / 14
    cx, cy = size // 2, size // 2 + int(size * 0.02)  # nudge down slightly

    if shape:
        shape(d, cx, cy, scale, col)
    else:
        r = int(size * 0.25)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=col)

    return img


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("destination", help="destinations/<name>.json stem")
    p.add_argument("--sizes", default="144,512",
                   help="comma-separated PNG sizes (default: 144,512)")
    args = p.parse_args()

    dest = load_destination(args.destination)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for size in (int(s) for s in args.sizes.split(",")):
        img = render(dest, size)
        out = OUT_DIR / f"{args.destination}-{size}.png"
        img.save(out)
        print(f"wrote {out.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
