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
// 天气图标共 8 种（晴/多云/阴/雨/雪/雷/雾/未知），全部画在 40×40 的槽
// 里，风格统一：描边线稿 + 2px 线粗 + 只用纯黑。参见 draw_weather_*。

#include "ui_home.h"
#include "ui_common.h"
#include "ui_model.h"
#include "lcd_clock.h"
#include "ui_font.h"

#include "lvgl.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// picolibc / ESP-IDF 上没有 GNU 的 strcasestr —— 自己搓一个简易版
static const char *ci_strstr(const char *hay, const char *needle)
{
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (const char *p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = p[i]; if (!a) return NULL;
            if (tolower((unsigned char)a) != tolower((unsigned char)needle[i])) break;
        }
        if (i == nl) return p;
    }
    return NULL;
}

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
static lv_obj_t *weather_icon_slot;   // 40×40 空容器，图标画在里面
static ui_status_bar_t *s_bar;        // 状态栏（动态刷新）
static int       s_last_weather_kind = -1;
static bool      s_colon_blink = true;

static const char *WEEKDAY_CN[7] = { "星期日","星期一","星期二","星期三","星期四","星期五","星期六" };

// ===============================================================
// 天气图标 —— 全部画在 40×40 的槽里，槽的左上是 (x=0, y=0)
// ===============================================================

// 太阳圆盘（半径 6，圆心 cx, cy） —— 描边圆
static void draw_sun_disc(lv_obj_t *p, int cx, int cy, int r)
{
    // 用一个圆角矩形当描边圆盘：LVGL 圆角 = 半径的一半即可近似圆
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, r * 2 + 2, r * 2 + 2);
    lv_obj_set_pos(o, cx - r - 1, cy - r - 1);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, r + 1, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_black(), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// 云朵描边（W×H≈32×18，左上角 x,y）
// 顶弧 + 底横 + 两侧下凸；风格与之前一致
static void draw_cloud_shape(lv_obj_t *p, int x, int y)
{
    // 顶弧
    ui_pixel_rect(p, x + 10, y,      12, 2);
    ui_pixel_rect(p, x + 8,  y + 2,  2,  2);
    ui_pixel_rect(p, x + 22, y + 2,  2,  2);
    ui_pixel_rect(p, x + 6,  y + 4,  2,  2);
    ui_pixel_rect(p, x + 24, y + 4,  2,  4);
    // 左侧下凸
    ui_pixel_rect(p, x + 4,  y + 6,  2,  2);
    ui_pixel_rect(p, x + 2,  y + 8,  2,  4);
    ui_pixel_rect(p, x,      y + 10, 2,  4);
    // 右侧下凸
    ui_pixel_rect(p, x + 26, y + 8,  2,  4);
    ui_pixel_rect(p, x + 28, y + 10, 2,  4);
    // 左右下钩
    ui_pixel_rect(p, x,      y + 14, 2,  2);
    ui_pixel_rect(p, x + 28, y + 14, 2,  2);
    // 底横线
    ui_pixel_rect(p, x + 2,  y + 16, 26, 2);
}

// -- 晴（太阳 + 8 条辐射光线） ---------------------------------------
static void draw_weather_sunny(lv_obj_t *p)
{
    int cx = 20, cy = 20, R = 7;
    draw_sun_disc(p, cx, cy, R);
    // 8 条光线：上下左右 + 4 条对角
    // 直线光（长 5）
    ui_pixel_rect(p, cx - 1, cy - R - 6, 2, 4);    // 上
    ui_pixel_rect(p, cx - 1, cy + R + 2, 2, 4);    // 下
    ui_pixel_rect(p, cx - R - 6, cy - 1, 4, 2);    // 左
    ui_pixel_rect(p, cx + R + 2, cy - 1, 4, 2);    // 右
    // 对角光（用 2×2 阶梯 3 段）
    // 左上
    ui_pixel_rect(p, cx - R - 5, cy - R - 5, 2, 2);
    ui_pixel_rect(p, cx - R - 3, cy - R - 3, 2, 2);
    // 右上
    ui_pixel_rect(p, cx + R + 3, cy - R - 5, 2, 2);
    ui_pixel_rect(p, cx + R + 1, cy - R - 3, 2, 2);
    // 左下
    ui_pixel_rect(p, cx - R - 5, cy + R + 3, 2, 2);
    ui_pixel_rect(p, cx - R - 3, cy + R + 1, 2, 2);
    // 右下
    ui_pixel_rect(p, cx + R + 3, cy + R + 3, 2, 2);
    ui_pixel_rect(p, cx + R + 1, cy + R + 1, 2, 2);
}

