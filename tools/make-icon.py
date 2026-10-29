"""Generate the PaperNotes application icon.

Two masters are drawn instead of one: the detailed note reads well from 24px up,
but at 16-20px its text lines turn to mush, so the small rendition drops the
accent marker and uses fewer, thicker lines.

The .ico is assembled by hand because PIL can only resize a single image into
every slot; PNG-compressed entries let each size keep its own drawing.
"""

import os
import struct

from PIL import Image, ImageDraw

PAPER = (255, 251, 235, 255)
EDGE = (211, 195, 160, 255)
STRIP = (247, 231, 186, 255)
INK = (58, 48, 34, 255)
AMBER = (217, 119, 6, 255)

MASTER = 1024


def build(detail: bool) -> Image.Image:
    """detail=True: full rendition. detail=False: thick-line small rendition."""
    s = MASTER
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    margin = int(s * (0.06 if detail else 0.04))
    box = (margin, margin, s - margin, s - margin)
    radius = int(s * (0.16 if detail else 0.18))
    stroke = max(2, int(s * (0.012 if detail else 0.03)))

    d.rounded_rectangle(box, radius=radius, fill=PAPER, outline=EDGE, width=stroke)

    strip_h = int((box[3] - box[1]) * (0.26 if detail else 0.30))
    d.rounded_rectangle(
        (box[0], box[1], box[2], box[1] + strip_h + radius),
        radius=radius,
        fill=STRIP,
    )
    d.rectangle((box[0], box[1] + strip_h, box[2], box[1] + strip_h + stroke), fill=STRIP)

    if detail:
        cx0 = box[0] + int((box[2] - box[0]) * 0.14)
        cy0 = box[1] + int(strip_h * 0.34)
        side = int(strip_h * 0.3)
        d.rounded_rectangle(
            (cx0, cy0, cx0 + side, cy0 + side), radius=int(side * 0.3), fill=AMBER
        )

    if detail:
        line_h = max(3, int(s * 0.045))
        gap = int(line_h * 1.9)
        widths = [1.0, 0.86, 0.62]
    else:
        line_h = max(6, int(s * 0.085))
        gap = int(line_h * 1.15)
        widths = [1.0, 0.70]

    left = box[0] + int((box[2] - box[0]) * (0.12 if detail else 0.13))
    right = box[2] - int((box[2] - box[0]) * (0.12 if detail else 0.13))
    y = box[1] + strip_h + int((box[3] - box[1]) * (0.12 if detail else 0.14))
    floor = box[3] - int((box[3] - box[1]) * 0.08)

    for w in widths:
        if y + line_h > floor:
            break
        d.rounded_rectangle(
            (left, y, left + int((right - left) * w), y + line_h),
            radius=line_h // 2,
            fill=INK,
        )
        y += line_h + gap

    return img


def png_bytes(img: Image.Image, size: int) -> bytes:
    from io import BytesIO

    buf = BytesIO()
    img.resize((size, size), Image.LANCZOS).save(buf, format="PNG")
    return buf.getvalue()


def write_ico(path: str, entries):
    """entries: list of (size, png bytes)."""
    count = len(entries)
    header = struct.pack("<HHH", 0, 1, count)
    offset = len(header) + 16 * count
    directory = b""
    payload = b""
    for size, data in entries:
        dim = 0 if size >= 256 else size
        directory += struct.pack(
            "<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset
        )
        payload += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + directory + payload)


def main():
    big = build(detail=True)
    small = build(detail=False)

    entries = []
    for size in (16, 20, 24, 32, 40, 48, 64, 128, 256):
        source = small if size <= 20 else big
        entries.append((size, png_bytes(source, size)))

    write_ico("src/App/app.ico", entries)
    print("wrote src/App/app.ico with sizes:", [s for s, _ in entries])

    # MSIX tile assets referenced by AppxManifest.xml. Drawn from the same master
    # so the package tile and the exe icon cannot drift apart.
    assets = {
        "StoreLogo.png": 50,
        "Square44x44Logo.png": 44,
        "Square150x150Logo.png": 150,
        "Square310x310Logo.png": 310,
    }
    for name, size in assets.items():
        path = "packaging/Assets/" + name
        big.resize((size, size), Image.LANCZOS).save(path)
        print("wrote {} {}x{}".format(path, size, size))

    # Preview only; out/ is not part of the repository.
    os.makedirs("out/diagnostics", exist_ok=True)
    big.resize((256, 256), Image.LANCZOS).save("out/diagnostics/app-icon-256.png")
    print("wrote out/diagnostics/app-icon-256.png")


if __name__ == "__main__":
    main()
