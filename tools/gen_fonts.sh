#!/usr/bin/env bash
#
# 生成界面用中文字库子集（仅包含界面实际显示的字符）。
# 依赖：node + lv_font_conv，系统 CJK 字体。
#
#   npm install -g lv_font_conv
#   tools/gen_fonts.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FONT="${CJK_FONT:-/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf}"
OUT="$ROOT/src"

if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "lv_font_conv not found. Install with: npm install -g lv_font_conv" >&2
    exit 1
fi
if [ ! -f "$FONT" ]; then
    echo "CJK font not found: $FONT (override with CJK_FONT=...)" >&2
    exit 1
fi

# 只从字符串字面量里取非 ASCII 字符（忽略注释），避免把注释里的汉字也塞进字库。
SYMS="$(python3 - "$ROOT/src/clock_ui.c" <<'PY'
import re, sys
src = open(sys.argv[1], encoding='utf-8').read()
src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
src = re.sub(r'//[^\n]*', '', src)
chars = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    for ch in lit:
        if ord(ch) > 0x7f:
            chars.add(ch)
print(''.join(sorted(chars)))
PY
)"

echo "glyphs: $SYMS"

for size in 20 16 12; do
    echo "generating font_cjk_${size}.c"
    lv_font_conv --font "$FONT" --size "$size" --bpp 4 --format lvgl \
        --lv-include lvgl.h --lv-font-name "font_cjk_${size}" \
        --range 0x20-0x7f --symbols "$SYMS" \
        -o "$OUT/font_cjk_${size}.c"
done

echo "done"
