#!/usr/bin/env python3
"""Render the Slow Ops Looper skin graphics: page backgrounds, labelled touch buttons,
choice segments and the beat boxes.

Style: dark charcoal pages with a faint violet/indigo wash, a card with a gradient
border behind each group of controls, and tinted "pill" buttons with gradient
borders, spaced capitals and a status dot (lit = stronger tint and a brighter rim).

Text is drawn into the images (no Label on top of a Button), so nothing can
swallow taps. Output: plugin/assets/buttons/*.png, committed to the repo;
scripts/regen_skins.sh copies them into plugin/skins/ and points each page at its
background (bg_<page>.png).

Needs Pillow (only when changing the graphics):
    python3 -m venv /tmp/venv && /tmp/venv/bin/pip install pillow
    /tmp/venv/bin/python scripts/make_buttons.py
Font: Titillium Web (SIL OFL 1.1, assets/fonts/OFL.txt), the MPC's own UI face.
"""
import os
from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "plugin", "assets", "buttons")
BOLD = os.path.join(ROOT, "assets", "fonts", "TitilliumWeb-Bold.ttf")
SEMI = os.path.join(ROOT, "assets", "fonts", "TitilliumWeb-SemiBold.ttf")

W, H = 248, 84                 # button size (native pixels; the MPC never scales images)
SMALL_H, HALF_W = 64, 120      # the shared OVERDUB / SOLO / MUTE slot, and half-width buttons
SS = 3                         # supersampling for smooth edges

# ---- palette ------------------------------------------------------------------------
PAGE_TOP = (0x15, 0x15, 0x1c)
PAGE_BOTTOM = (0x0f, 0x10, 0x15)
PANEL = (0x1a, 0x1b, 0x23)     # card fill; buttons sit on it
WHITE = (0xff, 0xff, 0xff)
TEXT = (0xec, 0xec, 0xf2)
DIM_TEXT = (0x86, 0x86, 0x92)
HEADER = (0x8e, 0x8f, 0x9e)

VIOLET = (0x8b, 0x5c, 0xf6)
INDIGO = (0x63, 0x66, 0xf1)
CYAN = (0x22, 0xd3, 0xee)
ORANGE = (0xfb, 0x92, 0x3c)
RED = (0xf4, 0x3f, 0x5e)
GREEN = (0x34, 0xd3, 0x99)
PURPLE = (0xa8, 0x55, 0xf7)
AMBER = (0xfb, 0xbf, 0x24)
BLUE = (0x60, 0xa5, 0xfa)
GREY = (0x8b, 0x8b, 0x98)


def mix(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


# ---- drawing helpers (all at supersampled size) ------------------------------------------
def gradient(w, h, c1, c2, direction="diag"):
    """RGBA image, c1 at the top-left (or top) to c2 at the bottom-right (or bottom)."""
    n = 64
    m = Image.new("L", (n, n))
    px = m.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2 * (n - 1)) if direction == "diag" else y / (n - 1)
            px[x, y] = int(round(255 * t))
    m = m.resize((w, h), Image.BILINEAR)
    a = Image.new("RGBA", (w, h), c1 if len(c1) == 4 else c1 + (255,))
    b = Image.new("RGBA", (w, h), c2 if len(c2) == 4 else c2 + (255,))
    return Image.composite(b, a, m)


def rounded_mask(w, h, box, radius):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def card(img, box, radius, border_w, fill_top, fill_bottom, border1, border2):
    """Rounded rectangle with a vertical fill gradient and a diagonal gradient border."""
    w, h = img.size
    x0, y0, x1, y1 = box
    outer = rounded_mask(w, h, box, radius)
    inner = rounded_mask(w, h, (x0 + border_w, y0 + border_w, x1 - border_w, y1 - border_w), max(1, radius - border_w))
    fill = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    fill.paste(gradient(x1 - x0, y1 - y0, fill_top, fill_bottom, "vertical"), (x0, y0))
    img.alpha_composite(Image.composite(fill, Image.new("RGBA", (w, h), (0, 0, 0, 0)), inner))
    rim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rim.paste(gradient(x1 - x0, y1 - y0, border1, border2, "diag"), (x0, y0))
    img.alpha_composite(Image.composite(rim, Image.new("RGBA", (w, h), (0, 0, 0, 0)), ImageChops.subtract(outer, inner)))


