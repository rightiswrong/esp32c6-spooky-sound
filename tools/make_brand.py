"""Draw the integration's brand icon: a jack-o'-lantern whose mouth is a sound
waveform, with sound waves rolling off one side. Renders at 4x and
downsamples for smooth edges.

    python3 tools/make_brand.py
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parent.parent / "custom_components" / "spooky_sound" / "brand"


def draw(size: int) -> Image.Image:
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = s * 0.44, s * 0.57

    # Pumpkin body: five overlapping lobes, darker at the seams
    for dx, rx, col in [(-0.27, 0.18, "#b84a00"), (0.27, 0.18, "#b84a00"), (-0.14, 0.23, "#e2680c"),
                        (0.14, 0.23, "#e2680c"), (0.0, 0.25, "#f7841c")]:
        x = cx + dx * s
        d.ellipse([x - rx * s, cy - 0.33 * s, x + rx * s, cy + 0.33 * s], fill=col)
    d.rounded_rectangle([cx - 0.035 * s, cy - 0.43 * s, cx + 0.045 * s, cy - 0.29 * s], radius=0.02 * s, fill="#4d6b1f")

    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    eye_y = cy - 0.08 * s
    for ex in (-0.13, 0.13):
        x = cx + ex * s
        g.polygon([(x - 0.075 * s, eye_y + 0.05 * s), (x + 0.075 * s, eye_y + 0.05 * s), (x, eye_y - 0.075 * s)],
                  fill="#ffe28a")
    # Mouth: a glowing waveform — a thunderclap spike in the middle
    pts = []
    for i in range(41):
        u = i / 40
        x = cx + (u - 0.5) * 0.42 * s
        env = math.exp(-((u - 0.5) / 0.16) ** 2)
        y = cy + 0.13 * s - math.sin(u * math.pi * 9) * (0.012 + 0.06 * env) * s
        pts.append((x, y))
    g.line(pts, fill="#fff1b8", width=int(0.022 * s), joint="curve")

    # Sound waves rolling off to the right
    for k, alpha in ((0, 255), (1, 190), (2, 120)):
        r = (0.40 + 0.09 * k) * s
        box = [cx - r, cy - r * 0.95, cx + r, cy + r * 0.95]
        g.arc(box, start=-38, end=38, fill=(178, 120, 255, alpha), width=int(0.026 * s))

    halo = glow.filter(ImageFilter.GaussianBlur(0.03 * s))
    img = Image.alpha_composite(img, halo)
    img = Image.alpha_composite(img, halo)
    img = Image.alpha_composite(img, glow)
    return img.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    draw(256).save(OUT / "icon.png", optimize=True)
    draw(512).save(OUT / "icon@2x.png", optimize=True)
    print("wrote", *sorted(p.name for p in OUT.iterdir()))
