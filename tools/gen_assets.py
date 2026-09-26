#!/usr/bin/env python3
# ============================================================================
#  MOBILADOR - tools/gen_assets.py
#  Single source of truth for the product's visual identity.
#
#  From the geometry defined here the script emits:
#     * src/ui/Icons.generated.h   - vector data compiled into the application
#     * assets/icon/*.svg|png|ico  - app icon, favicon, tray icon
#     * assets/logo/*.svg|png      - full logo, symbol, wordmark (dark + light)
#     * assets/splash.png          - splash artwork
#     * assets/icon/icon-sheet.png - review sheet of the whole icon family
#
#  ICON LANGUAGE (documented so new icons stay consistent)
#     canvas       24 x 24 units, origin top-left
#     safe area    2 .. 22  (2 units padding, 20 units of drawing space)
#     stroke       1.75 units, round caps and round joins
#     outer radius 3 units for device-like shapes, 1.5 for panels
#     style        outline first; solid fills only for accents <= 4 units
#     detail       max 8 strokes per icon, must stay readable at 16 px
# ============================================================================
import math, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GRID = 24.0

# --------------------------------------------------------------- primitives
def P(x, y):
    return (round(float(x), 3), round(float(y), 3))

def line(x0, y0, x1, y1):
    return {"kind": "stroke", "pts": [P(x0, y0), P(x1, y1)], "closed": False}

def poly(*pts, closed=False):
    return {"kind": "stroke", "pts": [P(*p) for p in pts], "closed": closed}

def arc(cx, cy, r, a0, a1, steps=None):
    """Arc in degrees, 0 = right, counter-clockwise in icon space (y down)."""
    if steps is None:
        span = abs(a1 - a0)
        steps = max(2, int(span / 12.0) + 1)
    pts = []
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append(P(cx + r * math.cos(a), cy - r * math.sin(a)))
    return {"kind": "stroke", "pts": pts, "closed": False}

def circle(cx, cy, r, steps=28):
    pts = []
    for i in range(steps):
        a = 2.0 * math.pi * i / steps
        pts.append(P(cx + r * math.cos(a), cy + r * math.sin(a)))
    return {"kind": "stroke", "pts": pts, "closed": True}

def rrect(x, y, w, h, r, closed=True, corner_steps=5):
    """Rounded rectangle as a stroked path."""
    pts = []
    def corner(cx, cy, a0, a1):
        for i in range(corner_steps + 1):
            a = math.radians(a0 + (a1 - a0) * i / corner_steps)
            pts.append(P(cx + r * math.cos(a), cy - r * math.sin(a)))
    corner(x + w - r, y + r, 90, 0)
    corner(x + w - r, y + h - r, 0, -90)
    corner(x + r, y + h - r, -90, -180)
    corner(x + r, y + r, 180, 90)
    return {"kind": "stroke", "pts": pts, "closed": True}

def fill_poly(*pts):
    return {"kind": "fill", "pts": [P(*p) for p in pts], "closed": True}

def fill_circle(cx, cy, r, steps=24):
    pts = []
    for i in range(steps):
        a = 2.0 * math.pi * i / steps
        pts.append(P(cx + r * math.cos(a), cy + r * math.sin(a)))
    return {"kind": "fill", "pts": pts, "closed": True}

def fill_rrect(x, y, w, h, r):
    return {"kind": "fill", "pts": rrect(x, y, w, h, r)["pts"], "closed": True}

def fill_round_line(x0, y0, x1, y1, thickness):
    """Capsule between two points - used for solid bars and bold accents."""
    dx, dy = x1 - x0, y1 - y0
    ln = math.hypot(dx, dy)
    if ln < 1e-6:
        return fill_circle(x0, y0, thickness * 0.5)
    nx, ny = -dy / ln * thickness * 0.5, dx / ln * thickness * 0.5
    pts = [P(x0 + nx, y0 + ny), P(x1 + nx, y1 + ny), P(x1 - nx, y1 - ny), P(x0 - nx, y0 - ny)]
    for i in range(0, 9):
        a = -math.atan2(dy, dx) + math.pi / 2 + math.pi * i / 8.0
        pts.insert(2, P(x1 + math.cos(a) * thickness * 0.5, y1 + math.sin(a) * thickness * 0.5))
    for i in range(0, 9):
        a = -math.atan2(-dy, -dx) + math.pi / 2 + math.pi * i / 8.0
        pts.append(P(x0 + math.cos(a) * thickness * 0.5, y0 + math.sin(a) * thickness * 0.5))
    return {"kind": "fill", "pts": pts, "closed": True}


def path(*items, closed=False):
    """A stroke composed of straight points and arc/segment pieces."""
    pts = []
    for it in items:
        if isinstance(it, dict):
            pts.extend(it["pts"])
        elif isinstance(it, (list, tuple)) and len(it) == 2 and isinstance(it[0], (int, float)):
            pts.append(P(*it))
        else:
            for q in it:
                pts.append(P(*q))
    return {"kind": "stroke", "pts": pts, "closed": closed}

def merge(*groups):
    out = []
    for g in groups:
        if isinstance(g, list):
            out.extend(g)
        elif g:
            out.append(g)
    return out

# ------------------------------------------------------------- icon library
I = {}

