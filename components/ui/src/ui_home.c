// ui_home —— 400×300 单色屏主界面
//
// 布局（对齐参考图 c55dbce3f5f89a14c49b1fa0d7538be2.jpg）：
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  📶                                          🔋 60%       │  ← 状态栏 0..30
//   │                                                          │
//   │              2  2  :  4  8                              │  ← 时钟 34..144（居中）
//   │  星期            (^▽^) 舒适            日期           │  ← 星期(温度卡中心) + 心情(中) + 日期(天气卡中心)
//   │    ↑                ↑                   ↑            │     （三者同行 y=158）
//   │ 温度卡中心       屏幕中心           天气卡中心        │
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

// 设备端要真正拿 LVGL 互斥；模拟器单线程渲染，锁退化成 no-op（同 ui_pages.c）。
#ifdef ESP_PLATFORM
#include "lvgl_bsp.h"
#else
static inline bool Lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
static inline void Lvgl_unlock(void) {}
#endif

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
// 未采集到数据的字段必须初始化成哨兵（NaN / UI_INT_NA / UI_TEMP_NA），
// 否则天气页在首次拉取成功前会显示虚假的 "0 ℃ / 0 % / 体感 0"。
static ui_model_t s_model = {
    .hour = 12, .minute = 0,
    .year = 2026, .month = 7, .day = 15, .weekday = 3,
    .indoor_temp = NAN, .indoor_humi = NAN,
    .outdoor_temp = NAN, .weather_code = 0,
    .weather_text = "", .city = "", .weather_update = "",
    // 天气详情页字段：全部标记为"无数据"
    .outdoor_humi = NAN, .wind_speed_kmh = NAN, .feels_like_temp = NAN,
    .wind_dir = "",
    .cloud_pct = UI_INT_NA, .uv_index = UI_INT_NA,
    .pressure_hpa = 0, .visibility_km = 0,
    .temp_min = UI_TEMP_NA, .temp_max = UI_TEMP_NA,
    .sunrise = "", .sunset = "",
    .wifi_connected = false, .wifi_rssi = 0,
    .battery_percent = 0,
    // BOT 页：xiaozhi 未配置/离线，情绪中性，无对话内容
    .bot_state = UI_BOT_ST_OFFLINE, .bot_emotion = UI_BOT_EMO_NEUTRAL,
    .bot_xz_status = UI_BOT_XZ_UNSET, .bot_xz_code = "",
    .bot_chat_user = "", .bot_chat_reply = "",
};

ui_model_t *ui_model_get(void) { return &s_model; }

// -------- 控件句柄 -------------------------------------------------
static lv_obj_t *clock_widget;
static lv_obj_t *lbl_weekday;         // 日期行左侧：星期
static lv_obj_t *lbl_date_num;        // 日期行右侧：MM-DD
static lv_obj_t *lbl_mood;            // 日期行居中：心情表情
static lv_obj_t *lbl_temp_val;
static lv_obj_t *lbl_humi_val;
static lv_obj_t *lbl_weather_val;
static lv_obj_t *weather_icon_img;    // 40×40 lv_img，按 weather_code 加载位图
static ui_status_bar_t *s_bar;        // 状态栏（动态刷新）
static int       s_last_weather_code = -1;
static bool      s_colon_blink = true;

static const char *WEEKDAY_CN[7] = { "星期日","星期一","星期二","星期三","星期四","星期五","星期六" };

// ===============================================================
// 心情表情 —— 根据室内温度 + 湿度查表，显示颜文字 + 中文描述
// ===============================================================

typedef struct {
    const char *emoji;   // 颜文字，如 "(^▽^)"
    const char *label;   // 中文描述，如 "舒适"
} mood_entry_t;

// 温度档位：0=<10℃  1=10..17  2=18..25  3=26..31  4=≥32
static int temp_level(float t)
{
    if (t < 10.0f)  return 0;
    if (t < 18.0f)  return 1;
    if (t < 26.0f)  return 2;
    if (t < 32.0f)  return 3;
    return 4;
}

