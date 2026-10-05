"""Render the app's original vector mark into PNG and multi-resolution Windows ICO.

The SVG and raster files share the same geometry and palette. Requires Pillow.
No external images, fonts, rendering service, or game branding are used.
"""
from io import BytesIO
from pathlib import Path
import struct

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "src/DcsVr.App/Assets"
UI = ROOT / "src/DcsVr.App/ui"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
JET = [(256, 112), (277, 216), (376, 286), (376, 316), (278, 280),
       (278, 339), (307, 363), (307, 383), (256, 362), (205, 383),
       (205, 363), (234, 339), (234, 280), (136, 316), (136, 286), (235, 216)]
BRACKETS = ["M168 104H132Q104 104 104 132V168", "M344 104H380Q408 104 408 132V168",
            "M104 344V380Q104 408 132 408H168", "M408 344V380Q408 408 380 408H344"]
# Explicit sampled curves also render on platforms without an SVG library.
CORNER = [(168, 104), (132, 104)] + [((1-t)**2*132+2*(1-t)*t*104+t*t*104,
          (1-t)**2*104+2*(1-t)*t*104+t*t*132) for t in [i/32 for i in range(1, 33)]] + [(104, 168)]


def gradient(size, top, bottom):
    image = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(image)
    a, b = tuple(bytes.fromhex(top)), tuple(bytes.fromhex(bottom))
    for y in range(size):
        color = tuple(round(x+(z-x)*y/(size-1)) for x, z in zip(a, b)) + (255,)
        draw.line((0, y, size, y), fill=color)
    return image


def render(size):
    # Supersampling preserves the nose, swept wings and focus brackets at 16 px.
    working = max(512, size*4)
    scale = working/512
    xy = lambda point: tuple(round(v*scale) for v in point)
    mask = Image.new("L", (working, working))
    ImageDraw.Draw(mask).rounded_rectangle((*xy((16, 16)), *xy((496, 496))), radius=88*scale, fill=255)
    image = gradient(working, "343434", "191919")
    image.putalpha(mask)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((*xy((17, 17)), *xy((495, 495))), radius=87*scale,
                           outline="#4a4a4a", width=max(1, round(3*scale)))
    for flip_x, flip_y in ((False, False), (True, False), (False, True), (True, True)):
        points = [xy((512-x if flip_x else x, 512-y if flip_y else y)) for x, y in CORNER]
        draw.line(points, fill="#ba7b25", width=round(12*scale), joint="curve")
        for point in (points[0], points[-1]):
            radius = 6*scale
            draw.ellipse((point[0]-radius, point[1]-radius, point[0]+radius, point[1]+radius), fill="#ba7b25")
    plane_mask = Image.new("L", (working, working))
    ImageDraw.Draw(plane_mask).polygon([xy(p) for p in JET], fill=255)
    orange = gradient(working, "ffbe55", "f5a01a")
    orange.putalpha(plane_mask)
    image.alpha_composite(orange)
    if size >= 48:
        draw = ImageDraw.Draw(image)
        draw.polygon([xy(p) for p in [(256, 174), (263, 216), (263, 255), (249, 255), (249, 216)]], fill="#262626")
    result = image.resize((size, size), Image.Resampling.LANCZOS)
    # Remove negligible resampling ringing outside the rounded tile.
    alpha = result.getchannel("A").point(lambda a: 0 if a <= 2 else a)
    result.putalpha(alpha)
    return result


def svg(tile):
    silhouette = " ".join(("M" if i == 0 else "L")+f"{x} {y}" for i, (x, y) in enumerate(JET)) + "Z"
    background = '<rect x="16" y="16" width="480" height="480" rx="88" fill="url(#tile)" stroke="#4a4a4a" stroke-width="3"/>' if tile else ""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" role="img" aria-label="DCS VR Control: aircraft in a focus reticle">
  <defs><linearGradient id="tile" gradientUnits="userSpaceOnUse" x2="0" y2="512"><stop stop-color="#343434"/><stop offset="1" stop-color="#191919"/></linearGradient><linearGradient id="aircraft" gradientUnits="userSpaceOnUse" x2="0" y2="512"><stop stop-color="#ffbe55"/><stop offset="1" stop-color="#f5a01a"/></linearGradient></defs>
  {background}
  <g fill="none" stroke="#ba7b25" stroke-width="12" stroke-linecap="round" stroke-linejoin="round">{''.join(f'<path d="{p}"/>' for p in BRACKETS)}</g>
  <path d="{silhouette}" fill="url(#aircraft)"/>
  <path d="M256 174L263 216V255H249V216Z" fill="#262626"/>
</svg>
'''


def main():
    ASSETS.mkdir(parents=True, exist_ok=True)
    (ASSETS / "app-icon.svg").write_text(svg(True), encoding="utf-8")
    (UI / "app-icon.svg").write_text(svg(True), encoding="utf-8")
    (UI / "brand-icon.svg").write_text(svg(False), encoding="utf-8")
    render(1024).save(ASSETS / "app-icon.png")
    render(256).save(ROOT / "docs/app-icon-preview.png")
    # PNG-compressed ICO frames support full alpha and crisp intermediate DPI sizes.
    payloads = []
    for size in SIZES:
        stream = BytesIO()
        render(size).save(stream, format="PNG")
        payloads.append(stream.getvalue())
    offset = 6 + 16*len(SIZES)
    directory = []
    for size, data in zip(SIZES, payloads):
        directory.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    (ASSETS / "app-icon.ico").write_bytes(struct.pack("<HHH", 0, 1, len(SIZES)) + b"".join(directory) + b"".join(payloads))
    ico = Image.open(ASSETS / "app-icon.ico")
    assert ico.ico.sizes() == {(n, n) for n in SIZES}
    for size in SIZES:
        frame = ico.ico.getimage((size, size))
        assert frame.mode == "RGBA" and frame.getpixel((0, 0))[3] == 0
    print(f"Built vector, 1024 px PNG and {len(SIZES)} alpha ICO frames: {ASSETS}")


if __name__ == "__main__":
    main()