// -- 多云（太阳 + 半遮挡的云） --------------------------------------
static void draw_weather_partly_cloudy(lv_obj_t *p)
{
    // 太阳在左上，光线 4 条
    int cx = 13, cy = 12, R = 6;
    draw_sun_disc(p, cx, cy, R);
    // 光线：上 / 左 + 两条对角
    ui_pixel_rect(p, cx - 1, cy - R - 5, 2, 3);
    ui_pixel_rect(p, cx - R - 5, cy - 1, 3, 2);
    ui_pixel_rect(p, cx - R - 4, cy - R - 4, 2, 2);
    ui_pixel_rect(p, cx + R + 2, cy - R - 4, 2, 2);
    // 云在右下方（覆盖太阳右下一角）
    draw_cloud_shape(p, 8, 20);
}

// -- 阴（两朵云叠加，后云偏左上、前云更实心稍大） -------------------
static void draw_weather_cloudy(lv_obj_t *p)
{
    // 后云：整体小、靠左上
    // 顶弧
    ui_pixel_rect(p, 6,  2, 10, 2);
    ui_pixel_rect(p, 4,  4, 2,  2);
    ui_pixel_rect(p, 16, 4, 2,  2);
    ui_pixel_rect(p, 2,  6, 2,  4);
    ui_pixel_rect(p, 18, 6, 2,  4);
    ui_pixel_rect(p, 4,  10, 14, 2);
    // 前云：更大、更靠右下
    draw_cloud_shape(p, 8, 18);
}

// -- 云 + 雨滴（3 竖线） --------------------------------------------
static void draw_weather_rain(lv_obj_t *p)
{
    draw_cloud_shape(p, 4, 4);
    // 3 条雨滴（错落）
    ui_pixel_rect(p,  8, 24, 2, 4);
    ui_pixel_rect(p,  9, 30, 2, 4);
    ui_pixel_rect(p, 16, 24, 2, 4);
    ui_pixel_rect(p, 17, 30, 2, 4);
    ui_pixel_rect(p, 24, 24, 2, 4);
    ui_pixel_rect(p, 25, 30, 2, 4);
}

// -- 云 + 雪花（3 朵十字） ------------------------------------------
static void draw_snowflake(lv_obj_t *p, int cx, int cy)
{
    // 3×3 十字 + 4 对角 = 类星形
    ui_pixel_rect(p, cx - 1, cy - 3, 2, 2);
    ui_pixel_rect(p, cx - 1, cy + 1, 2, 2);
    ui_pixel_rect(p, cx - 3, cy - 1, 2, 2);
    ui_pixel_rect(p, cx + 1, cy - 1, 2, 2);
    ui_pixel_rect(p, cx - 1, cy - 1, 2, 2);   // 中心
}
static void draw_weather_snow(lv_obj_t *p)
{
    draw_cloud_shape(p, 4, 4);
    draw_snowflake(p, 10, 28);
    draw_snowflake(p, 20, 32);
    draw_snowflake(p, 30, 28);
}

// -- 云 + 闪电 -------------------------------------------------------
static void draw_weather_thunder(lv_obj_t *p)
{
    draw_cloud_shape(p, 4, 4);
    // 闪电 Z 字形（宽 8 高 12）
    int lx = 16, ly = 24;
    ui_pixel_rect(p, lx + 4, ly,      4, 2);
    ui_pixel_rect(p, lx + 2, ly + 2,  4, 2);
    ui_pixel_rect(p, lx,     ly + 4,  6, 2);
    ui_pixel_rect(p, lx + 4, ly + 6,  4, 2);
    ui_pixel_rect(p, lx + 2, ly + 8,  4, 2);
    ui_pixel_rect(p, lx,     ly + 10, 4, 2);
}

// -- 雾（4 条横线，逐渐变短） ---------------------------------------
static void draw_weather_fog(lv_obj_t *p)
{
    ui_pixel_rect(p,  4,  8, 32, 2);
    ui_pixel_rect(p,  8, 14, 26, 2);
    ui_pixel_rect(p,  4, 20, 30, 2);
    ui_pixel_rect(p,  6, 26, 28, 2);
    ui_pixel_rect(p, 10, 32, 22, 2);
}

// -- 未知（问号） ----------------------------------------------------
static void draw_weather_unknown(lv_obj_t *p)
{
    // 大问号：宽 16 高 30，居中在 40×40 槽里
    // 顶部半圆
    ui_pixel_rect(p, 14,  6, 8, 2);
    ui_pixel_rect(p, 12,  8, 2, 2);
    ui_pixel_rect(p, 22,  8, 2, 2);
    ui_pixel_rect(p, 22, 10, 2, 4);
    // 右侧弯下
    ui_pixel_rect(p, 20, 14, 2, 2);
    // 中间下钩到中心
    ui_pixel_rect(p, 18, 16, 2, 4);
    ui_pixel_rect(p, 18, 20, 2, 4);
    // 点（底部方点）
    ui_pixel_rect(p, 18, 30, 4, 4);
}

