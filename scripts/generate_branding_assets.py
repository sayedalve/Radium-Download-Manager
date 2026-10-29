# SPDX-License-Identifier: Apache-2.0
"""
generate_branding_assets.py - Reproducible asset generation from master SVG (assets/branding/RDM_Logo.svg)
Generates:
  - Multi-resolution Windows application icon: src/app/radium.ico (16, 24, 32, 48, 64, 128, 256)
  - Browser extension icons: extension/icons/icon-{16,32,48,128}.png (and dist/ if exists)
  - Documentation / branding fallback: docs/images/radium-logo.png
"""

import os
import io
import struct
from pathlib import Path
from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parent.parent
MASTER_SVG = REPO_ROOT / "assets" / "branding" / "RDM_Logo.svg"

# Polygon definitions from the master SVG viewBox (0 0 612 612)
# Colors:
#   st0: #0D0F14 (Deep charcoal background)
#   st1: #F2F6FA (Crisp white / silver chevron right wing)
#   st2: #FF4757 (Coral red chevron left wing)
POLYGONS = [
    # Top Chevron
    (
        [
            (88.73, 42.78), (189.68, 42.78), (306.0, 159.1), (422.32, 42.78),
            (422.32, 68.02), (306.0, 184.34), (189.68, 68.02), (149.08, 68.02),
            (306.0, 224.94), (306.0, 260.06)
        ],
        (255, 71, 87, 255)
    ),
    (
        [
            (422.32, 42.78), (523.27, 42.78), (306.0, 260.06), (306.0, 224.94),
            (462.92, 68.02), (422.32, 68.02)
        ],
        (242, 246, 250, 255)
    ),
    # Middle Chevron
    (
        [
            (88.73, 202.36), (189.68, 202.36), (306.0, 318.68), (422.32, 202.36),
            (422.32, 227.6), (306.0, 343.92), (189.68, 227.6), (149.08, 227.6),
            (306.0, 384.52), (306.0, 419.64)
        ],
        (255, 71, 87, 255)
    ),
    (
        [
            (422.32, 202.36), (523.27, 202.36), (306.0, 419.64), (306.0, 384.52),
            (462.92, 227.6), (422.32, 227.6)
        ],
        (242, 246, 250, 255)
    ),
    # Bottom Chevron
    (
        [
            (88.73, 361.94), (189.68, 361.94), (306.0, 478.26), (422.32, 361.94),
            (422.32, 387.18), (306.0, 503.5), (189.68, 387.18), (149.08, 387.18),
            (306.0, 544.1), (306.0, 579.22)
        ],
        (255, 71, 87, 255)
    ),
    (
        [
            (422.32, 361.94), (523.27, 361.94), (306.0, 579.22), (306.0, 544.1),
            (462.92, 387.18), (422.32, 387.18)
        ],
        (242, 246, 250, 255)
    )
]

def render_logo(size: int, bg_color=(13, 15, 20, 255)) -> Image.Image:
    # 4x supersampling for high fidelity sub-pixel antialiasing
    scale = (size * 4) / 612.0
    s4 = size * 4
    im = Image.new("RGBA", (s4, s4), (0, 0, 0, 0))
    draw = ImageDraw.Draw(im)

    if bg_color is not None:
        draw.rectangle([0, 0, s4, s4], fill=bg_color)

    for pts, col in POLYGONS:
        scaled_pts = [(x * scale, y * scale) for x, y in pts]
        draw.polygon(scaled_pts, fill=col)

    return im.resize((size, size), Image.Resampling.LANCZOS)

def create_ico(output_path: Path, sizes=(16, 24, 32, 48, 64, 128, 256)):
    png_datas = []
    for s in sizes:
        im = render_logo(s, bg_color=(13, 15, 20, 255))
        buf = io.BytesIO()
        im.save(buf, format="PNG")
        png_datas.append(buf.getvalue())

    # Build multi-resolution ICO header & directory
    header = struct.pack("<HHH", 0, 1, len(sizes))
    entries = []
    offset = 6 + len(sizes) * 16

    for i, s in enumerate(sizes):
        data = png_datas[i]
        w = 0 if s == 256 else s
        h = 0 if s == 256 else s
        entry = struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(data), offset)
        entries.append(entry)
        offset += len(data)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(header)
        for e in entries:
            f.write(e)
        for d in png_datas:
            f.write(d)
    print(f"  [+] Generated multi-res ICO ({', '.join(str(s) for s in sizes)}): {output_path}")

def main():
    print("==> Generating reproducible branding assets from master SVG...")
    if not MASTER_SVG.exists():
        raise FileNotFoundError(f"Master SVG not found at {MASTER_SVG}")

    # 1. Main Windows PE Application Icon
    ico_path = REPO_ROOT / "src" / "app" / "radium.ico"
    create_ico(ico_path)

    # 2. Browser Extension PNGs
    ext_dir = REPO_ROOT / "extension" / "icons"
    ext_dir.mkdir(parents=True, exist_ok=True)
    for s in (16, 32, 48, 128):
        out_file = ext_dir / f"icon-{s}.png"
        render_logo(s, bg_color=(13, 15, 20, 255)).save(out_file)
        print(f"  [+] Generated extension icon: {out_file}")

    dist_ext_dir = REPO_ROOT / "dist" / "extension" / "icons"
    if dist_ext_dir.parent.exists():
        dist_ext_dir.mkdir(parents=True, exist_ok=True)
        for s in (16, 32, 48, 128):
            out_file = dist_ext_dir / f"icon-{s}.png"
            render_logo(s, bg_color=(13, 15, 20, 255)).save(out_file)

    # 3. Docs Branding Fallback
    doc_logo = REPO_ROOT / "docs" / "images" / "radium-logo.png"
    doc_logo.parent.mkdir(parents=True, exist_ok=True)
    render_logo(256, bg_color=(13, 15, 20, 255)).save(doc_logo)
    print(f"  [+] Generated documentation logo: {doc_logo}")

    print("==> All branding assets generated successfully.")

if __name__ == "__main__":
    main()