# --- device / connection family -------------------------------------------------
I["phone_connect"] = merge(
    rrect(6.5, 2, 11, 17, 2.4),
    [poly((9.5, 5.6), (14.5, 5.6))],                       # speaker slot
    [line(12, 16.2, 12, 16.2)],                            # home indicator (dot handled below)
    [fill_circle(12, 16.2, 0.95)],
    [line(19.5, 12.5, 22.5, 12.5), fill_circle(17.8, 12.5, 1.0)],   # link dashes to the right
)
I["usb"] = merge(
    [line(12, 20.5, 12, 4.5)],
    [poly((12, 4.5), (10.0, 6.6), (14.0, 6.6), (12, 4.5), (12, 4.5))],
    [fill_poly((12, 3.0), (9.6, 6.6), (14.4, 6.6))],       # arrow head
    [fill_circle(12, 20.5, 1.9)],                          # connector dot
    [poly((12, 13.5), (8.2, 13.5), (8.2, 10.6))],          # left branch
    [poly((12, 10.5), (16.0, 10.5), (16.0, 13.6))],        # right branch
    [fill_circle(8.2, 9.8, 1.0), fill_rrect(15.0, 12.4, 2.0, 2.0, 0.4)],
)
I["phone"] = merge(
    rrect(6.5, 2, 11, 20, 2.6),
    [poly((10, 5.2), (14, 5.2))],
    [fill_circle(12, 19, 0.9)],
)
I["pc"] = merge(
    rrect(2, 4, 20, 13, 1.8),
    [poly((9.5, 20.5), (14.5, 20.5)), line(12, 17.2, 12, 20.5)],
    [fill_round_line(12, 20.5, 12, 20.5, 0.1)],
)
I["monitor"] = merge(
    rrect(2, 3.5, 20, 13.5, 2.0),
    [line(12, 17, 12, 20.5), poly((8.5, 20.5), (15.5, 20.5))],
    [fill_rrect(4.2, 5.7, 6.0, 1.6, 0.8), fill_rrect(4.2, 8.5, 9.0, 1.6, 0.8)],
)
I["screen"] = merge(
    rrect(2.6, 3.4, 18.8, 12.6, 2.0),
    [fill_rrect(4.8, 16.2, 3.2, 1.6, 0.5)],
    [arc(6.6, 16.2, 4.4, 0, -90, steps=6)],
    [arc(6.6, 16.2, 7.2, 0, -90, steps=8)],
    [fill_circle(6.2, 16.0, 1.2)],
)
I["fullscreen"] = merge(
    [poly((3.5, 9), (3.5, 3.5), (9, 3.5))],
    [poly((15, 3.5), (20.5, 3.5), (20.5, 9))],
    [poly((20.5, 15), (20.5, 20.5), (15, 20.5))],
    [poly((9, 20.5), (3.5, 20.5), (3.5, 15))],
)
I["connection"] = merge(
    rrect(2.5, 8.5, 10, 7, 1.6),
    rrect(11.5, 8.5, 10, 7, 1.6),
    [line(6.5, 12, 7.0, 12), line(12, 12, 12.6, 12), line(17, 12, 17.5, 12)],
)
I["link"] = merge(
    [path((10.9, 13.1), (13.1, 10.9))],
    [path(arc(8.6, 12.4, 3.6, 118, 305, steps=12), arc(15.4, 11.6, 3.6, -62, 125, steps=12))],
    [path(arc(8.6, 12.4, 3.6, 118, 305, steps=12))],
    [path(arc(15.4, 11.6, 3.6, -62, 125, steps=12))],
)
I["usb_mode"] = merge(
    rrect(2.6, 8.4, 18.8, 7.2, 2.2),
    [fill_rrect(5.4, 10.6, 3.4, 2.8, 0.6), fill_rrect(15.2, 10.6, 3.4, 2.8, 0.6)],
    [line(11.2, 5.0, 11.2, 8.4), line(12.8, 5.0, 12.8, 8.4)],
    [fill_circle(12.0, 18.0, 1.0)],
)
I["refresh"] = merge(
    [arc(12, 12, 7.4, 70, 350, steps=18)],
    [fill_poly((16.2, 3.2), (19.6, 7.4), (14.6, 7.9))],
)
I["reset"] = merge(
    [arc(12, 12, 7.6, 120, 40, steps=18)],
    [fill_poly((7.4, 2.6), (12.6, 5.0), (8.4, 8.0))],
    [fill_circle(12, 12, 1.6)],
)
I["power"] = merge(
    [arc(12, 12.6, 7.0, 70, 470)],
    [line(12, 2.8, 12, 8.2)],
)
I["plug"] = merge(
    [rrect(8.0, 2.8, 8.0, 6.2, 3.0)],
    [fill_rrect(10.2, 4.6, 3.6, 1.4, 0.7)],
    [path((12.0, 9.0), (12.0, 12.8))],
    [rrect(5.6, 12.8, 12.8, 8.4, 2.2)],
    [fill_circle(12.0, 17.0, 1.6)],
)
# --- input family --------------------------------------------------------------
I["mouse"] = merge(
    [path((7.2, 11.4), arc(12, 11.4, 4.8, 180, 0, steps=10), (16.8, 11.4),
          arc(12, 11.4, 4.8, 0, 0, steps=2), (16.8, 15.2),
          arc(12, 15.2, 4.8, 0, 180, steps=10), (7.2, 15.2), (7.2, 11.4))],
    [line(7.4, 11.4, 16.6, 11.4)],
    [fill_round_line(12, 5.4, 12, 7.6, 1.5)],
    [line(12, 2.4, 12, 3.2)],
)
I["keyboard"] = merge(
    rrect(2, 6, 20, 12, 2.0),
    [line(5.3, 9.6, 6.9, 9.6), line(9.0, 9.6, 10.6, 9.6), line(12.7, 9.6, 14.3, 9.6), line(16.4, 9.6, 18.0, 9.6),
     fill_round_line(5.3, 12.4, 6.9, 12.4, 0.9), fill_round_line(9.0, 12.4, 10.6, 12.4, 0.9),
     fill_round_line(12.7, 12.4, 14.3, 12.4, 0.9), fill_round_line(16.4, 12.4, 18.0, 12.4, 0.9)],
    [line(8.0, 15.2, 16.0, 15.2)],
)
I["cursor"] = merge(
    [poly((6, 3.2), (6, 18.4), (10.2, 14.6), (12.9, 20.6), (15.6, 19.4), (12.9, 13.4), (18.2, 13.0), (6, 3.2), (6, 3.2))],
    [fill_poly((6.6, 5.4), (6.6, 15.6), (9.9, 12.7), (16.0, 11.9), (6.6, 5.4))],
)
I["gamepad"] = merge(
    [poly((8.2, 7.5), (15.8, 7.5), (17.6, 8.4), (21, 13.6), (20.6, 17.4), (18.4, 18.8), (16.2, 17.8), (15.2, 15.6), (8.8, 15.6), (7.8, 17.8), (5.6, 18.8), (3.4, 17.4), (3, 13.6), (6.4, 8.4), (8.2, 7.5))],
    [fill_round_line(7.2, 11.6, 10.0, 11.6, 1.5), fill_round_line(8.6, 10.2, 8.6, 13.0, 1.5)],
    [fill_circle(16.2, 11.8, 1.15), fill_circle(18.2, 13.9, 1.15)],
)
# --- metrics family ------------------------------------------------------------
I["fps"] = merge(
    [arc(12, 13.6, 8.0, 180, 0, steps=14)],
    [poly((4, 13.6), (4, 15.6)), poly((20, 13.6), (20, 15.6))],
    [poly((12, 13.6), (16.6, 8.4))],
    [fill_circle(12, 13.6, 1.6)],
    [fill_rrect(9.2, 18.4, 5.6, 1.4, 0.7)],
)
I["gauge"] = merge(
    [arc(12, 12, 8.2, 145, 35, steps=14)],
    [poly((12, 12), (16.4, 7.6))],
    [fill_circle(12, 12, 1.7)],
    [line(5.0, 17.6, 7.0, 16.0), line(19.0, 17.6, 17.0, 16.0)],
)
I["latency"] = merge(
    rrect(2, 4, 20, 16, 2.2),
    [poly((5, 12), (7.6, 12), (9.2, 8.4), (11.6, 15.6), (13.6, 12), (19, 12))],
)
I["activity"] = merge(
    [poly((2.6, 12), (6.4, 12), (8.6, 5.6), (12.2, 18.6), (14.6, 12), (21.4, 12))],
)
I["performance"] = merge(
    [fill_poly((13.6, 2.4), (5.6, 13.6), (10.9, 13.6), (9.4, 21.6), (18.4, 10.4), (12.9, 10.4))],
)
I["bolt"] = merge(
    [fill_poly((13.4, 2.2), (6.0, 13.2), (11.0, 13.2), (9.6, 21.8), (18.2, 10.8), (13.0, 10.8))],
)
I["clock"] = merge(
    circle(12, 12, 8.6),
    [poly((12, 6.6), (12, 12.4), (16.2, 14.4))],
)
I["thermometer"] = merge(
    [path((10.2, 13.6), (10.2, 5.4), arc(12, 5.4, 1.8, 180, 0, steps=6), (13.8, 5.4), (13.8, 13.6))],
    [arc(12, 16.4, 3.6, 180, 360, steps=10)],
    [fill_circle(12, 16.4, 2.4)],
)
I["chart"] = merge(
    [poly((3.6, 20.4), (3.6, 3.6))],
    [poly((3.6, 20.4), (20.6, 20.4))],
    [fill_rrect(6.4, 13.2, 2.6, 6.0, 0.6), fill_rrect(10.7, 9.0, 2.6, 10.2, 0.6),
     fill_rrect(15.0, 5.4, 2.6, 13.8, 0.6)],
)
I["target"] = merge(
    circle(12, 12, 8.4),
    circle(12, 12, 4.2),
    [fill_circle(12, 12, 1.5)],
)
I["layers"] = merge(
    [fill_poly((12, 2.6), (21, 7.2), (12, 11.8), (3, 7.2))],
    [poly((3, 12), (12, 16.6), (21, 12))],
    [poly((3, 16.6), (12, 21.2), (21, 16.6))],
)
# --- hardware family -----------------------------------------------------------
def chip(x, y, w, h, pins_side=3, label=None):
    out = [rrect(x, y, w, h, 1.4)]
    step = h / (pins_side + 1)
    for i in range(pins_side):
        py = y + step * (i + 1)
        out.append(line(x - 1.6, py, x, py))
        out.append(line(x + w, py, x + w + 1.6, py))
    stepx = w / (pins_side + 1)
    for i in range(pins_side):
        px = x + stepx * (i + 1)
        out.append(line(px, y - 1.6, px, y))
        out.append(line(px, y + h, px, y + h + 1.6))
    if label:
        out.append(fill_rrect(x + w * 0.22, y + h * 0.34, w * 0.56, h * 0.32, 0.6))
    return out