// 湿度档位：0=<40%  1=40..65%  2=>65%
static int humi_level(float h)
{
    if (h < 40.0f)  return 0;
    if (h <= 65.0f) return 1;
    return 2;
}

// 查找表：MOOD_MAP[温度档][湿度档]
// 设计原则：湿度低→"干/凉"，适中→"舒适"，高→"潮/闷"
static const mood_entry_t MOOD_MAP[5][3] = {
    /* <10℃ */ {
        {"(+_+)", "干冷"},
        {"(o_o)", "寒冷"},
        {"(~_~)","湿冷"},
    },
    /* 10..17℃ */ {
        {"(-_-)", "凉·干"},
        {"(^_^)", "微凉"},
        {"(o~o)", "凉·潮"},
    },
    /* 18..25℃ */ {
        {"(^○^)","舒适·干"},
        {"(^▽^)", "舒适"},
        {"(~▽~)","舒适·潮"},
    },
    /* 26..31℃ */ {
        {"(=_=)", "干热"},
        {"(*_*)", "微热"},
        {"(○~○)","闷热"},
    },
    /* ≥32℃ */ {
        {"(T_T)", "暴晒"},
        {"(O_O)", "炎热"},
        {"(#_#)","蒸笼"},
    },
};

static void render_mood(float temp, float humi)
{
    if (isnan(temp) || isnan(humi)) {
        lv_label_set_text(lbl_mood, "");
        return;
    }
    const mood_entry_t *e = &MOOD_MAP[temp_level(temp)][humi_level(humi)];
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s", e->emoji, e->label);
    lv_label_set_text(lbl_mood, buf);
    // 居中：左边界 = 温度卡左边 (CARD_X0)，右边界 = 天气卡右边
    lv_point_t size;
    lv_txt_get_size(&size, buf, ui_font_mood_16(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int left_bound  = CARD_X0;
    int right_bound = CARD_X0 + 2 * (CARD_W + CARD_GAP) + CARD_W; // 天气卡右边
    int center_x = (left_bound + right_bound) / 2;
    lv_obj_set_x(lbl_mood, center_x - size.x / 2);
}

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
                                 s_model.battery_percent);

    // --- 时钟（居中）-----------------------------------------------
    int cw = lcd_clock_width();
    int cx = (SCR_W - cw) / 2;
    clock_widget = lcd_clock_create(scr, cx, 42);
    lcd_clock_set_time(clock_widget, 0, 0, true);

    // --- 星期(温度卡中心) + 心情(中) + 日期(天气卡中心) ----------------
    // 三者同行（y=158，与心情表情同高），星期 / 日期分别居中在温度 / 天气卡中心
    // 横坐标：按卡片中心 − 文本半宽，apply 里动态测量后设定
    lbl_weekday  = ui_make_label(scr, ui_font_cjk_16(), 0, 158, "星期日");
    lbl_date_num = ui_make_label(scr, ui_font_cjk_16(), 0, 158, "--- --");

    // 心情表情：居中，位置由 render_mood 测量后调整
    lbl_mood = ui_make_label(scr, ui_font_mood_16(), 0, 158, "");

    // --- 三张卡片 --------------------------------------------------
    // 温度卡（标题 + 数值均居中）
    {
        int x = CARD_X0;
        ui_rounded_frame(scr, x, CARD_Y, CARD_W, CARD_H, 12, 2);
        lv_obj_t *lbl_temp_title = ui_make_label(scr, ui_font_cjk_16(), x, CARD_Y + 16, "温度");
        lv_obj_set_width(lbl_temp_title, CARD_W);
        lv_obj_set_style_text_align(lbl_temp_title, LV_TEXT_ALIGN_CENTER, 0);
        lbl_temp_val = ui_make_label(scr, ui_font_digit_mid(), x, CARD_Y + 52, "--℃");
        lv_obj_set_width(lbl_temp_val, CARD_W);
        lv_obj_set_style_text_align(lbl_temp_val, LV_TEXT_ALIGN_CENTER, 0);
    }
    // 湿度卡（标题 + 数值均居中）
    {
        int x = CARD_X0 + CARD_W + CARD_GAP;
        ui_rounded_frame(scr, x, CARD_Y, CARD_W, CARD_H, 12, 2);
        lv_obj_t *lbl_humi_title = ui_make_label(scr, ui_font_cjk_16(), x, CARD_Y + 16, "湿度");
        lv_obj_set_width(lbl_humi_title, CARD_W);
        lv_obj_set_style_text_align(lbl_humi_title, LV_TEXT_ALIGN_CENTER, 0);
        lbl_humi_val = ui_make_label(scr, ui_font_digit_mid(), x, CARD_Y + 52, "--%");
        lv_obj_set_width(lbl_humi_val, CARD_W);
        lv_obj_set_style_text_align(lbl_humi_val, LV_TEXT_ALIGN_CENTER, 0);
    }
    // 天气卡（图标槽 + 中文天气名）
    {
        int x = CARD_X0 + 2 * (CARD_W + CARD_GAP);
        ui_rounded_frame(scr, x, CARD_Y, CARD_W, CARD_H, 12, 2);
        ui_make_label(scr, ui_font_cjk_16(), x + 18, CARD_Y + 16, "天气");

        // 图标槽：40×40，放在卡片右上 —— lv_img 按 weather_code 显示位图
        weather_icon_img = lv_img_create(scr);
        lv_obj_set_size(weather_icon_img, WICON_W, WICON_H);
        lv_obj_set_pos(weather_icon_img, x + CARD_W - WICON_W - 8, CARD_Y + 26);
        lv_obj_clear_flag(weather_icon_img, LV_OBJ_FLAG_SCROLLABLE);

        // 天气文字放槽下方，左对齐（与标题「天气」同 x），长文本截断
        lbl_weather_val = ui_make_label(scr, ui_font_cjk_16(), x + 18, CARD_Y + 60, "----");
        lv_obj_set_width(lbl_weather_val, CARD_W - 18);
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

    // 星期 (温度卡中心) + 日期 (天气卡中心)
    lv_label_set_text(lbl_weekday, WEEKDAY_CN[(s_model.weekday % 7 + 7) % 7]);
    snprintf(buf, sizeof(buf), "%02d-%02d", s_model.month, s_model.day);
    lv_label_set_text(lbl_date_num, buf);
    // 居中：测量文本宽度，按卡片中心 − 文本半宽定位
    lv_point_t wd_size;
    lv_txt_get_size(&wd_size, WEEKDAY_CN[(s_model.weekday % 7 + 7) % 7], ui_font_cjk_16(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int temp_cx = CARD_X0 + CARD_W / 2;                                   // 温度卡中心
    lv_obj_set_x(lbl_weekday, temp_cx - wd_size.x / 2);
    lv_point_t date_size;
    lv_txt_get_size(&date_size, buf, ui_font_cjk_16(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int wea_cx = CARD_X0 + 2 * (CARD_W + CARD_GAP) + CARD_W / 2;          // 天气卡中心
    lv_obj_set_x(lbl_date_num, wea_cx - date_size.x / 2);

    // 心情表情：根据室内温湿度查表 (居中)
    render_mood(s_model.indoor_temp, s_model.indoor_humi);

    if (isnan(s_model.indoor_temp)) {
        lv_label_set_text(lbl_temp_val, "--℃");
    } else {
        snprintf(buf, sizeof(buf), "%d℃", (int)roundf(s_model.indoor_temp));
        lv_label_set_text(lbl_temp_val, buf);
    }

    if (isnan(s_model.indoor_humi)) {
        lv_label_set_text(lbl_humi_val, "--%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", (int)roundf(s_model.indoor_humi));
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
                          s_model.battery_percent);
}

// 头文件承诺"可从任何任务调用，内部会加锁" —— 这里必须真的加锁，
// 否则从非 LVGL 任务调用就是在渲染任务眼皮下改控件树（data race）。
// 已持锁的调用方请直接用 ui_home_apply_locked()，避免自死锁。
void ui_home_request_refresh(void)
{
    if (Lvgl_lock(200)) {
        ui_home_apply_locked();
        Lvgl_unlock();
    }
}
