/*****************************************************************************
* | File       :   clock_ui.c
* | Function   :   横屏 LVGL 时钟 + 菜单 + 手势调亮度 + 音频 + 电源键
*
* 显示：LVGL 逻辑 640x172，full_refresh，flush 里自己做 90° 旋转并整屏写入。
******************************************************************************/
#include "clock_ui.h"
#include "pico/multicore.h"
#include "psram_tool.h"
#include "rp_pico_alloc.h"
#include "audio_pio.h"
#include "es8311.h"
#include "hardware/watchdog.h"

/* 逻辑横屏分辨率 */
#define LOGIC_W 640
#define LOGIC_H 172

/* ================= 显示 ================= */
static lv_disp_drv_t      disp_drv;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t        *buf0;

#define ROT_BLOCK_ROWS 8
static uint16_t s_rot_buf[172 * ROT_BLOCK_ROWS];

/* ================= 界面 ================= */
static lv_obj_t *tv;
static lv_obj_t *tile_clock, *tile_info, *tile_menu;

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *sec_bar;
static lv_obj_t *imu_label;
static lv_obj_t *batt_label;
static lv_obj_t *brightness_hint;
static lv_obj_t *brightness_slider = NULL;

/* ================= 触摸 ================= */
static uint16_t ts_x, ts_y;
static lv_indev_state_t ts_act;
static lv_indev_drv_t   indev_ts;

/* 手动滑动检测（右半区上下滑调亮度） */
static bool     s_touch_down   = false;
static bool     s_swipe_fired  = false;
static int      s_swipe_x0     = 0;
static int      s_swipe_y0     = 0;
static int      s_swipe_x1     = 0;
static int      s_swipe_y1     = 0;
static uint32_t s_last_touch_ms = 0;

/* ================= 定时器 ================= */
static struct repeating_timer lvgl_timer;

/* ================= 状态 ================= */
static int  s_last_sec     = -1;
static int  s_brightness   = 100;
static bool s_sound_on     = true;
static bool s_use_24h      = true;
static bool s_show_sec_bar = true;
static int  s_theme_idx    = 0;
static uint32_t s_hint_until = 0;

static const uint32_t THEME_COLORS[] = { 0x00E5FF, 0x76FF03, 0xFF4081, 0xFFD600, 0xB388FF };
#define THEME_COUNT (sizeof(THEME_COLORS) / sizeof(THEME_COLORS[0]))

static const char *WEEK_CN[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
static void touch_callback(uint gpio, uint32_t events);
static void ts_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
static bool repeating_lvgl_timer_callback(struct repeating_timer *t);
static void build_clock_tile(lv_obj_t *tile);
static void build_info_tile(lv_obj_t *tile);
static void build_menu_tile(lv_obj_t *tile);
static void update_clock_ui(datetime_t *t);
static void brightness_set(int v);
static void apply_theme(void);
static void rtc_set_from_build(void);

static void slider_brightness_cb(lv_event_t *e);
static void switch_sound_cb(lv_event_t *e);
static void switch_24h_cb(lv_event_t *e);
static void switch_secbar_cb(lv_event_t *e);
static void theme_btn_cb(lv_event_t *e);
static void reset_time_btn_cb(lv_event_t *e);

/********************************************************************************
function : 初始化 LVGL
********************************************************************************/
void ClockUI_Init(void)
{
    add_repeating_timer_ms(5, repeating_lvgl_timer_callback, NULL, &lvgl_timer);

    lv_init();

    buf0 = (lv_color_t *)malloc(LOGIC_W * LOGIC_H * sizeof(lv_color_t));
    if (buf0 == NULL) {
        printf("lvgl draw buffer malloc failed\r\n");
        return;
    }
    lv_disp_draw_buf_init(&disp_buf, buf0, NULL, LOGIC_W * LOGIC_H);

    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb     = disp_flush_cb;
    disp_drv.draw_buf     = &disp_buf;
    disp_drv.hor_res      = LOGIC_W;
    disp_drv.ver_res      = LOGIC_H;
    disp_drv.full_refresh = 1;
    disp_drv.direct_mode  = 0;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_ts);
    indev_ts.type    = LV_INDEV_TYPE_POINTER;
    indev_ts.read_cb = ts_read_cb;
    lv_indev_drv_register(&indev_ts);

    DEV_KEY_Config(TOUCH_INT_PIN);
    DEV_IRQ_SET(TOUCH_INT_PIN, GPIO_IRQ_EDGE_FALL, &touch_callback);
}

