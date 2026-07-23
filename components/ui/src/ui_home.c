// ui_home —— 400×300 单色屏主界面
//
// 布局（对齐参考图 c55dbce3f5f89a14c49b1fa0d7538be2.jpg）：
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  📶                                          🔋 60%       │  ← 状态栏 0..30
//   │                                                          │
//   │              2  2  :  4  8                              │  ← 时钟 34..144（居中）
//   │                  星期二 06-02                            │  ← 日期 150..168（居中）
//   │                                                          │
//   │  ┌────────┐  ┌────────┐  ┌────────┐                     │  ← 卡片 184..288
//   │  │  温度  │  │ 湿度 🌡 │  │ 天气 ☁ │                     │
//   │  │  24℃  │  │  68%   │  │  多云  │                     │
//   │  └────────┘  └────────┘  └────────┘                     │
//   └──────────────────────────────────────────────────────────┘
//
// 天气图标：按 weather_code 从 fonts 分区加载 QWeather 1-bit 位图（40×40），
// 见 ui_weather_icon.c。

#include "ui_home.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_weather_icon.h"
#include "lcd_clock.h"
#include "ui_font.h"

#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// -------- 常量 -----------------------------------------------------
#define SCR_W 400
#define SCR_H 300

// 状态栏
#define STATUS_Y   6
#define WIFI_ICON_X  14
#define BATT_ICON_X  342

// 卡片
#define CARD_Y   184
#define CARD_H   104
#define CARD_W   120
#define CARD_GAP 6
#define CARD_X0  11

// 天气图标槽
#define WICON_W  40
#define WICON_H  40

// -------- 全局数据 -------------------------------------------------
static ui_model_t s_model = {
    .hour = 12, .minute = 0,
    .year = 2026, .month = 7, .day = 15, .weekday = 3,
    .indoor_temp = NAN, .indoor_humi = NAN,
    .outdoor_temp = NAN, .weather_code = 0,
    .weather_text = "", .city = "", .weather_update = "",
    .wifi_connected = false, .wifi_rssi = 0,
    .battery_percent = 0, .battery_charging = false,
};

ui_model_t *ui_model_get(void) { return &s_model; }

// -------- 控件句柄 -------------------------------------------------
static lv_obj_t *clock_widget;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_temp_val;
static lv_obj_t *lbl_humi_val;
static lv_obj_t *lbl_weather_val;
static lv_obj_t *weather_icon_img;    // 40×40 lv_img，按 weather_code 加载位图
static ui_status_bar_t *s_bar;        // 状态栏（动态刷新）
static int       s_last_weather_code = -1;
static bool      s_colon_blink = true;

static const char *WEEKDAY_CN[7] = { "星期日","星期一","星期二","星期三","星期四","星期五","星期六" };

// ===============================================================
// 天气图标 —— 按 weather_code 加载 QWeather 1-bit 位图（ui_weather_icon.c）
// ===============================================================

static void render_weather_icon(int code)
{
    // weather_code > 0 时加载位图；位图缺失由 ui_weather_icon_load 内部回落 999
    if (code > 0) {
        ui_weather_icon_load(weather_icon_img, code);
    } else {
        // code == 0（尚无天气数据）：清空图标槽，避免显示上一状态残留
        lv_img_set_src(weather_icon_img, NULL);
    }
}

// -------- 主界面构建 -----------------------------------------------
static lv_obj_t *s_home_scr = NULL;
lv_obj_t *ui_home_screen(void) { return s_home_scr; }

