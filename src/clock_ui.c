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
#include "tick_pcm.h"
#include "hardware/watchdog.h"

/* 逻辑横屏分辨率 */
#define LOGIC_W 640
#define LOGIC_H 172

/* 中文字库（由 DroidSansFallbackFull.ttf 生成，见 tools/gen_fonts.sh） */
LV_FONT_DECLARE(font_cjk_20);
LV_FONT_DECLARE(font_cjk_16);
LV_FONT_DECLARE(font_cjk_12);
LV_FONT_DECLARE(font_7seg);

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
static lv_obj_t *time_ghost;
static lv_obj_t *ampm_label;
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

static const uint32_t THEME_COLORS[] = { 0xFF1A1A, 0x00E5FF, 0x76FF03, 0xFF4081, 0xFFD600, 0xB388FF };
#define THEME_COUNT (sizeof(THEME_COLORS) / sizeof(THEME_COLORS[0]))
static lv_obj_t *theme_swatches[THEME_COUNT];

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
static int  battery_percent(float v);

static void slider_brightness_cb(lv_event_t *e);
static void switch_sound_cb(lv_event_t *e);
static void switch_24h_cb(lv_event_t *e);
static void switch_secbar_cb(lv_event_t *e);
static void theme_swatch_cb(lv_event_t *e);
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
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x000000), 0);

    date_label = lv_label_create(tile);
    lv_obj_set_style_text_font(date_label, &font_cjk_16, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xFF1A1A), 0);
    lv_obj_set_style_text_opa(date_label, LV_OPA_60, 0);
    lv_label_set_text(date_label, "----/--/--");
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, 2);

    /* 数码管：先铺一层暗色 "88:88:88" 作为未点亮的段，时间再叠上去 */
    time_ghost = lv_label_create(tile);
    lv_obj_set_style_text_font(time_ghost, &font_7seg, 0);
    lv_obj_set_style_text_color(time_ghost, lv_color_hex(0xFF1A1A), 0);
    lv_obj_set_style_text_opa(time_ghost, LV_OPA_20, 0);
    lv_label_set_text(time_ghost, "88:88:88");
    lv_obj_align(time_ghost, LV_ALIGN_CENTER, 0, 10);

    time_label = lv_label_create(tile);
    lv_obj_set_style_text_font(time_label, &font_7seg, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xFF1A1A), 0);
    lv_label_set_text(time_label, "00:00:00");
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 10);

    /* 12 小时制时的 AM/PM 小标（右侧） */
    ampm_label = lv_label_create(tile);
    lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ampm_label, lv_color_hex(0xFF1A1A), 0);
    lv_label_set_text(ampm_label, "AM");
    lv_obj_align(ampm_label, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(ampm_label, LV_OBJ_FLAG_HIDDEN);

    sec_bar = lv_bar_create(tile);
    lv_obj_set_size(sec_bar, 600, 5);
    lv_obj_align(sec_bar, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_bar_set_range(sec_bar, 0, 59);
    lv_bar_set_value(sec_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sec_bar, lv_color_hex(0x1A0505), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sec_bar, lv_color_hex(0xFF1A1A), LV_PART_INDICATOR);

    /* 亮度提示（右上角，调节时短暂显示） */
    brightness_hint = lv_label_create(tile);
    lv_obj_set_style_text_font(brightness_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(brightness_hint, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(brightness_hint, "100%");
    lv_obj_align(brightness_hint, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_obj_add_flag(brightness_hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
}

/* ----------------------------- 信息页 ----------------------------- */
static void build_info_tile(lv_obj_t *tile)
{
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0A0F0A), 0);

    lv_obj_t *title = lv_label_create(tile);
    lv_obj_set_style_text_font(title, &font_cjk_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x76FF03), 0);
    lv_label_set_text(title, "设备信息");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    imu_label = lv_label_create(tile);
    lv_obj_set_style_text_font(imu_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(imu_label, lv_color_hex(0xE0E0E0), 0);
    lv_label_set_text(imu_label, "IMU: --");
    lv_obj_align(imu_label, LV_ALIGN_CENTER, 0, -10);

    batt_label = lv_label_create(tile);
    lv_obj_set_style_text_font(batt_label, &font_cjk_16, 0);
    lv_obj_set_style_text_color(batt_label, lv_color_hex(0xB0BEC5), 0);
    lv_label_set_text(batt_label, "BAT: --");
    lv_obj_align(batt_label, LV_ALIGN_CENTER, 0, 28);
}

/* ----------------------------- 设置页 ----------------------------- */
/* 菜单行：左侧两行文字（标题 + 功能说明），右侧留给控件 */
static lv_obj_t *menu_add_row(lv_obj_t *list, int index,
                              const char *title, const char *desc)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, LOGIC_W - 24, 48);
    lv_obj_set_pos(row, 12, 6 + index * 54);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x141A22), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 8, 0);

    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, &font_cjk_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xF0F0F0), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *d = lv_label_create(row);
    lv_obj_set_style_text_font(d, &font_cjk_12, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(0x808A94), 0);
    lv_label_set_text(d, desc);
    lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 0, 0);

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
    lv_obj_t *r0 = menu_add_row(list, 0, "亮度", "调整屏幕背光亮度");
    brightness_slider = lv_slider_create(r0);
    lv_obj_set_width(brightness_slider, 240);
    /* 右移 18px：滑块圆点约超出本体右缘 16px，避免越出行边界 */
    lv_obj_align(brightness_slider, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, s_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, slider_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 1 声音 */
    lv_obj_t *r1 = menu_add_row(list, 1, "声音", "每秒滴答提示音");
    lv_obj_t *sw1 = lv_switch_create(r1);
    lv_obj_align(sw1, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_sound_on) lv_obj_add_state(sw1, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw1, switch_sound_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 2 24 小时制 */
    lv_obj_t *r2 = menu_add_row(list, 2, "24 小时制", "切换 12/24 小时显示");
    lv_obj_t *sw2 = lv_switch_create(r2);
    lv_obj_align(sw2, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_use_24h) lv_obj_add_state(sw2, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw2, switch_24h_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 3 秒进度条 */
    lv_obj_t *r3 = menu_add_row(list, 3, "秒进度条", "显示秒进度指示条");
    lv_obj_t *sw3 = lv_switch_create(r3);
    lv_obj_align(sw3, LV_ALIGN_RIGHT_MID, 0, 0);
    if (s_show_sec_bar) lv_obj_add_state(sw3, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw3, switch_secbar_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 4 主题色：直接列出可选色块，点击即选中 */
    lv_obj_t *r4 = menu_add_row(list, 4, "主题色", "选择时钟主题颜色");
    lv_obj_t *sw = lv_obj_create(r4);
    lv_obj_remove_style_all(sw);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sw, (int)THEME_COUNT * 38 - 8, 30);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    for (int i = 0; i < (int)THEME_COUNT; i++) {
        lv_obj_t *b = lv_btn_create(sw);
        lv_obj_set_size(b, 30, 30);
        lv_obj_set_pos(b, i * 38, 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(THEME_COLORS[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, (i == s_theme_idx) ? 3 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(b, theme_swatch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        theme_swatches[i] = b;
    }

    /* 5 时间重置 */
    lv_obj_t *r5 = menu_add_row(list, 5, "时间重置", "用编译时间校准 RTC");
    lv_obj_t *btn5 = lv_btn_create(r5);
    lv_obj_set_size(btn5, 90, 34);
    lv_obj_align(btn5, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *bl5 = lv_label_create(btn5);
    lv_obj_set_style_text_font(bl5, &font_cjk_16, 0);
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

static void theme_swatch_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)THEME_COUNT) return;
    s_theme_idx = idx;
    apply_theme();
    for (int i = 0; i < (int)THEME_COUNT; i++) {
        if (theme_swatches[i]) {
            lv_obj_set_style_border_width(theme_swatches[i], (i == idx) ? 3 : 0, 0);
        }
    }
}

static void reset_time_btn_cb(lv_event_t *e)
{
    rtc_set_from_build();
    s_last_sec = -1;
}


/* ================= 音频：滴答声（MP3 提取的 PCM）+ core1 常驻喂 I2S ================= */
#define PA_CTRL_PIN    0
#define AUDIO_RING_SIZE 8192
#define KEY_LONG_MS    1500



static volatile int16_t  audio_ring[AUDIO_RING_SIZE];
static volatile uint32_t audio_head = 0;
static volatile uint32_t audio_tail = 0;

static bool     s_screen_on    = true;
static bool     s_audio_muted  = true;
static uint32_t s_last_tick_ms = 0;

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
    const int16_t *buf = high ? tick_pcm_a : tick_pcm_b;
    for (int i = 0; i < TICK_PCM_N; i++) {
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
        uint32_t raw = 0;
        for (int i = 0; i < 8; i++) raw += adc_read();
        float v = (raw / 8.0f) * 3.299f / 4096.0f * 3.0f;   /* 分压比 3:1 */
        snprintf(b, sizeof(b), "电量 %d%%   %.2fV",
                 battery_percent(v), (double)v);
        lv_label_set_text(batt_label, b);
    }
}

/* ================= 辅助 ================= */
/* 单节锂电池电压 -> 电量百分比（轻载放电曲线，线性插值） */
static int battery_percent(float v)
{
    static const float volt[] = {
        4.20f, 4.10f, 4.00f, 3.90f, 3.80f, 3.75f,
        3.70f, 3.65f, 3.60f, 3.50f, 3.40f, 3.30f, 3.20f
    };
    static const int pct[] = {
        100, 90, 80, 65, 50, 40, 32, 25, 20, 12, 7, 3, 0
    };
    const int n = (int)(sizeof(pct) / sizeof(pct[0]));
    if (v >= volt[0])     return 100;
    if (v <= volt[n - 1]) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (v <= volt[i] && v >= volt[i + 1]) {
            float f = (v - volt[i + 1]) / (volt[i] - volt[i + 1]);
            return pct[i + 1] + (int)((pct[i] - pct[i + 1]) * f + 0.5f);
        }
    }
    return 0;
}

static void update_clock_ui(datetime_t *t)
{
    char buf[40];
    if (s_use_24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->hour, t->min, t->sec);
        if (ampm_label) lv_obj_add_flag(ampm_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        int h12 = t->hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h12, t->min, t->sec);
        if (ampm_label) {
            lv_label_set_text(ampm_label, t->hour < 12 ? "AM" : "PM");
            lv_obj_clear_flag(ampm_label, LV_OBJ_FLAG_HIDDEN);
        }
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
    if (time_ghost) lv_obj_set_style_text_color(time_ghost, lv_color_hex(c), 0);
    if (ampm_label) lv_obj_set_style_text_color(ampm_label, lv_color_hex(c), 0);
    if (date_label) {
        lv_obj_set_style_text_color(date_label, lv_color_hex(c), 0);
        lv_obj_set_style_text_opa(date_label, LV_OPA_60, 0);
    }
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