/********************************************************************************
function : 构建界面
********************************************************************************/
void ClockUI_Build(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x05070A), 0);

    tv = lv_tileview_create(lv_scr_act());
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x05070A), 0);

    tile_clock = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    tile_info  = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    tile_menu  = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);

    build_clock_tile(tile_clock);
    build_info_tile(tile_info);
    build_menu_tile(tile_menu);

    s_brightness = 100;
    DEV_SET_PWM(s_brightness);
    apply_theme();
}

/* ----------------------------- 时钟页 ----------------------------- */
static void build_clock_tile(lv_obj_t *tile)
{
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x071018), 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);

    lv_obj_t *line = lv_obj_create(tile);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LOGIC_W, 4);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    date_label = lv_label_create(tile);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0x9AA0A6), 0);
    lv_label_set_text(date_label, "----/--/--");
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, 12);

    time_label = lv_label_create(tile);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x00E5FF), 0);
    lv_label_set_text(time_label, "00:00:00");
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 10);

    sec_bar = lv_bar_create(tile);
    lv_obj_set_size(sec_bar, 600, 8);
    lv_obj_align(sec_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_bar_set_range(sec_bar, 0, 59);
    lv_bar_set_value(sec_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sec_bar, lv_color_hex(0x1B2733), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sec_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);

    /* 亮度提示（右上角，调节时短暂显示） */
    brightness_hint = lv_label_create(tile);
    lv_obj_set_style_text_font(brightness_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(brightness_hint, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(brightness_hint, "100%");
    lv_obj_align(brightness_hint, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_flag(brightness_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
}

/* ----------------------------- 信息页 ----------------------------- */
static void build_info_tile(lv_obj_t *tile)
{
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0A0F0A), 0);

    lv_obj_t *title = lv_label_create(tile);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x76FF03), 0);
    lv_label_set_text(title, "设备信息");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    imu_label = lv_label_create(tile);
    lv_obj_set_style_text_font(imu_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(imu_label, lv_color_hex(0xE0E0E0), 0);
    lv_label_set_text(imu_label, "IMU: --");
    lv_obj_align(imu_label, LV_ALIGN_CENTER, 0, -10);

    batt_label = lv_label_create(tile);
    lv_obj_set_style_text_font(batt_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(batt_label, lv_color_hex(0xB0BEC5), 0);
    lv_label_set_text(batt_label, "BAT: --");
    lv_obj_align(batt_label, LV_ALIGN_CENTER, 0, 28);
}

/* ----------------------------- 设置页 ----------------------------- */
static lv_obj_t *menu_add_row(lv_obj_t *list, int index, const char *text)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, LOGIC_W - 24, 46);
    lv_obj_set_pos(row, 12, 8 + index * 54);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x141A22), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 6, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0E0E0), 0);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