def spaced(d, xy, text, font, fill, spacing, anchor="mm"):
    """Text with letter spacing (PIL has none), centred on xy."""
    widths = [font.getlength(ch) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x = xy[0] - total / 2 if anchor == "mm" else xy[0]
    for ch, cw in zip(text, widths):
        d.text((x, xy[1]), ch, font=font, fill=fill, anchor="lm")
        x += cw + spacing
    return total


def fit(path, text, size, max_w, spacing):
    while size > 10:
        f = ImageFont.truetype(path, size * SS)
        if sum(f.getlength(c) for c in text) + spacing * SS * (len(text) - 1) <= max_w * SS:
            return f, size
        size -= 1
    return ImageFont.truetype(path, size * SS), size


def alpha(c, a):
    return tuple(c[:3]) + (a,)


def save(img, w, h, path):
    img.resize((w, h), Image.LANCZOS).save(path)


# ---- buttons ------------------------------------------------------------------------------
def pill(path, title, sub, accent, lit, w=W, h=H, flash=False, dim_title=False, icon=None):
    """A tinted button: off = faint tint and a gradient rim, lit = stronger tint and a bright rim.
    `sub` (a state word) is drawn small, in the accent colour, after a status dot."""
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    k = 0.58 if flash else (0.40 if lit else 0.10)
    top, bottom = mix(PANEL, accent, k), mix(PANEL, accent, k * 0.62)
    if lit or flash:
        b1, b2 = alpha(mix(accent, WHITE, 0.45), 255), alpha(accent, 235)
    else:
        b1, b2 = alpha(accent, 210), alpha(mix(accent, INDIGO, 0.55), 90)
    r = min(14, h // 2) * SS
    card(img, (SS, SS, w * SS - SS, h * SS - SS), r, 2 * SS, top, bottom, b1, b2)
    d = ImageDraw.Draw(img)
    if lit or flash:   # a faint highlight along the top edge
        d.line([(r, 4 * SS), (w * SS - r, 4 * SS)], fill=(255, 255, 255, 38), width=SS)
    tcol = WHITE if (lit or flash) else (DIM_TEXT if dim_title else TEXT)
    scol = WHITE if (lit or flash) else mix(accent, WHITE, 0.2)
    if icon:
        cx, cy, ir = w * SS / 2, (h * 0.38) * SS, 13 * SS
        if icon == "play":
            d.polygon([(cx - ir * 0.75, cy - ir), (cx - ir * 0.75, cy + ir), (cx + ir, cy)], fill=tcol)
        else:
            d.rounded_rectangle([cx - ir * 0.8, cy - ir * 0.8, cx + ir * 0.8, cy + ir * 0.8], radius=3 * SS, fill=tcol)
    if title:
        tf, _ = fit(BOLD, title, 25 if h >= 80 else 22, w - 20, 1.5)
        ty = (h * 0.40 if sub else h * 0.5) * SS
        spaced(d, (w * SS / 2, ty), title, tf, tcol, 1.5 * SS)
    if sub:
        sf, ssz = fit(SEMI, sub, 14, w - 34, 1.5)
        sy = (h * 0.74 if (title or icon) else h * 0.5) * SS
        if icon:
            sy = h * 0.76 * SS
        tw = spaced(d, (w * SS / 2 + 6 * SS, sy), sub, sf, scol, 1.5 * SS)
        dx = w * SS / 2 + 6 * SS - tw / 2 - 9 * SS
        dot = 3.5 * SS
        d.ellipse([dx - dot, sy - dot, dx + dot, sy + dot], fill=scol if (lit or flash) else accent)
    save(img, w, h, path)


SEG_H = 52
SEGMENTS = [(c, 86) for c in ("Last Bar", "Rolling", "Next Bar", "Freeze", "On Demand", "Continuous")] + \
           [(c, 64) for c in ("1 Bar", "2 Bars", "4 Bars", "8 Bars")] + \
           [(c, 86) for c in ("Immediate", "Bar End", "Loop End")]


def slug(caption):
    return caption.lower().replace(" ", "")


def seg(path, caption, width, on):
    """Radio segment. Off: quiet dark chip. On: the primary-action look (violet fill, violet-to-indigo rim)."""
    img = Image.new("RGBA", (width * SS, SEG_H * SS), (0, 0, 0, 0))
    if on:
        card(img, (SS, SS, width * SS - SS, SEG_H * SS - SS), 10 * SS, 2 * SS,
             (0x4c, 0x2f, 0x88), (0x32, 0x29, 0x6e), alpha((0xc0, 0x84, 0xfc), 255), alpha(INDIGO, 255))
    else:
        card(img, (SS, SS, width * SS - SS, SEG_H * SS - SS), 10 * SS, 2 * SS,
             (0x20, 0x21, 0x2b), (0x1b, 0x1c, 0x25), alpha((0x4a, 0x4b, 0x5c), 255), alpha((0x2c, 0x2d, 0x3a), 255))
    d = ImageDraw.Draw(img)
    f, _ = fit(BOLD if on else SEMI, caption, 16, width - 10, 0.5)
    spaced(d, (width * SS / 2, SEG_H * SS / 2), caption, f, WHITE if on else (0xbc, 0xbc, 0xc8), 0.5 * SS)
    save(img, width, SEG_H, path)


def beat(path, accent, on):
    """Beat box (248 x 30), a full pill. The beat number is a Label drawn over it by the MPC."""
    w, h = 248, 30
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    if on:
        card(img, (SS, SS, w * SS - SS, h * SS - SS), 14 * SS, 2 * SS, mix(accent, WHITE, 0.05), mix(accent, PANEL, 0.25),
             alpha(mix(accent, WHITE, 0.5), 255), alpha(accent, 255))
    else:
        card(img, (SS, SS, w * SS - SS, h * SS - SS), 14 * SS, 2 * SS, mix(PAGE_TOP, accent, 0.10), mix(PAGE_TOP, accent, 0.05),
             alpha(accent, 120), alpha(mix(accent, INDIGO, 0.5), 50))
    save(img, w, h, path)


# ---- page backgrounds ---------------------------------------------------------------------
PAGE_W, PAGE_H = 1280, 628
SLOT_X, SLOT_Y = [23, 343, 663, 983], [45, 196, 346, 497]   # the generated 4 x 4 grid (gen_ui)


def glow(w, h, cx, cy, rx, ry, colour, strength):
    """Soft elliptical glow, computed small and scaled up."""
    n = 96
    sw, sh = n, max(2, int(n * h / w))
    m = Image.new("L", (sw, sh))
    px = m.load()
    for y in range(sh):
        for x in range(sw):
            dx = (x / (sw - 1) * w - cx) / rx
            dy = (y / (sh - 1) * h - cy) / ry
            v = max(0.0, 1.0 - (dx * dx + dy * dy) ** 0.5)
            px[x, y] = int(255 * strength * v * v)
    m = m.resize((w, h), Image.BICUBIC)
    layer = Image.new("RGBA", (w, h), colour + (255,))
    layer.putalpha(m)
    return layer


def background(path, cards):
    """cards: (col0, col1, row0, row1, header or None, top_y or None)."""
    s = 2
    w, h = PAGE_W * s, PAGE_H * s
    img = gradient(w, h, PAGE_TOP, PAGE_BOTTOM, "vertical")
    img.alpha_composite(glow(w, h, w * 0.18, -h * 0.25, w * 0.55, h * 0.75, (0x6d, 0x3a, 0xd8), 0.22))
    img.alpha_composite(glow(w, h, w * 0.92, -h * 0.20, w * 0.45, h * 0.65, (0x3b, 0x4f, 0xd8), 0.16))
    d = ImageDraw.Draw(img)
    hf = ImageFont.truetype(SEMI, 15 * s)
    for c0, c1, r0, r1, header, top in cards:
        x0 = SLOT_X[c0] - 4
        x1 = SLOT_X[c1] + 282
        y0 = top if top is not None else (8 if r0 == 0 else SLOT_Y[r0] - 2)
        y1 = min(PAGE_H - 8, SLOT_Y[r1] + 138)   # leaves a gap above a card that starts on the next row
        card(img, (x0 * s, y0 * s, x1 * s, y1 * s), 16 * s, int(1.5 * s),
             alpha(mix(PANEL, VIOLET, 0.04), 238), alpha(PANEL, 238),
             alpha(mix(VIOLET, WHITE, 0.15), 150), alpha(INDIGO, 40))
        if header:
            spaced(d, ((x0 + 18) * s, (y0 + 20) * s), header, hf, HEADER, 2.2 * s, anchor="lm")
    img.convert("RGB").resize((PAGE_W, PAGE_H), Image.LANCZOS).save(path)


def main():
    os.makedirs(OUT, exist_ok=True)
    # CAPTURE n: one look per Status choice (No Hist, Ready, Armed, Rec Bar, Playing, Overdub, Too Long).
    # "off" is the resting look; "on" is the brief flash while the tap is being taken.
    capture = [
        ("nohist", "NO HISTORY", GREY, False),
        ("ready", "READY", CYAN, False),
        ("armed", "WAITING FOR 1", ORANGE, True),
        ("rec", "RECORDING", RED, True),
        ("play", "PLAYING", GREEN, True),
        ("dub", "OVERDUB", PURPLE, True),
        ("toolong", "TOO LONG", RED, False),
    ]
    for name, sub, accent, lit in capture:
        pill(os.path.join(OUT, "capture_%s_off.png" % name), "CAPTURE", sub, accent, lit, dim_title=(name == "nohist"))
        pill(os.path.join(OUT, "capture_%s_on.png" % name), "CAPTURE", sub, accent, True, flash=True)

    # METRONOME: one look per Metronome state (Off, Standby, On); a tap steps Off -> On -> Standby -> Off.
    # Standby = silenced by Auto Standby, back On at the next Clear All (or when the last loop is cleared).
    for name, sub, accent, lit in [("off", "OFF", GREY, False), ("standby", "STANDBY", AMBER, False), ("on", "ON", CYAN, True)]:
        pill(os.path.join(OUT, "metro_%s_off.png" % name), "METRONOME", sub, accent, lit)
        pill(os.path.join(OUT, "metro_%s_on.png" % name), "METRONOME", sub, accent, True, flash=True)

    # Latched / one-shot buttons: off = faint tint, on = lit.
    # OVERDUB n shares its LOOPS slot with SOLO n / MUTE n: OVERDUB 248x64 on top, SOLO and MUTE 120x64 below.
    # CLEAR n / CLEAR ALL share the bottom row with PLAY/STOP n / PLAY/STOP ALL: 120x84 each.
    for name, title, accent, w, h in [("overdub", "OVERDUB", PURPLE, W, SMALL_H), ("clear", "CLEAR", GREY, HALF_W, H),
                                      ("solo", "SOLO", AMBER, HALF_W, SMALL_H), ("mute", "MUTE", BLUE, HALF_W, SMALL_H)]:
        pill(os.path.join(OUT, "%s_off.png" % name), title, None, accent, False, w, h)
        pill(os.path.join(OUT, "%s_on.png" % name), title, None, accent, True, w, h)
    pill(os.path.join(OUT, "clearall_off.png"), "CLEAR", "ALL", RED, False, HALF_W, H)
    pill(os.path.join(OUT, "clearall_on.png"), "CLEAR", "ALL", RED, True, HALF_W, H, flash=True)
    # PLAY/STOP n and PLAY/STOP ALL: one look per Play State (Stopped, Playing, Starting, Stopping).
    # Starting / Stopping = the tap is waiting for the Play Timing boundary (bar line or loop end).
    for name, prefix in [("play", ""), ("playall", "ALL ")]:
        for state, label, accent, lit, icon in [("stopped", "STOPPED", GREY, False, "stop"), ("playing", "PLAYING", GREEN, True, "play"),
                                                ("starting", "STARTING", AMBER, False, "play"), ("stopping", "STOPPING", AMBER, False, "stop")]:
            pill(os.path.join(OUT, "%s_%s.png" % (name, state)), None, prefix + label, accent, lit, HALF_W, H, icon=icon)
    # Segment (radio) buttons for the choice parameters: one image pair per caption.
    for caption, width in SEGMENTS:
        seg(os.path.join(OUT, "seg_%s_%d_off.png" % (slug(caption), width)), caption, width, False)
        seg(os.path.join(OUT, "seg_%s_%d_on.png" % (slug(caption), width)), caption, width, True)
    # Beat boxes, one pair per Loop State look (Idle, Armed, Rec, Play, Dub).
    for name, accent in [("idle", CYAN), ("armed", ORANGE), ("rec", RED), ("play", GREEN), ("dub", PURPLE)]:
        beat(os.path.join(OUT, "beat_%s_on.png" % name), accent, True)
        beat(os.path.join(OUT, "beat_%s_off.png" % name), accent, False)
    # Page backgrounds: a card behind each group of controls (cells of the 4 x 4 grid).
    background(os.path.join(OUT, "bg_loops.png"), [(c, c, 0, 3, None, 44) for c in range(4)])
    background(os.path.join(OUT, "bg_mix.png"), [(0, 2, 0, 0, "LOOP VOLUME", None), (0, 0, 1, 1, None, None), (2, 2, 1, 3, None, None),
                                                (3, 3, 0, 3, "OUTPUT", None)])
    background(os.path.join(OUT, "bg_sound.png"), [(c, c, 0, 1, "BANK %d" % (c + 1), None) for c in range(3)])
    background(os.path.join(OUT, "bg_debug.png"), [(0, 3, 0, 2, "DIAGNOSTICS", None)])
    print("wrote", len(os.listdir(OUT)), "images to", OUT)


if __name__ == "__main__":
    main()
