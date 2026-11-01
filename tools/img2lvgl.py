"""Konwersja obrazu (PNG/JPG) do tablicy C dla LVGL 8 (RGB565, little endian).

Użycie:
    python tools/img2lvgl.py <obraz> <nazwa_zmiennej> [--size 480] [--circle] [--out src/<nazwa>.c]

  --size    maksymalny bok w pikselach (proporcje zachowane), domyślnie 480
  --circle  zamaluj na czarno wszystko poza wpisanym okręgiem (okrągłe logo na białym tle)
"""
import argparse
import re

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("name")
    ap.add_argument("--size", type=int, default=480)
    ap.add_argument("--circle", action="store_true")
    ap.add_argument("--out")
    args = ap.parse_args()

    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.name):
        ap.error("nazwa zmiennej moze zawierac tylko litery, cyfry i _ (bez myslnikow)")
    out_path = args.out or f"src/{args.name}.c"

    # Przezroczystość spłaszczana na czarne tło (tło ekranu)
    src = Image.open(args.image).convert("RGBA")
    img = Image.new("RGB", src.size, (0, 0, 0))
    img.paste(src, mask=src.split()[3])
    img.thumbnail((args.size, args.size), Image.Resampling.LANCZOS)
    w, h = img.size

    if args.circle:
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, w - 1, h - 1), fill=255)
        black = Image.new("RGB", (w, h), (0, 0, 0))
        img = Image.composite(img, black, mask)

    with open(out_path, "w") as f:
        f.write('#include "lvgl.h"\n\n')
        f.write(f"const uint8_t {args.name}_map[] = {{\n")
        for y in range(h):
            row = []
            for x in range(w):
                r, g, b = img.getpixel((x, y))
                c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                row.append(f"0x{c & 0xFF:02x}, 0x{c >> 8:02x}")
            f.write(", ".join(row) + ",\n")
        f.write("};\n\n")
        f.write(f"const lv_img_dsc_t {args.name} = {{\n")
        f.write("  .header = {\n")
        f.write("    .cf = LV_IMG_CF_TRUE_COLOR,\n")
        f.write("    .always_zero = 0,\n")
        f.write("    .reserved = 0,\n")
        f.write(f"    .w = {w},\n")
        f.write(f"    .h = {h},\n")
        f.write("  },\n")
        f.write(f"  .data_size = {w * h * 2},\n")
        f.write(f"  .data = {args.name}_map,\n")
        f.write("};\n")

    print(f"Zapisano {out_path}: {w}x{h} px, {w * h * 2} bajtow obrazu")


if __name__ == "__main__":
    main()