I["cpu"] = chip(6.6, 6.6, 10.8, 10.8, 3, label=True)
I["gpu"] = merge(
    rrect(2.6, 5.4, 18.8, 12.4, 1.8),
    [arc(9.2, 11.6, 3.2, 180, 380, steps=12)],
    [fill_circle(9.2, 11.6, 1.1)],
    [fill_rrect(14.6, 8.4, 5.0, 2.0, 0.5), fill_rrect(14.6, 12.6, 5.0, 2.0, 0.5)],
    [line(6.0, 17.8, 6.0, 20.6), line(18.2, 17.8, 18.2, 20.6)],
)
I["ram"] = merge(
    rrect(2.2, 7.4, 19.6, 9.2, 1.4),
    [fill_rrect(5.0, 9.8, 2.2, 4.4, 0.4), fill_rrect(8.6, 9.8, 2.2, 4.4, 0.4), fill_rrect(12.2, 9.8, 2.2, 4.4, 0.4)],
    [line(8.0, 16.6, 8.0, 19.0), line(12.0, 16.6, 12.0, 19.0), line(16.0, 16.6, 16.0, 19.0)],
)
I["display"] = merge(
    rrect(2, 3.6, 20, 13.4, 2.0),
    [poly((8.6, 20.6), (15.4, 20.6)), line(12, 17, 12, 20.6)],
    [fill_circle(12, 10.3, 1.3)],
)
I["audio"] = merge(
    [poly((3.4, 9.2), (7.2, 9.2), (12.2, 4.6), (12.2, 19.4), (7.2, 14.8), (3.4, 14.8), (3.4, 9.2))],
    [arc(12.2, 12, 4.4, -55, 55, steps=8)],
    [arc(12.2, 12, 7.6, -50, 50, steps=10)],
)
# --- system / UI family --------------------------------------------------------
def gear(cx, cy, outer=8.6, inner=3.0, teeth=8):
    segs = [circle(cx, cy, inner)]
    for i in range(teeth):
        a = 2 * math.pi * i / teeth + math.pi / teeth
        x0 = cx + math.cos(a) * (outer - 2.4)
        y0 = cy + math.sin(a) * (outer - 2.4)
        x1 = cx + math.cos(a) * outer
        y1 = cy + math.sin(a) * outer
        segs.append(fill_round_line(x0, y0, x1, y1, 2.9))
    segs.append(circle(cx, cy, outer - 3.1))
    return segs

