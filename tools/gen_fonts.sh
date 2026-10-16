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
# 拉丁/数字字形取自 Montserrat，与 LVGL 内置字体一致（Droid fallback 不含拉丁字形）
LATIN_FONT="${LATIN_FONT:-$ROOT/lib/lvgl/scripts/built_in_font/Montserrat-Medium.ttf}"
OUT="$ROOT/src"

if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "lv_font_conv not found. Install with: npm install -g lv_font_conv" >&2
    exit 1
fi
if [ ! -f "$FONT" ]; then
    echo "CJK font not found: $FONT (override with CJK_FONT=...)" >&2
    exit 1
fi
if [ ! -f "$LATIN_FONT" ]; then
    echo "Latin font not found: $LATIN_FONT (override with LATIN_FONT=...)" >&2
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
    lv_font_conv --font "$LATIN_FONT" -r 0x20-0x7f \
        --font "$FONT" --symbols "$SYMS" \
        --size "$size" --bpp 4 --format lvgl \
        --no-compress --no-prefilter \
        --lv-include lvgl.h --lv-font-name "font_cjk_${size}" \
        -o "$OUT/font_cjk_${size}.c"
done

# 数码管字体（DSEG7 Classic Bold，SIL OFL），用于时钟页 7 段数码显示
SEG_FONT="$ROOT/tools/fonts/DSEG7Classic-Bold.woff"
if [ -f "$SEG_FONT" ]; then
    echo "generating font_7seg.c"
    lv_font_conv --font "$SEG_FONT" -r 0x20-0x3a --size 104 --bpp 4 --format lvgl \
        --no-compress --no-prefilter \
        --lv-include lvgl.h --lv-font-name font_7seg \
        -o "$ROOT/src/font_7seg.c"
else
    echo "skip font_7seg.c: $SEG_FONT not found" >&2
fi

echo "done"
