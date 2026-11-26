"""Generate the original mRSS landscape/RSS icon (requires Pillow).

SPDX-License-Identifier: GPL-3.0-only
Copyright (C) 2026 MorpheusUK. All Rights Reserved.
"""

from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)

# Draw at 4x the largest icon size for smooth edges at every Windows DPI.
SCALE = 4
image = Image.new("RGBA", (256 * SCALE, 256 * SCALE))
draw = ImageDraw.Draw(image)


def box(values):
    return tuple(round(value * SCALE) for value in values)


draw.rounded_rectangle(box((8, 8, 248, 248)), radius=48 * SCALE, fill="#12436B")
draw.rounded_rectangle(box((33, 65, 223, 210)), radius=16 * SCALE, fill="#EEF7FF")
draw.rounded_rectangle(box((44, 76, 212, 199)), radius=8 * SCALE, fill="#388FC0")
draw.ellipse(box((62, 92, 94, 124)), fill="#FFE19A")
draw.polygon(box((44, 178, 102, 122, 155, 178, 181, 153, 212, 181, 212, 199, 44, 199)), fill="#103D62")
draw.polygon(box((44, 190, 91, 158, 142, 199, 44, 199)), fill="#57B3AD")
# Distinct orange RSS badge, on a dark separation ring.
draw.ellipse(box((132, 21, 236, 125)), fill="#12436B")
draw.ellipse(box((140, 29, 228, 117)), fill="#F58C28")
draw.ellipse(box((156, 86, 168, 98)), fill="white")
for radius in (29, 48):
    draw.arc(box((162 - radius, 92 - radius, 162 + radius, 92 + radius)),
             start=270, end=360, fill="white", width=8 * SCALE)

image = image.resize((256, 256), Image.Resampling.LANCZOS)
image.save(ASSETS / "mrss.png")
image.save(ASSETS / "mrss.ico", sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