I["settings"] = gear(12, 12)
I["advanced"] = merge(
    [line(3, 7.4, 21, 7.4), line(3, 16.6, 21, 16.6)],
    [fill_circle(8.6, 7.4, 2.5), fill_circle(15.4, 16.6, 2.5)],
    [fill_circle(8.6, 7.4, 1.0), fill_circle(15.4, 16.6, 1.0)],
)
I["appearance"] = merge(
    circle(12, 12, 8.6),
    [arc(12, 12, 8.6, 90, 270, steps=14)],
    [fill_circle(12, 12, 8.6 - 4.3)],
)
I["palette"] = merge(
    [poly((12, 3.4), (17.2, 7.0), (17.2, 17.0), (12, 20.6), (6.8, 17.0), (6.8, 7.0), (12, 3.4), (12, 3.4))],
    [fill_circle(12, 8.0, 1.9), fill_circle(8.6, 14.6, 1.5), fill_circle(15.4, 14.6, 1.5)],
)
I["color_drop"] = merge(
    [poly((12, 3.0), (17.4, 10.4), (17.4, 14.2), (14.6, 17.6), (9.4, 17.6), (6.6, 14.2), (6.6, 10.4), (12, 3.0))],
    [fill_circle(12, 12.6, 3.0)],
)
I["system"] = merge(
    rrect(3.0, 3.6, 18.0, 4.6, 1.2),
    rrect(3.0, 9.7, 18.0, 4.6, 1.2),
    rrect(3.0, 15.8, 18.0, 4.6, 1.2),
    [fill_circle(6.4, 5.9, 0.9), fill_circle(6.4, 12.0, 0.9), fill_circle(6.4, 18.1, 0.9)],
)
I["diagnostics"] = merge(
    rrect(2.6, 4.4, 18.8, 15.2, 2.2),
    [poly((6, 12), (8.6, 12), (10.2, 8.6), (12.4, 15.2), (14.2, 12), (18, 12))],
)
I["benchmark"] = merge(
    [poly((3.4, 4.0), (3.4, 20.0), (20.6, 20.0))],
    [fill_round_line(6.6, 17.2, 6.6, 12.4, 2.6)],
    [fill_round_line(11.0, 17.2, 11.0, 8.6, 2.6)],
    [fill_round_line(15.4, 17.2, 15.4, 5.6, 2.6)],
)
I["info"] = merge(circle(12, 12, 8.6), [line(12, 11.0, 12, 16.6)])
I["help"] = merge(circle(12, 12, 8.6),
                  [arc(9.4, 8.6, 2.7, 200, -40, steps=8)], [line(12.1, 11.0, 12.1, 11.2)], [line(12, 13.6, 12, 16.4)])
