"""Bahnschrift Bold Condensed dan font_bahn.h yasaydi (24 / 20 / 15 px bosh harf, ikki panel uchun).
Ishlatish: python gen_bahn.py  (Windows, Pillow kerak)"""
from PIL import Image, ImageDraw, ImageFont

TTF = "C:/Windows/Fonts/bahnschrift.ttf"
VAR = "Bold Condensed"
import os
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font_bahn.h")

CHARS = (
    [ord(c) for c in "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-.,$%+/="]
    + [0x401] + list(range(0x410, 0x430))          # Ё, А..Я
    + [0x4A2, 0x4AE, 0x4E8]                         # Ң, Ү, Ө
)
CHARS = sorted(set(CHARS))


def make(cap):
    size = cap
    while True:
        f = ImageFont.truetype(TTF, size)
        f.set_variation_by_name(VAR)
        if -f.getbbox("M", anchor="ls")[1] >= cap:
            break
        size += 1
    asc, desc = f.getmetrics()
    glyphs = {}
    for code in CHARS:
        img = Image.new("L", (size * 2, asc + desc + 8), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((4, asc + 4), chr(code), font=f, fill=255, anchor="ls")
        px = img.load()
        ink = [[1 if px[x, y] > 127 else 0 for x in range(img.width)] for y in range(img.height)]
        cols = [x for x in range(img.width) if any(r[x] for r in ink)]
        glyphs[code] = [r[cols[0]:cols[-1] + 1] for r in ink]
    ys = [y for g in glyphs.values() for y, r in enumerate(g) if any(r)]
    top, bot = min(ys), max(ys)
    cap_top = (asc + 4 - cap) - top
    rows = bot - top + 1
    return {c: g[top:bot + 1] for c, g in glyphs.items()}, rows, cap_top


out = ["// Avtomatik yasalgan: Bahnschrift Bold Condensed (Windows shrifti) -> piksel shrift.",
       "// Qayta yasash: gen_bahn.py. Qo'lda o'zgartirmang.",
       "// Belgilar: 0-9 A-Z : - . , $ % + / =",
       "// hamda kirill А-Я, Ё, Ң, Ү, Ө. Har bir qator: bit (w-1-ustun) = piksel.",
       "#pragma once", "#include <Arduino.h>", "",
       "struct BahnFont {",
       "  uint8_t cap;              // bosh harf balandligi, px",
       "  uint8_t rows;             // glif qatorlari soni",
       "  uint8_t capTop;           // bosh harf yuqori chizig'i qaysi qatorda",
       "  uint8_t count;            // belgilar soni",
       "  const uint16_t *codes;    // Unicode, o'sish tartibida",
       "  const uint8_t  *widths;   // har bir belgi eni",
       "  const uint32_t *bits;     // count * rows ta qator",
       "};", ""]
for cap in (24, 20, 15):
    g, rows, cap_top = make(cap)
    widths = [len(g[c][0]) for c in CHARS]
    assert max(widths) <= 32
    n = f"bahn{cap}"
    out.append(f"// ---- {cap} px: {rows} qator, eng keng belgi {max(widths)} px ----")
    out.append(f"static const uint16_t {n}_codes[] PROGMEM = {{" + ", ".join(f"0x{c:04X}" for c in CHARS) + "};")
    out.append(f"static const uint8_t {n}_widths[] PROGMEM = {{" + ", ".join(map(str, widths)) + "};")
    out.append(f"static const uint32_t {n}_bits[] PROGMEM = {{")
    for c in CHARS:
        w = len(g[c][0])
        vals = [sum(bit << (w - 1 - i) for i, bit in enumerate(r)) for r in g[c]]
        label = chr(c) if c < 128 else f"U+{c:04X}"
        out.append("  " + ", ".join(f"0x{v:X}" for v in vals) + f",  // {label}")
    out.append("};")
    out.append(f"static const BahnFont {n} = {{{cap}, {rows}, {cap_top}, {len(CHARS)}, {n}_codes, {n}_widths, {n}_bits}};")
    out.append("")
    print(n, "rows", rows, "capTop", cap_top, "maxw", max(widths))
open(OUT, "w", encoding="utf-8").write("\n".join(out))
print("yozildi", OUT)
