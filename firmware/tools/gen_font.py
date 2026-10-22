"""Sinh font bitmap 8x16 (ASCII 32..126) cho màn hình gateway từ DejaVu Sans Mono.

DejaVu dùng giấy phép Bitstream Vera (cho phép nhúng/phân phối lại).
Chạy:  python firmware/tools/gen_font.py [đường dẫn .ttf]  -> firmware/gateway/main/font8x16.h
Cần Pillow (pip install pillow).
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H, SIZE, BASELINE = 8, 16, 13, 12
ttf = sys.argv[1] if len(sys.argv) > 1 else r"C:\Windows\Fonts\DejaVuSansMono.ttf"
font = ImageFont.truetype(ttf, SIZE)
out = Path(__file__).resolve().parents[1] / "gateway" / "main" / "font8x16.h"

rows = []
for code in range(32, 127):
    img = Image.new("L", (W, H), 0)
    ImageDraw.Draw(img).text((0, BASELINE), chr(code), font=font, fill=255, anchor="ls")
    px = img.load()
    data = []
    for y in range(H):
        b = 0
        for x in range(W):
            if px[x, y] >= 110:
                b |= 0x80 >> x
        data.append(b)
    ch = chr(code).replace("\\", "\\\\")
    rows.append("    {" + ", ".join(f"0x{b:02X}" for b in data) + f"}},  // '{ch}'")

out.write_text(
    "#pragma once\n#include <stdint.h>\n\n"
    "// Font 8x16 ASCII 32..126, sinh bởi firmware/tools/gen_font.py từ DejaVu Sans Mono (giấy phép Bitstream Vera).\n"
    "// Mỗi ký tự 16 byte (1 byte/hàng), bit 7 = điểm ảnh trái nhất.\n"
    "#define FONT_W 8\n#define FONT_H 16\n#define FONT_FIRST 32\n#define FONT_LAST 126\n\n"
    "static const uint8_t FONT8X16[FONT_LAST - FONT_FIRST + 1][FONT_H] = {\n" + "\n".join(rows) + "\n};\n",
    encoding="utf-8", newline="\n")
print(f"Da ghi {out}")