I["warning"] = merge(
    [poly((12, 3.4), (21.2, 19.6), (2.8, 19.6), (12, 3.4), (12, 3.4))],
    [line(12, 9.2, 12, 14.2)],
    [fill_circle(12, 16.9, 1.15)],
)
I["error"] = merge(circle(12, 12, 8.6), [line(8.6, 8.6, 15.4, 15.4), line(15.4, 8.6, 8.6, 15.4)])
I["check"] = merge([poly((4.4, 12.6), (9.6, 17.8), (19.6, 6.6))])
I["check_circle"] = merge(circle(12, 12, 8.6), [poly((7.8, 12.2), (10.9, 15.3), (16.4, 9.0))])
I["close"] = merge([line(5.6, 5.6, 18.4, 18.4), line(18.4, 5.6, 5.6, 18.4)])
I["minimize"] = merge([line(5.6, 12, 18.4, 12)])
I["maximize"] = merge(rrect(4.6, 4.6, 14.8, 14.8, 1.6))
I["pin"] = merge(
    [poly((9.4, 3.6), (14.6, 3.6), (13.4, 9.0), (16.6, 12.2), (7.4, 12.2), (10.6, 9.0), (9.4, 3.6))],
    [line(12, 12.2, 12, 20.4)],
)
I["chevron_down"] = merge([poly((5.6, 9.2), (12, 15.6), (18.4, 9.2))])
I["chevron_up"] = merge([poly((5.6, 14.8), (12, 8.4), (18.4, 14.8))])
I["chevron_left"] = merge([poly((14.8, 5.6), (8.4, 12), (14.8, 18.4))])
I["chevron_right"] = merge([poly((9.2, 5.6), (15.6, 12), (9.2, 18.4))])
I["play"] = merge([fill_poly((7.4, 4.4), (19.6, 12), (7.4, 19.6))])
I["stop"] = merge([fill_rrect(6.4, 6.4, 11.2, 11.2, 1.4)])
I["pause"] = merge([fill_rrect(6.8, 5.4, 3.8, 13.2, 0.9), fill_rrect(13.4, 5.4, 3.8, 13.2, 0.9)])
I["save"] = merge(
    [poly((4.6, 9.4), (4.6, 19.4), (19.4, 19.4), (19.4, 9.4), (12, 3.2), (4.6, 9.4))],
    [poly((8.4, 13.0), (12, 16.6), (15.6, 13.0)), line(12, 8.6, 12, 16.4)],
)
I["restore"] = merge(
    [poly((4.6, 14.0), (4.6, 19.4), (19.4, 19.4), (19.4, 14.0), (12, 6.4), (4.6, 14.0))],
    [poly((8.4, 11.2), (12, 7.4), (15.6, 11.2)), line(12, 7.6, 12, 15.4)],
)
I["folder"] = merge(
    [path((3.2, 19.2), (3.2, 7.2), (9.0, 7.2), (10.8, 9.6), (20.8, 9.6), (20.8, 19.2), (3.2, 19.2))],
    [line(3.4, 12.6, 20.6, 12.6)],
)
I["copy"] = merge(
    rrect(8.4, 8.4, 12.4, 12.4, 1.8),
    [poly((15.6, 5.4), (15.6, 3.4), (3.4, 3.4), (3.4, 15.6), (5.6, 15.6))],
)
I["eye"] = merge(
    [poly((2.2, 12), (6.2, 6.8), (17.8, 6.8), (21.8, 12), (17.8, 17.2), (6.2, 17.2), (2.2, 12))],
    [fill_circle(12, 12, 3.1)],
)
I["eye_off"] = merge(
    [poly((2.2, 12), (6.2, 6.8), (17.8, 6.8), (21.8, 12), (17.8, 17.2), (6.2, 17.2), (2.2, 12))],
    [line(4.4, 19.6, 19.6, 4.4)],
)
I["wand"] = merge(
    [fill_round_line(5.0, 19.0, 15.8, 8.2, 2.2)],
    [fill_poly((17.6, 2.8), (19.0, 6.0), (22.2, 7.4), (19.0, 8.8), (17.6, 12.0), (16.2, 8.8), (13.0, 7.4), (16.2, 6.0))],
)
I["shield"] = merge(
    [poly((12, 2.6), (20.2, 5.8), (20.2, 12.2), (18.2, 17.2), (12, 21.4), (5.8, 17.2), (3.8, 12.2), (3.8, 5.8), (12, 2.6))],
    [poly((8.2, 11.8), (11.0, 14.6), (16.0, 9.2))],
)
I["lock"] = merge(
    rrect(4.6, 10.4, 14.8, 10.6, 2.0),
    [path((8.0, 10.4), (8.0, 7.4), arc(12, 7.4, 4.0, 180, 0, steps=8), (16.0, 7.4), (16.0, 10.4))],
    [fill_circle(12, 15.6, 1.6)],
)
I["tag"] = merge(
    [poly((3.2, 11.6), (11.6, 3.2), (20.6, 3.2), (20.6, 12.2), (12.2, 20.6), (3.2, 11.6))],
    [fill_circle(16.6, 7.2, 1.7)],
)
I["list"] = merge(
    [line(9.0, 6.4, 20.6, 6.4), line(9.0, 12, 20.6, 12), line(9.0, 17.6, 20.6, 17.6)],
    [fill_circle(4.8, 6.4, 1.6), fill_circle(4.8, 12, 1.6), fill_circle(4.8, 17.6, 1.6)],
)
I["download"] = merge(
    [poly((4.6, 14.4), (4.6, 19.4), (19.4, 19.4), (19.4, 14.4))],
    [line(12, 4.0, 12, 14.6), poly((7.8, 10.6), (12, 14.8), (16.2, 10.6))],
)
I["upload"] = merge(
    [poly((4.6, 14.4), (4.6, 19.4), (19.4, 19.4), (19.4, 14.4))],
    [line(12, 14.8, 12, 4.2), poly((7.8, 8.4), (12, 4.2), (16.2, 8.4))],
)
I["wifi"] = merge(
    [arc(12, 18.8, 9.6, 52, 128, steps=12)],
    [arc(12, 18.8, 6.4, 48, 132, steps=10)],
    [arc(12, 18.8, 3.2, 44, 136, steps=8)],
    [fill_circle(12, 18.6, 1.35)],
)
I["grid"] = merge(
    rrect(3.4, 3.4, 7.4, 7.4, 1.2), rrect(13.2, 3.4, 7.4, 7.4, 1.2),
    rrect(3.4, 13.2, 7.4, 7.4, 1.2), rrect(13.2, 13.2, 7.4, 7.4, 1.2),
)
I["star"] = merge(
    [poly((12, 2.8), (15.0, 9.2), (22.0, 10.2), (16.9, 15.1), (18.1, 22.0), (12, 18.7), (5.9, 22.0), (7.1, 15.1), (2.0, 10.2), (9.0, 9.2), (12, 2.8), (12, 2.8))],
)
I["battery"] = merge(
    rrect(2.4, 7.4, 17.2, 9.2, 2.0),
    [fill_rrect(4.6, 9.6, 8.4, 4.8, 1.0)],
    [fill_rrect(21.0, 10.6, 1.6, 2.8, 0.6)],
)
I["clock_session"] = merge(
    circle(12, 12.8, 8.0),
    [poly((12, 7.6), (12, 13.0), (15.8, 15.0))],
    [line(9.0, 2.6, 15.0, 2.6)],
    [line(12, 2.6, 12, 4.8)],
)
I["mirror"] = merge(
    rrect(6.6, 2.4, 10.8, 19.2, 2.4),
    [fill_round_line(9.4, 9.4, 14.6, 9.4, 1.6), fill_round_line(9.4, 12.2, 14.6, 12.2, 1.6)],
    [poly((9.4, 15.2), (12, 15.2))],
)
I["refresh_rate"] = merge(
    [fill_poly((13.6, 2.6), (8.0, 11.8), (11.6, 11.8), (10.6, 18.4), (16.6, 8.8), (12.9, 8.8))],
    [fill_round_line(5.4, 19.4, 9.6, 19.4, 1.6)],
    [fill_round_line(7.4, 15.6, 12.4, 15.6, 1.6)],
)
I["block"] = merge(
    fill_circle(12, 12, 8.6),
    [fill_poly((12, 5.6), (17.6, 18.4), (6.4, 18.4))],
)
I["dot"] = merge([fill_circle(12, 12, 4.6)])
I["dot_small"] = merge([fill_circle(12, 12, 2.6)])
I["logo_mark"] = merge(
    # The Mobilador symbol is also available as an icon so it can be used inline.
    rrect(4.2, 2.2, 15.6, 19.6, 3.4),
    [fill_poly((13.6, 6.4), (8.4, 13.4), (11.7, 13.4), (10.8, 18.4), (16.4, 11.2), (13.0, 11.2))],
    [fill_round_line(8.4, 19.0, 15.6, 19.0, 1.4)],
)

