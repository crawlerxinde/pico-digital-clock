#!/usr/bin/env bash
#
# 编译并烧录。每次烧录前会 touch clock_ui.c，使 __DATE__/__TIME__
# 反映本次烧录时间；固件启动时据此判断“刚烧录过”并校准 RTC。
#
# 可覆盖的环境变量：
#   PICO_SDK_PATH, PICO_TOOLCHAIN_PATH, CMAKE, NINJA, PICOTOOL
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SDK="${PICO_SDK_PATH:-$HOME/.pico-sdk/sdk/2.3.1}"
TOOLCHAIN="${PICO_TOOLCHAIN_PATH:-$HOME/.pico-sdk/toolchain/15_2_Rel1}"
CMAKE="${CMAKE:-$HOME/.pico-sdk/cmake/v4.3.4/bin/cmake}"
NINJA="${NINJA:-$HOME/.pico-sdk/ninja/v1.13.2/ninja}"
PICOTOOL="${PICOTOOL:-$HOME/.pico-sdk/picotool/2.3.1/picotool/picotool}"

export PICO_SDK_PATH="$SDK"
export PICO_TOOLCHAIN_PATH="$TOOLCHAIN"
export PATH="$(dirname "$CMAKE"):$(dirname "$NINJA"):$TOOLCHAIN/bin:$PATH"

BOARD="${PICO_BOARD:-waveshare_rp2350_touch_lcd_3.49}"

# 刷新编译时间戳（只改 mtime，不改内容，git 无变化）
touch "$ROOT/src/clock_ui.c"

"$CMAKE" -S "$ROOT" -B "$ROOT/build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DPICO_BOARD="$BOARD"
"$CMAKE" --build "$ROOT/build"

"$PICOTOOL" load -f -x "$ROOT/build/pico-clock.uf2"
