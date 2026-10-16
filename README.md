# pico-clock

RP2350-Touch-LCD-3.49 (Waveshare) 横屏 LVGL 时钟。

## 功能
- LVGL 横屏时钟（640x172 逻辑，软件旋转后整屏刷新到 172x640 面板）
- 大号时间 + 日期/星期 + 秒进度条
- 多页菜单（左右滑动）：时钟 / 设备信息 / 设置
- 设置页（上下滚动）：每项含标题 + 功能说明：亮度、声音、24 小时制、秒进度条、主题色、时间重置
- 时钟页右半区：上滑调亮、下滑调暗
- 每秒很轻的"喀哒"声（ES8311 + core1 常驻喂 I2S）
- 电源键：短按熄屏/亮屏，长按关机

## 硬件
- RP2350B + AXS15231B 3.49" 172x640 QSPI 屏（含触摸）+ PCF85063A RTC
  + QMI8658 IMU + ES8311 音频 + 16MB Flash

## 构建
```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.1
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
cmake -S . -B build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=$HOME/.pico-sdk/ninja/v1.13.2/ninja \
  -DPICO_BOARD=waveshare_rp2350_touch_lcd_3.49
cmake --build build
# 产物: build/pico-clock.uf2
```

## 关键文件
- `src/clock_ui.c`  — 界面 + 音频 + 电源键 + 显示旋转
- `src/app_main.c`  — 入口 `app_main()`
- `src/font_cjk_*.c` — 界面中文字库子集（由 `tools/gen_fonts.sh` 生成）
- `tools/gen_fonts.sh` — 从 `clock_ui.c` 的字符串提取字符并生成字库
- `include/clock_ui.h` `include/app.h` — 头文件
- `include/lv_conf.h` — LVGL 配置
- `lib/`                      — 官方驱动 (Config/QSPI_PIO/LCD/Touch/RTC/IMU/PSRAM) + LVGL + audio_pio/es8311
- `boards/waveshare_rp2350_touch_lcd_3.49.h` — 板级定义 (RP2350B)

> 修改界面中文文案后，需重新生成字库（否则新字不会显示）：
> ```bash
> npm install -g lv_font_conv   # 仅首次
> tools/gen_fonts.sh
> ```