ICON_ORDER = list(I.keys())

# ------------------------------------------------------------------ logo art
def logo_symbol_paths(scale=1.0, ox=0.0, oy=0.0):
    """The official symbol: device + screen + speed bolt + link dashes."""
    def T(pts):
        return [(ox + x * scale, oy + y * scale) for (x, y) in pts]
    segs = []
    body = rrect(5.0, 2.0, 14.0, 20.0, 3.2)
    segs.append({"kind": "stroke", "pts": T(body["pts"]), "closed": True, "width": 2.0})
    bolt = [P(14.4, 6.0), P(8.6, 13.6), P(12.2, 13.6), P(11.3, 18.6), P(17.0, 11.0), P(13.4, 11.0)]
    segs.append({"kind": "fill", "pts": T(bolt), "closed": True})
    segs.append({"kind": "fill", "pts": T(fill_circle(12, 4.6, 1.0)["pts"]), "closed": True})
    # link dashes to the right: connection + USB speed
    segs.append({"kind": "stroke", "pts": T([P(19.6, 9.0), P(22.6, 9.0)]), "closed": False, "width": 1.8})
    segs.append({"kind": "stroke", "pts": T([P(19.6, 13.0), P(21.6, 13.0)]), "closed": False, "width": 1.8})
    return segs

def logo_full_paths(cap_height=48.0):
    """Full lockup: symbol + MOBILADOR wordmark geometry (outline only, the app
    draws the real text with the UI font; the SVG uses vector letterforms)."""
    segs = list(logo_symbol_paths())
    # wordmark letters are emitted by svg_logo() as text; keep geometry minimal
    return segs

# --------------------------------------------------------------- svg emitter
def seg_to_svg_path(seg):
    pts = seg["pts"]
    d = "M " + " L ".join(f"{x:.2f} {y:.2f}" for (x, y) in pts)
    if seg.get("closed"):
        d += " Z"
    return d

def svg_icon(name, segs, size=24, color="#4C8DFF", stroke=1.75, bg=None, pad=0.0):
    body = []
    if bg:
        body.append(f'<rect x="0" y="0" width="{size}" height="{size}" rx="{size*0.22:.2f}" fill="{bg}"/>')
    for s in segs:
        w = s.get("width", stroke)
        if s["kind"] == "stroke":
            body.append(f'<path d="{seg_to_svg_path(s)}" fill="none" stroke="{color}" stroke-width="{w}" '
                        f'stroke-linecap="round" stroke-linejoin="round"/>')
        else:
            body.append(f'<path d="{seg_to_svg_path(s)}" fill="{color}" stroke="none"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
            f'viewBox="{pad} {pad} {size - 2*pad} {size - 2*pad}">\n  ' + "\n  ".join(body) + "\n</svg>\n")

def svg_logo(symbol_color="#4C8DFF", text_color="#0E1116", dark=False, height=96, with_text=True):
    segs = logo_symbol_paths()
    parts = []
    grad = ('<defs><linearGradient id="sym" x1="0" y1="0" x2="1" y2="1">'
            f'<stop offset="0%" stop-color="{symbol_color}"/>'
            f'<stop offset="100%" stop-color="{lighten(symbol_color, 0.18)}"/></linearGradient></defs>')
    for s in segs:
        w = s.get("width", 2.0)
        if s["kind"] == "stroke":
            parts.append(f'<path d="{seg_to_svg_path(s)}" fill="none" stroke="url(#sym)" stroke-width="{w}" '
                         f'stroke-linecap="round" stroke-linejoin="round"/>')
        else:
            parts.append(f'<path d="{seg_to_svg_path(s)}" fill="url(#sym)"/>')
    sym = '<g>' + "".join(parts) + '</g>'
    if with_text:
        txt = (f'<text x="40" y="19.5" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="15.6" '
               f'font-weight="600" letter-spacing="0.6" fill="{text_color}">MOBILADOR</text>'
               f'<text x="40.6" y="26.4" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="5.4" '
               f'letter-spacing="2.6" fill="{lighten(text_color, 0.45)}">USB LOW LATENCY MIRROR</text>')
        vb = "0 0 148 28"
        w, h = 148 * (height / 28.0), height
    else:
        txt = ""
        vb = "0 0 28 28"
        w = h = height
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="{h:.0f}" viewBox="{vb}">{grad}'
            f'{sym}{txt}</svg>\n')

def lighten(hexcol, k):
    hexcol = hexcol.lstrip("#")
    r, g, b = (int(hexcol[i:i+2], 16) for i in (0, 2, 4))
    r = int(r + (255 - r) * k); g = int(g + (255 - g) * k); b = int(b + (255 - b) * k)
    return f"#{r:02X}{g:02X}{b:02X}"

