# pico-clock

RP2350-Touch-LCD-3.49 (Waveshare) 横屏 LVGL 时钟。

## 功能
- LVGL 横屏时钟（640x172 逻辑，软件旋转后整屏刷新到 172x640 面板）
- 大号 7 段数码管时间（DSEG7，红色 LED 风格，含未点亮段残影）
- 多页菜单（左右滑动）：时钟 / 设备信息 / 设置
- 设置页（上下滚动）：每项含标题 + 功能说明：亮度、声音、24 小时制、秒进度条、主题色、时间重置
- 时钟页右半区：上滑调亮、下滑调暗
- 每秒“滴答”声（由 `tools/gen_tick_pcm.py` 从音频文件提取 PCM，ES8311 + core1 常驻喂 I2S）
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
- `src/font_7seg.c`  — 时钟页 7 段数码管字体（由 DSEG7 Classic Bold 生成）
- `src/tick_pcm.c` `src/tick_pcm.h` — 滴答声 PCM 样本（由 `tools/gen_tick_pcm.py` 生成）
- `tools/gen_fonts.sh` — 从 `clock_ui.c` 的字符串提取字符并生成字库
- `tools/gen_tick_pcm.py` — 从 mp3 提取“滴答”样本并生成 PCM 数组
- `tools/fonts/`    — 字库源文件（DroidSansFallback / DSEG7，含 OFL 许可）
- `include/clock_ui.h` `include/app.h` — 头文件
- `include/lv_conf.h` — LVGL 配置
- `lib/`                      — 官方驱动 (Config/QSPI_PIO/LCD/Touch/RTC/IMU/PSRAM) + LVGL + audio_pio/es8311
- `boards/waveshare_rp2350_touch_lcd_3.49.h` — 板级定义 (RP2350B)

> 修改界面中文文案后，需重新生成字库（否则新字不会显示）：
> ```bash
> npm install -g lv_font_conv   # 仅首次
> tools/gen_fonts.sh
> ```
