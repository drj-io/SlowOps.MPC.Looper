#!/usr/bin/env python3
"""Render the labelled touch buttons for the Slow Ops Looper skin.

Text is drawn into the images (no Label on top of a Button), so nothing can
swallow taps. Output: plugin/assets/buttons/*.png, committed to the repo;
scripts/regen_skins.sh copies them into plugin/skins/.

Needs Pillow (only when changing the graphics):
    python3 -m venv /tmp/venv && /tmp/venv/bin/pip install pillow
    /tmp/venv/bin/python scripts/make_buttons.py
Font: Titillium Web (SIL OFL 1.1, assets/fonts/OFL.txt), the MPC's own UI face.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "plugin", "assets", "buttons")
BOLD = os.path.join(ROOT, "assets", "fonts", "TitilliumWeb-Bold.ttf")
SEMI = os.path.join(ROOT, "assets", "fonts", "TitilliumWeb-SemiBold.ttf")

W, H, R = 248, 84, 14          # button size (native pixels; the MPC never scales images) and corner radius
SS = 3                         # supersampling for smooth edges

DARK = (0x26, 0x26, 0x29)
TEXT = (0xff, 0xff, 0xff)
DIM_TEXT = (0x8a, 0x8a, 0x90)

# Same palette as the beat boxes.
CYAN = (0x19, 0xc3, 0xe6)
ORANGE = (0xff, 0x8c, 0x1a)
RED = (0xe8, 0x2a, 0x2a)
GREEN = (0x2e, 0xc8, 0x4e)
PURPLE = (0xa0, 0x4c, 0xe8)
AMBER = (0xf2, 0xc2, 0x1b)
BLUE = (0x3a, 0x7b, 0xf0)
GREY = (0x70, 0x70, 0x76)


def mix(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def button(path, title, sub, fill, outline, title_colour=TEXT, sub_colour=None):
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([SS, SS, W * SS - SS - 1, H * SS - SS - 1], radius=R * SS, fill=fill + (255,),
                        outline=outline + (255,), width=3 * SS)
    big = ImageFont.truetype(BOLD, 30 * SS)
    small = ImageFont.truetype(SEMI, 17 * SS)
    if sub:
        d.text((W * SS / 2, 33 * SS), title, font=big, fill=title_colour, anchor="mm")
        d.text((W * SS / 2, 62 * SS), sub, font=small, fill=sub_colour or title_colour, anchor="mm")
    else:
        d.text((W * SS / 2, H * SS / 2), title, font=big, fill=title_colour, anchor="mm")
    img.resize((W, H), Image.LANCZOS).save(path)


SEG_H = 52
SEGMENTS = [(c, 86) for c in ("Last Bar", "Rolling", "Next Bar", "Freeze", "On Demand", "Continuous")] + \
           [(c, 64) for c in ("1 Bar", "2 Bars", "4 Bars", "8 Bars")]


def slug(caption):
    return caption.lower().replace(" ", "")


def seg(path, caption, width, on):
    img = Image.new("RGBA", (width * SS, SEG_H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fill = CYAN if on else DARK
    outline = mix(CYAN, TEXT, 0.4) if on else mix(CYAN, DARK, 0.55)
    d.rounded_rectangle([SS, SS, width * SS - SS - 1, SEG_H * SS - SS - 1], radius=10 * SS, fill=fill + (255,),
                        outline=outline + (255,), width=2 * SS)
    size = 17
    font = ImageFont.truetype(SEMI, size * SS)
    while font.getlength(caption) > (width - 8) * SS and size > 11:
        size -= 1
        font = ImageFont.truetype(SEMI, size * SS)
    d.text((width * SS / 2, SEG_H * SS / 2), caption, font=font, fill=(0x10, 0x10, 0x10) if on else TEXT, anchor="mm")
    img.resize((width, SEG_H), Image.LANCZOS).save(path)


def main():
    os.makedirs(OUT, exist_ok=True)
    # CAPTURE n: one look per Status choice (No Hist, Ready, Armed, Rec Bar, Playing, Overdub, Too Long).
    # "off" is the resting look; "on" is the brief flash while the tap is being taken.
    capture = [
        ("nohist", "NO HISTORY", DARK, GREY, DIM_TEXT),
        ("ready", "READY", DARK, CYAN, CYAN),
        ("armed", "WAITING FOR 1", ORANGE, mix(ORANGE, TEXT, 0.4), TEXT),
        ("rec", "RECORDING", RED, mix(RED, TEXT, 0.4), TEXT),
        ("play", "PLAYING", GREEN, mix(GREEN, TEXT, 0.4), TEXT),
        ("dub", "OVERDUB", PURPLE, mix(PURPLE, TEXT, 0.4), TEXT),
        ("toolong", "TOO LONG", DARK, RED, RED),
    ]
    for name, sub, fill, outline, sub_colour in capture:
        title_colour = DIM_TEXT if name == "nohist" else TEXT
        button(os.path.join(OUT, "capture_%s_off.png" % name), "CAPTURE", sub, fill, outline, title_colour, sub_colour)
        button(os.path.join(OUT, "capture_%s_on.png" % name), "CAPTURE", sub, mix(fill, TEXT, 0.35), TEXT, TEXT, TEXT)

    # Latched / one-shot buttons: off = dark with a coloured rim, on = filled.
    for name, title, colour in [("overdub", "OVERDUB", PURPLE), ("clear", "CLEAR", GREY),
                                ("solo", "SOLO", AMBER), ("mute", "MUTE", BLUE)]:
        button(os.path.join(OUT, "%s_off.png" % name), title, None, DARK, mix(colour, DARK, 0.25), TEXT)
        button(os.path.join(OUT, "%s_on.png" % name), title, None, colour, mix(colour, TEXT, 0.4),
               (0x10, 0x10, 0x10) if name == "solo" else TEXT)
    # Segment (radio) buttons for the choice parameters: one image pair per caption.
    # 3-way: 86 px wide; 4-way: 64 px. Off = dark, on = cyan fill.
    for caption, width in SEGMENTS:
        seg(os.path.join(OUT, "seg_%s_%d_off.png" % (slug(caption), width)), caption, width, False)
        seg(os.path.join(OUT, "seg_%s_%d_on.png" % (slug(caption), width)), caption, width, True)
    print("wrote", len(os.listdir(OUT)), "images to", OUT)


if __name__ == "__main__":
    main()