# ------------------------------------------------------------- C++ emitter
def emit_cpp_header(path):
    lines = []
    lines.append("// ============================================================================")
    lines.append("//  MOBILADOR - src/ui/Icons.generated.h")
    lines.append("//  GENERATED BY tools/gen_assets.py - DO NOT EDIT BY HAND.")
    lines.append("//  Vector icon library (24x24 grid, 1.75u stroke, round caps).")
    lines.append("// ============================================================================")
    lines.append("#pragma once")
    lines.append("#include \"../core/base.h\"")
    lines.append("")
    lines.append("namespace mob {")
    lines.append("")
    lines.append("enum IconId : int {")
    lines.append("    ICON_NONE = 0,")
    for name in ICON_ORDER:
        lines.append(f"    ICON_{name.upper()},")
    lines.append("    ICON_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("enum IconSegKind : u8 { ICON_SEG_STROKE = 0, ICON_SEG_FILL = 1 };")
    lines.append("")
    lines.append("struct IconPoint { f32 x, y; };")
    lines.append("struct IconSeg {")
    lines.append("    u8     kind;")
    lines.append("    u8     closed;")
    lines.append("    u16    first_point;")
    lines.append("    u16    point_count;")
    lines.append("};")
    lines.append("struct IconDef { u16 first_seg; u16 seg_count; };")
    lines.append("")

    all_pts = []
    all_segs = []
    defs = []
    for name in ICON_ORDER:
        segs = I[name]
        first_seg = len(all_segs)
        for s in segs:
            first_pt = len(all_pts)
            for (x, y) in s["pts"]:
                all_pts.append((x, y))
            all_segs.append((0 if s["kind"] == "stroke" else 1, 1 if s.get("closed") else 0,
                             first_pt, len(s["pts"])))
        defs.append((first_seg, len(all_segs) - first_seg))

    lines.append(f"static const IconPoint kIconPoints[{max(len(all_pts),1)}] = {{")
    for i in range(0, len(all_pts), 6):
        chunk = ", ".join(f"{{{x:.3f}f,{y:.3f}f}}" for (x, y) in all_pts[i:i+6])
        lines.append("    " + chunk + ",")
    if not all_pts:
        lines.append("    {0,0},")
    lines.append("};")
    lines.append("")
    lines.append(f"static const IconSeg kIconSegments[{max(len(all_segs),1)}] = {{")
    for (k, c, fp, pc) in all_segs:
        lines.append(f"    {{{k},{c},{fp},{pc}}},")
    if not all_segs:
        lines.append("    {0,0,0,0},")
    lines.append("};")
    lines.append("")
    lines.append(f"static const IconDef kIconDefs[ICON_COUNT] = {{")
    lines.append("    {0,0},")   # ICON_NONE
    for (fs, sc) in defs:
        lines.append(f"    {{{fs},{sc}}},")
    lines.append("};")
    lines.append("")
    lines.append("MOB_INLINE const IconSeg*   icon_segs(const IconDef* d) { return kIconSegments + d->first_seg; }")
    lines.append("MOB_INLINE const IconPoint* icon_pts(const IconDef*)    { return kIconPoints; }")
    lines.append("MOB_INLINE const IconDef*   icon_def(IconId id) {")
    lines.append("    if ((int)id <= 0 || (int)id >= ICON_COUNT) return nullptr;")
    lines.append("    return &kIconDefs[(int)id];")
    lines.append("}")
    lines.append("MOB_INLINE const char* icon_name(IconId id) {")
    lines.append("    static const char* names[ICON_COUNT] = {")
    lines.append("        \"none\",")
    for name in ICON_ORDER:
        lines.append(f"        \"{name}\",")
    lines.append("    };")
    lines.append("    return ((int)id > 0 && (int)id < ICON_COUNT) ? names[(int)id] : \"none\";")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace mob")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return len(all_pts), len(all_segs), len(ICON_ORDER)

# ------------------------------------------------------------------ rasterise
def rasterize(segs, px, stroke=1.75, color=(76, 141, 255, 255), ss=4, bg=None, filled_color=None):
    from PIL import Image, ImageDraw
    S = px * ss
    img = Image.new("RGBA", (S, S), bg if bg else (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    k = S / GRID
    for s in segs:
        pts = [(x * k, y * k) for (x, y) in s["pts"]]
        w = max(1, int(round(s.get("width", stroke) * k)))
        if s["kind"] == "stroke":
            if len(pts) == 1:
                r = w / 2.0
                dr.ellipse([pts[0][0] - r, pts[0][1] - r, pts[0][0] + r, pts[0][1] + r], fill=color)
                continue
            seq = pts + ([pts[0]] if s.get("closed") else [])
            dr.line(seq, fill=color, width=w, joint="curve")
            for p in seq:
                r = w / 2.0
                dr.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)
        else:
            if len(pts) < 3:
                if pts:
                    r = max(1.0, s.get("width", stroke) * k * 0.6)
                    dr.ellipse([pts[0][0] - r, pts[0][1] - r, pts[0][0] + r, pts[0][1] + r], fill=color)
                continue
            dr.polygon(pts, fill=color)
    return img.resize((px, px), Image.LANCZOS)

def main():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("Pillow is required:  pip install pillow", file=sys.stderr)
        return 2

    os.makedirs(os.path.join(ROOT, "src", "ui"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "assets", "icon"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "assets", "logo"), exist_ok=True)

    npts, nsegs, nicons = emit_cpp_header(os.path.join(ROOT, "src", "ui", "Icons.generated.h"))
    print(f"  icon library: {nicons} icons, {nsegs} segments, {npts} points -> src/ui/Icons.generated.h")

    # --- SVG for every icon (for docs / future web use)
    svg_dir = os.path.join(ROOT, "assets", "icon", "svg")
    os.makedirs(svg_dir, exist_ok=True)
    for name in ICON_ORDER:
        with open(os.path.join(svg_dir, f"{name}.svg"), "w") as f:
            f.write(svg_icon(name, I[name]))

    # --- icon sheet for review
    cols = 10
    cell = 56
    rows = (nicons + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell + 20, rows * cell + 20), (16, 19, 24, 255))
    for idx, name in enumerate(ICON_ORDER):
        im = rasterize(I[name], 32, color=(214, 222, 235, 255))
        x = 10 + (idx % cols) * cell
        y = 10 + (idx // cols) * cell
        sheet.paste(im, (x + 12, y + 4), im)
    sheet_path = os.path.join(ROOT, "assets", "icon", "icon-sheet.png")
    sheet.save(sheet_path)
    print(f"  icon sheet    -> assets/icon/icon-sheet.png")

    # --- application icon: rounded square + symbol
    def app_icon(px, dark=True):
        S = px * 4
        img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        dr = ImageDraw.Draw(img)
        # background: subtle vertical gradient using the app "graphite" tones
        top = (24, 28, 36) if dark else (255, 255, 255)
        bot = (12, 14, 19) if dark else (238, 241, 247)
        for y in range(S):
            t = y / max(1, S - 1)
            dr.line([(0, y), (S, y)], fill=(int(top[0] + (bot[0] - top[0]) * t),
                                            int(top[1] + (bot[1] - top[1]) * t),
                                            int(top[2] + (bot[2] - top[2]) * t), 255))
        # rounded mask
        mask = Image.new("L", (S, S), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
        img.putalpha(mask)
        # accent ring (1.5% of size) keeps the icon crisp on light taskbars
        ring = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(ring).rounded_rectangle([int(S*0.03), int(S*0.03), S-1-int(S*0.03), S-1-int(S*0.03)],
                                               radius=int(S * 0.20), outline=(76, 141, 255, 90),
                                               width=max(1, int(S * 0.006)))
        img.alpha_composite(ring)
        # symbol scaled into the safe area
        segs = logo_symbol_paths()
        sym_px = int(px * 0.66)
        sym = rasterize(segs, sym_px, stroke=2.0, color=(120, 176, 255, 255), ss=4)
        # paint the bolt in a brighter accent
        sym2 = rasterize(segs, sym_px, stroke=2.0, color=(255, 255, 255, 255), ss=4)
        # composite: draw sym2 only where the fill segs exist
        off = ((S - sym_px * 4) // 2, (S - sym_px * 4) // 2)
        img.alpha_composite(sym.resize((sym_px * 4, sym_px * 4), Image.LANCZOS), off)
        return img.resize((px, px), Image.LANCZOS)

    ico_path = os.path.join(ROOT, "assets", "icon", "mobilador.ico")
    base = app_icon(256, dark=True)
    base.save(ico_path, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
    for size, suffix in ((16, "16"), (32, "32"), (64, "64"), (128, "128"), (256, "256"), (512, "512")):
        app_icon(size, dark=True).save(os.path.join(ROOT, "assets", "icon", f"mobilador-{suffix}.png"))
        if size <= 256:
            app_icon(size, dark=False).save(os.path.join(ROOT, "assets", "icon", f"mobilador-light-{suffix}.png"))
    app_icon(64, dark=True).save(os.path.join(ROOT, "assets", "icon", "favicon.png"))
    print("  app icon      -> assets/icon/mobilador.ico (+ png set, favicon)")

    # --- logos
    with open(os.path.join(ROOT, "assets", "logo", "mobilador-symbol.svg"), "w") as f:
        f.write(svg_logo(symbol_color="#4C8DFF", height=256, with_text=False))
    with open(os.path.join(ROOT, "assets", "logo", "mobilador-logo-dark.svg"), "w") as f:
        f.write(svg_logo(symbol_color="#5C9BFF", text_color="#FFFFFF", dark=True, height=96))
    with open(os.path.join(ROOT, "assets", "logo", "mobilador-logo-light.svg"), "w") as f:
        f.write(svg_logo(symbol_color="#2F6FE4", text_color="#0E1116", height=96))

    # PNG lockup rendered from the same geometry
    def logo_png(height, dark, path, with_text=True):
        S = height * 4
        W = int(S * 5.2) if with_text else S
        img = Image.new("RGBA", (W, S), (0, 0, 0, 0))
        sym_px = height
        segs = logo_symbol_paths()
        sym = rasterize(segs, sym_px, stroke=2.0, color=(92, 155, 255, 255), ss=4)
        img.alpha_composite(sym.resize((sym_px * 4, sym_px * 4), Image.LANCZOS), (0, 0))
        if with_text:
            try:
                f1 = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", int(S * 0.52))
                f2 = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", int(S * 0.17))
            except Exception:
                f1 = f2 = ImageFont.load_default()
            dr = ImageDraw.Draw(img)
            tc = (255, 255, 255) if dark else (14, 17, 22)
            sc = (150, 160, 178) if dark else (110, 120, 138)
            dr.text((int(S * 1.42), int(S * 0.16)), "MOBILADOR", font=f1, fill=tc + (255,))
            dr.text((int(S * 1.46), int(S * 0.73)), "USB LOW LATENCY MIRROR", font=f2, fill=sc + (255,))
        img.save(path)

    logo_png(128, True, os.path.join(ROOT, "assets", "logo", "mobilador-logo-dark.png"))
    logo_png(128, False, os.path.join(ROOT, "assets", "logo", "mobilador-logo-light.png"))
    print("  logos         -> assets/logo/*.svg|png")

    # --- splash artwork
    sw, sh = 1920, 1080
    sp = Image.new("RGBA", (sw, sh), (10, 12, 16, 255))
    d = ImageDraw.Draw(sp)
    for y in range(sh):
        t = y / sh
        d.line([(0, y), (sw, y)], fill=(int(13 + 8 * t), int(16 + 9 * t), int(22 + 12 * t), 255))
    # accent glow, kept subtle (the product avoids neon excess)
    glow = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for i in range(46, 0, -1):
        r = i * 11
        a = int(2.4 * (46 - i) ** 1.15)
        gd.ellipse([sw * 0.5 - r * 1.9, sh * 0.62 - r, sw * 0.5 + r * 1.9, sh * 0.62 + r],
                   fill=(60, 120, 255, min(a, 26)))
    sp.alpha_composite(glow)
    sym = rasterize(logo_symbol_paths(), 200, stroke=2.0, color=(120, 176, 255, 255), ss=4)
    sp.alpha_composite(sym.resize((800, 800), Image.LANCZOS), (int(sw / 2 - 400), int(sh * 0.5 - 520)))
    try:
        fb = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 92)
        fs = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 30)
        d.text((sw / 2, sh * 0.52 + 190), "MOBILADOR", font=fb, fill=(255, 255, 255, 255), anchor="mm")
        d.text((sw / 2, sh * 0.52 + 262), "USB  •  ULTRA LOW LATENCY  •  FREE FIRE READY",
               font=fs, fill=(140, 152, 172, 255), anchor="mm")
        d.text((sw / 2, sh - 70), "v1.0.0", font=fs, fill=(96, 106, 124, 255), anchor="mm")
    except Exception:
        pass
    sp.convert("RGB").save(os.path.join(ROOT, "assets", "mobilador-splash.png"))
    print("  splash        -> assets/mobilador-splash.png")
    return 0

if __name__ == "__main__":
    sys.exit(main())