void ui_home_create(void)
{
    if (s_home_scr) {
        lv_screen_load(s_home_scr);
        return;
    }
    // 独立 screen（不依赖 lv_scr_act）—— 便于多页面切换
    lv_obj_t *scr = lv_obj_create(NULL);
    s_home_scr = scr;
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    ui_apply_mono_bg(scr);

    // --- 状态栏（共享模块，返回的 bar 在 apply 里动态刷新）-------
    s_bar = ui_status_bar_create(scr, s_model.wifi_rssi, s_model.wifi_connected,
                                 s_model.battery_percent, s_model.battery_charging);

    // --- 时钟（居中）-----------------------------------------------
    int cw = lcd_clock_width();
    int cx = (SCR_W - cw) / 2;
    clock_widget = lcd_clock_create(scr, cx, 42);
    lcd_clock_set_time(clock_widget, 0, 0, true);

    // --- 日期（居中）-----------------------------------------------
    lbl_date = ui_make_label(scr, ui_font_cjk_16(), 0, 158, "--- --");
    lv_obj_set_width(lbl_date, SCR_W);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_CENTER, 0);

    // --- 三张卡片 --------------------------------------------------
    // 温度卡
    {
        int x = CARD_X0;
        ui_rounded_frame(scr, x, CARD_Y, CARD_W, CARD_H, 12, 2);
        ui_make_label(scr, ui_font_cjk_16(),    x + 40, CARD_Y + 16, "温度");
        lbl_temp_val = ui_make_label(scr, ui_font_digit_mid(), x + 30, CARD_Y + 52, "--℃");
    }
    // 湿度卡（右侧温度计）
    {
        int x = CARD_X0 + CARD_W + CARD_GAP;
        ui_rounded_frame(scr, x, CARD_Y, CARD_W, CARD_H, 12, 2);
        ui_make_label(scr, ui_font_cjk_16(), x + 18, CARD_Y + 16, "湿度");
        lbl_humi_val = ui_make_label(scr, ui_font_digit_mid(), x + 10, CARD_Y + 52, "--%");
    }
    // 天气卡（图标槽 + 中文天气名）
    {
        int x = CARD_X0 + 2 * (CARD_W + CARD_GAP);
        ui_rounded_frame(scr, x, CARD_Y, CARD_W, CARD_H, 12, 2);
        ui_make_label(scr, ui_font_cjk_16(), x + 18, CARD_Y + 16, "天气");

        // 图标槽：40×40，放在卡片右上 —— lv_img 按 weather_code 显示位图
        weather_icon_img = lv_img_create(scr);
        lv_obj_set_size(weather_icon_img, WICON_W, WICON_H);
        lv_obj_set_pos(weather_icon_img, x + CARD_W - WICON_W - 8, CARD_Y + 10);
        lv_obj_clear_flag(weather_icon_img, LV_OBJ_FLAG_SCROLLABLE);

        // 天气文字放槽下方，限宽 + 截断，避免长英文溢出卡片边框
        lbl_weather_val = ui_make_label(scr, ui_font_cjk_16(), x + 14, CARD_Y + 60, "----");
        lv_obj_set_width(lbl_weather_val, CARD_W - 28);
        lv_obj_set_height(lbl_weather_val, 16);
        lv_label_set_long_mode(lbl_weather_val, LV_LABEL_LONG_DOT);
    }

    ui_home_apply_locked();
    lv_screen_load(scr);
}

// -------- 数据 → UI 同步 -------------------------------------------
void ui_home_apply_locked(void)
{
    if (!s_home_scr) return;      // 页面还没建，不能刷
    char buf[48];

    s_colon_blink = !s_colon_blink;
    lcd_clock_set_time(clock_widget, s_model.hour, s_model.minute, s_colon_blink);

    const char *wd = WEEKDAY_CN[(s_model.weekday % 7 + 7) % 7];
    snprintf(buf, sizeof(buf), "%s %02d-%02d", wd, s_model.month, s_model.day);
    lv_label_set_text(lbl_date, buf);

    if (isnan(s_model.indoor_temp)) {
        lv_label_set_text(lbl_temp_val, "--℃");
    } else {
        snprintf(buf, sizeof(buf), "%d℃", (int)(s_model.indoor_temp + 0.5f));
        lv_label_set_text(lbl_temp_val, buf);
    }

    if (isnan(s_model.indoor_humi)) {
        lv_label_set_text(lbl_humi_val, "--%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", (int)(s_model.indoor_humi + 0.5f));
        lv_label_set_text(lbl_humi_val, buf);
    }

    lv_label_set_text(lbl_weather_val, s_model.weather_text[0] ? s_model.weather_text : "----");

    // 按 weather_code 切换位图图标（相同代码不重画，避免闪烁）
    int code = s_model.weather_code;
    if (code != s_last_weather_code) {
        s_last_weather_code = code;
        render_weather_icon(code);
    }

    // 状态栏动态刷新（WiFi 信号 + 电池电量）
    ui_status_bar_update(s_bar, s_model.wifi_rssi, s_model.wifi_connected,
                          s_model.battery_percent, s_model.battery_charging);
}

void ui_home_request_refresh(void)
{
    ui_home_apply_locked();
}