static void build_menu_tile(lv_obj_t *tile)
{
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0A0A14), 0);

    /* 可纵向滚动的列表 */
    lv_obj_t *list = lv_obj_create(tile);
    lv_obj_set_size(list, LOGIC_W, LOGIC_H);
    lv_obj_center(list);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    /* 0 亮度 */
    lv_obj_t *r0 = menu_add_row(list, 0, "亮度");
    brightness_slider = lv_slider_create(r0);
    lv_obj_set_width(brightness_slider, 300);
    lv_obj_align(brightness_slider, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, s_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, slider_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 1 声音 */
    lv_obj_t *r1 = menu_add_row(list, 1, "声音");
    lv_obj_t *sw1 = lv_switch_create(r1);
    lv_obj_align(sw1, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_sound_on) lv_obj_add_state(sw1, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw1, switch_sound_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 2 24 小时制 */
    lv_obj_t *r2 = menu_add_row(list, 2, "24 小时制");
    lv_obj_t *sw2 = lv_switch_create(r2);
    lv_obj_align(sw2, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_use_24h) lv_obj_add_state(sw2, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw2, switch_24h_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 3 秒进度条 */
    lv_obj_t *r3 = menu_add_row(list, 3, "秒进度条");
    lv_obj_t *sw3 = lv_switch_create(r3);
    lv_obj_align(sw3, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_show_sec_bar) lv_obj_add_state(sw3, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw3, switch_secbar_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 4 主题色 */
    lv_obj_t *r4 = menu_add_row(list, 4, "主题色");
    lv_obj_t *btn4 = lv_btn_create(r4);
    lv_obj_set_size(btn4, 90, 34);
    lv_obj_align(btn4, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *bl4 = lv_label_create(btn4);
    lv_label_set_text(bl4, "切换");
    lv_obj_center(bl4);
    lv_obj_add_event_cb(btn4, theme_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 5 时间重置 */
    lv_obj_t *r5 = menu_add_row(list, 5, "时间重置");
    lv_obj_t *btn5 = lv_btn_create(r5);
    lv_obj_set_size(btn5, 90, 34);
    lv_obj_align(btn5, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *bl5 = lv_label_create(btn5);
    lv_label_set_text(bl5, "重置");
    lv_obj_center(bl5);
    lv_obj_add_event_cb(btn5, reset_time_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void slider_brightness_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    brightness_set(lv_slider_get_value(s));
}

static void switch_sound_cb(lv_event_t *e)
{
    s_sound_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void switch_24h_cb(lv_event_t *e)
{
    s_use_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_last_sec = -1;                 /* 立即刷新时间显示 */
}

static void switch_secbar_cb(lv_event_t *e)
{
    s_show_sec_bar = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (s_show_sec_bar) lv_obj_clear_flag(sec_bar, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(sec_bar, LV_OBJ_FLAG_HIDDEN);
}

static void theme_btn_cb(lv_event_t *e)
{
    s_theme_idx = (s_theme_idx + 1) % THEME_COUNT;
    apply_theme();
}

static void reset_time_btn_cb(lv_event_t *e)
{
    rtc_set_from_build();
    s_last_sec = -1;
}


/* ================= 音频：很轻的"喀哒"声 + core1 常驻喂 I2S ================= */
#define PA_CTRL_PIN    0
#define SAMPLE_RATE    24000
#define TICK_MS        12
#define TICK_N         (SAMPLE_RATE * TICK_MS / 1000)
#define AUDIO_RING_SIZE 4096
#define KEY_LONG_MS    1500

static const int16_t sine256[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

static int16_t tick_hi[TICK_N];
static int16_t tick_lo[TICK_N];

static volatile int16_t  audio_ring[AUDIO_RING_SIZE];
static volatile uint32_t audio_head = 0;
static volatile uint32_t audio_tail = 0;

static bool     s_screen_on    = true;
static bool     s_audio_muted  = true;
static uint32_t s_last_tick_ms = 0;

static uint32_t rng_state = 0x9E3779B9u;
static int32_t noise_sample(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int32_t)(rng_state >> 18) - 4096;
}

static void make_click(int16_t *buf, uint32_t freq_hz)
{
    const int attack = SAMPLE_RATE / 2000;
    uint32_t phase = 0;
    uint32_t step = (uint32_t)(((uint64_t)freq_hz * 256 * 65536) / SAMPLE_RATE);
    int32_t env = 0, lp = 0;

    for (int i = 0; i < TICK_N; i++) {
        if (i < attack) env = 32767 * i / attack;
        else            env = env * 250 / 256;
        int idx = (phase >> 16) & 0xFF;
        int32_t mix = sine256[idx] / 3 + noise_sample();
        lp += (mix - lp) / 2;
        buf[i] = (int16_t)((int32_t)lp * env / 32768);
        phase += step;
    }
    int32_t peak = 0;
    for (int i = 0; i < TICK_N; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        const int32_t target = 8000;
        for (int i = 0; i < TICK_N; i++) buf[i] = (int16_t)((int32_t)buf[i] * target / peak);
    }
}

static void tick_init(void)
{
    make_click(tick_hi, 1500);
    make_click(tick_lo, 1100);
}

static void audio_feed_core1(void)
{
    while (true) {
        int16_t smp = 0;
        if (audio_tail != audio_head) {
            smp = audio_ring[audio_tail];
            audio_tail = (audio_tail + 1) & (AUDIO_RING_SIZE - 1);
        }
        pio_sm_put_blocking(pico_audio.pio_2, pico_audio.sm_dout, (int32_t)smp * 65536);
    }
}

static void play_tick(bool high)
{
    const int16_t *buf = high ? tick_hi : tick_lo;
    for (int i = 0; i < TICK_N; i++) {
        uint32_t next = (audio_head + 1) & (AUDIO_RING_SIZE - 1);
        if (next == audio_tail) break;
        audio_ring[audio_head] = buf[i];
        audio_head = next;
    }
}

static void audio_init(void)
{
    gpio_init(PA_CTRL_PIN);
    gpio_set_dir(PA_CTRL_PIN, GPIO_OUT);
    gpio_put(PA_CTRL_PIN, 1);

    es8311_init(pico_audio);
    es8311_sample_frequency_config(pico_audio.mclk_freq, pico_audio.sample_freq);
    es8311_voice_volume_set(55);

    mclk_pio_init();
    dout_pio_init();
    tick_init();
    es8311_voice_mute(true);
}

/* ================= Power 按键 ================= */
static bool     s_key_down = false;
static uint32_t s_key_t0   = 0;

static void power_off(void)
{
    DEV_SET_PWM(0);
    es8311_voice_mute(true);
    gpio_put(PIN_PWR_EN, 0);
    gpio_put(SYS_EN, 0);
    while (gpio_get(SYS_OUT) == 0) sleep_ms(20);
    while (gpio_get(SYS_OUT) != 0) sleep_ms(20);
    gpio_put(SYS_EN, 1);
    watchdog_reboot(0, 0, 0);
}

static int key_poll(bool *screen_on)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool down = (gpio_get(SYS_OUT) == 0);

    if (down) {
        if (!s_key_down) { s_key_down = true; s_key_t0 = now; }
        else if ((now - s_key_t0) >= KEY_LONG_MS) power_off();
    } else if (s_key_down) {
        s_key_down = false;
        uint32_t dur = now - s_key_t0;
        if (dur >= 30 && dur < KEY_LONG_MS) { *screen_on = !*screen_on; return 1; }
    }
    return 0;
}

/********************************************************************************
function : 应用初始化
********************************************************************************/
void App_Init(void)
{
    audio_init();

    datetime_t t;
    PCF85063A_Read_now(&t);
    if (t.year < 2024 || t.year > 2099) {
        rtc_set_from_build();
    }

    multicore_launch_core1(audio_feed_core1);
}

/********************************************************************************
function : 主循环高频调用
********************************************************************************/
void App_Poll(void)
{
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (key_poll(&s_screen_on) == 1) {
        if (s_screen_on) {
            DEV_SET_PWM(s_brightness);
        } else {
            DEV_SET_PWM(0);
            es8311_voice_mute(true);
            s_audio_muted = true;
        }
    }

    if (s_touch_down && (now_ms - s_last_touch_ms) > 150) {
        s_touch_down = false;
    }

    if (!s_audio_muted && (now_ms - s_last_tick_ms) > 250) {
        es8311_voice_mute(true);
        s_audio_muted = true;
    }

    /* 亮度提示 1s 后隐藏 */
    if (brightness_hint && !lv_obj_has_flag(brightness_hint, LV_OBJ_FLAG_HIDDEN)) {
        if (now_ms > s_hint_until) {
            lv_obj_add_flag(brightness_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/********************************************************************************
function : 低频刷新（时钟 / 信息）
********************************************************************************/
void App_Update(void)
{
    static uint32_t last_ms = 0;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - last_ms < 200) {
        return;
    }
    last_ms = now_ms;

    datetime_t t;
    PCF85063A_Read_now(&t);
    if (t.sec != s_last_sec) {
        s_last_sec = t.sec;
        update_clock_ui(&t);

        if (s_screen_on && s_sound_on) {
            if (s_audio_muted) {
                es8311_voice_mute(false);
                s_audio_muted = false;
                sleep_ms(3);
            }
            play_tick((t.sec & 1) == 0);
            s_last_tick_ms = now_ms;
        }
    }

    if (lv_tileview_get_tile_act(tv) == tile_info) {
        float acc[3], gyro[3];
        unsigned int cnt = 0;
        QMI8658_read_xyz(acc, gyro, &cnt);
        char b[64];
        snprintf(b, sizeof(b), "ACC  X%5.0f Y%5.0f Z%5.0f",
                 (double)acc[0], (double)acc[1], (double)acc[2]);
        lv_label_set_text(imu_label, b);

        adc_select_input(BAT_CHANNEL);
        uint16_t raw = adc_read();
        float v = raw * 3.3f / 4096.0f * 3.0f;
        snprintf(b, sizeof(b), "BAT: %.2f V", (double)v);
        lv_label_set_text(batt_label, b);
    }
}

/* ================= 辅助 ================= */
static void update_clock_ui(datetime_t *t)
{
    char buf[40];
    if (s_use_24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->hour, t->min, t->sec);
    } else {
        int h12 = t->hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", h12, t->min, t->sec,
                 t->hour < 12 ? "AM" : "PM");
    }
    lv_label_set_text(time_label, buf);

    int wd = (t->dotw >= 0 && t->dotw < 7) ? t->dotw : 0;
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d  %s",
             t->year, t->month, t->day, WEEK_CN[wd]);
    lv_label_set_text(date_label, buf);

    lv_bar_set_value(sec_bar, t->sec, LV_ANIM_OFF);
}

static void brightness_set(int v)
{
    if (v < 10)  v = 10;
    if (v > 100) v = 100;
    s_brightness = v;
    DEV_SET_PWM(s_brightness);

    if (brightness_slider) {
        lv_slider_set_value(brightness_slider, v, LV_ANIM_OFF);
    }
    if (brightness_hint) {
        lv_label_set_text_fmt(brightness_hint, "%d%%", v);
        lv_obj_clear_flag(brightness_hint, LV_OBJ_FLAG_HIDDEN);
        s_hint_until = to_ms_since_boot(get_absolute_time()) + 1000;
    }
}

static void apply_theme(void)
{
    uint32_t c = THEME_COLORS[s_theme_idx];
    if (time_label) lv_obj_set_style_text_color(time_label, lv_color_hex(c), 0);
    if (sec_bar)    lv_obj_set_style_bg_color(sec_bar, lv_color_hex(c), LV_PART_INDICATOR);
}

static void rtc_set_from_build(void)
{
    char mon[4] = {0};
    int day = 1, year = 2024, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(months, mon);
    int month = p ? (int)(p - months) / 3 + 1 : 1;

    datetime_t t;
    t.year = year; t.month = month; t.day = day; t.dotw = 0;
    t.hour = hh; t.min = mm; t.sec = ss;
    PCF85063A_Set_All(t);
}

/* ================= 刷屏：逻辑横屏旋转 + 整屏写入 ================= */
static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    (void)area;
    const uint16_t *src = (const uint16_t *)color_p;

    LCD_3IN49_SetWindows(0, 0, LCD_3IN49_WIDTH, LCD_3IN49_HEIGHT);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);
    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

    for (int y = 0; y < LCD_3IN49_HEIGHT; y += ROT_BLOCK_ROWS) {
        int rows = LCD_3IN49_HEIGHT - y;
        if (rows > ROT_BLOCK_ROWS) rows = ROT_BLOCK_ROWS;

        for (int r = 0; r < rows; r++) {
            int py = y + r;
            for (int px = 0; px < LCD_3IN49_WIDTH; px++) {
                s_rot_buf[r * LCD_3IN49_WIDTH + px] =
                    src[py + (LCD_3IN49_WIDTH - 1 - px) * LOGIC_W];
            }
        }

        dma_channel_configure(dma_tx, &c, &qspi.pio->txf[qspi.sm],
                              s_rot_buf, rows * LCD_3IN49_WIDTH * 2, true);
        while (dma_channel_is_busy(dma_tx));
    }
    QSPI_Deselect(qspi);

    lv_disp_flush_ready(disp);
}

static void touch_callback(uint gpio, uint32_t events)
{
    if (gpio != TOUCH_INT_PIN) return;

    Touch_Read_State();
    int lx = TOUCH.Point1_y;
    int ly = (LCD_3IN49_WIDTH - 1) - TOUCH.Point1_x;
    s_last_touch_ms = to_ms_since_boot(get_absolute_time());

    if (TOUCH.Finger_Num > 0) {
        if (!s_touch_down) {                 /* 按下 */
            s_touch_down  = true;
            s_swipe_fired = false;
            s_swipe_x0 = lx; s_swipe_y0 = ly;
        }
        s_swipe_x1 = lx; s_swipe_y1 = ly;

        /* 右半区：竖直滑动 -> 调亮度（一次滑动只调一档） */
        int dx = s_swipe_x1 - s_swipe_x0;
        int dy = s_swipe_y1 - s_swipe_y0;
        if (!s_swipe_fired && s_swipe_x0 >= LOGIC_W / 2 &&
            abs(dy) > 60 && abs(dy) > abs(dx)) {
            if (dy < 0) brightness_set(s_brightness + 10);   /* 上滑 */
            else        brightness_set(s_brightness - 10);   /* 下滑 */
            s_swipe_fired = true;
        }

        ts_x = lx; ts_y = ly; ts_act = LV_INDEV_STATE_PRESSED;
    } else {
        s_touch_down = false;
        ts_act = LV_INDEV_STATE_RELEASED;
    }
}

static void ts_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    data->point.x = ts_x;
    data->point.y = ts_y;
    data->state   = ts_act;
    ts_act = LV_INDEV_STATE_RELEASED;
}

static bool repeating_lvgl_timer_callback(struct repeating_timer *t)
{
    lv_tick_inc(5);
    return true;
}