// -- kind 枚举 -------------------------------------------------------
typedef enum {
    WK_SUNNY = 0,
    WK_PARTLY_CLOUDY,
    WK_CLOUDY,
    WK_RAIN,
    WK_SNOW,
    WK_THUNDER,
    WK_FOG,
    WK_UNKNOWN,
} weather_kind_t;

// 根据 weather_text（中英文混合都试）猜一个类型
static weather_kind_t classify_weather(const char *t)
{
    if (!t || !t[0]) return WK_UNKNOWN;
    // 中文关键字（UTF-8 里出现即匹配）
    if (strstr(t, "雷")) return WK_THUNDER;
    if (strstr(t, "雪")) return WK_SNOW;
    if (strstr(t, "雨")) return WK_RAIN;
    if (strstr(t, "雾") || strstr(t, "霾")) return WK_FOG;
    if (strstr(t, "多云"))                     return WK_PARTLY_CLOUDY;
    if (strstr(t, "阴"))                       return WK_CLOUDY;
    if (strstr(t, "晴"))                       return WK_SUNNY;
    // 英文（wttr.in 返回的 %C 是英文）
    if (ci_strstr(t, "thunder") || ci_strstr(t, "storm")) return WK_THUNDER;
    if (ci_strstr(t, "snow") || ci_strstr(t, "sleet"))    return WK_SNOW;
    if (ci_strstr(t, "rain") || ci_strstr(t, "shower") ||
        ci_strstr(t, "drizzle"))                          return WK_RAIN;
    if (ci_strstr(t, "fog") || ci_strstr(t, "mist") ||
        ci_strstr(t, "haze"))                             return WK_FOG;
    if (ci_strstr(t, "partly") || ci_strstr(t, "few"))    return WK_PARTLY_CLOUDY;
    if (ci_strstr(t, "cloud") || ci_strstr(t, "overcast")) return WK_CLOUDY;
    if (ci_strstr(t, "sun") || ci_strstr(t, "clear") ||
        ci_strstr(t, "fair"))                             return WK_SUNNY;
    return WK_UNKNOWN;
}

// 清空槽并按 kind 画新图标
static void render_weather_icon(weather_kind_t k)
{
    lv_obj_clean(weather_icon_slot);
    switch (k) {
        case WK_SUNNY:          draw_weather_sunny(weather_icon_slot); break;
        case WK_PARTLY_CLOUDY:  draw_weather_partly_cloudy(weather_icon_slot); break;
        case WK_CLOUDY:         draw_weather_cloudy(weather_icon_slot); break;
        case WK_RAIN:           draw_weather_rain(weather_icon_slot); break;
        case WK_SNOW:           draw_weather_snow(weather_icon_slot); break;
        case WK_THUNDER:        draw_weather_thunder(weather_icon_slot); break;
        case WK_FOG:            draw_weather_fog(weather_icon_slot); break;
        default:                draw_weather_unknown(weather_icon_slot); break;
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

        // 图标槽：40×40，放在卡片右上
        weather_icon_slot = lv_obj_create(scr);
        lv_obj_set_size(weather_icon_slot, WICON_W, WICON_H);
        lv_obj_set_pos(weather_icon_slot, x + CARD_W - WICON_W - 8, CARD_Y + 10);
        lv_obj_set_style_bg_opa(weather_icon_slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(weather_icon_slot, 0, 0);
        lv_obj_set_style_pad_all(weather_icon_slot, 0, 0);
        lv_obj_set_style_shadow_width(weather_icon_slot, 0, 0);
        lv_obj_clear_flag(weather_icon_slot, LV_OBJ_FLAG_SCROLLABLE);

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

    // 按 weather_text 切换图标（相同类型不重画，避免闪烁）
    weather_kind_t k = classify_weather(s_model.weather_text);
    if ((int)k != s_last_weather_kind) {
        s_last_weather_kind = (int)k;
        render_weather_icon(k);
    }

    // 状态栏动态刷新（WiFi 信号 + 电池电量）
    ui_status_bar_update(s_bar, s_model.wifi_rssi, s_model.wifi_connected,
                          s_model.battery_percent, s_model.battery_charging);
}

void ui_home_request_refresh(void)
{
    ui_home_apply_locked();
}
