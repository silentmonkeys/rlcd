// ui_weather —— 天气详情页
//
// 布局：
//   - 顶部状态栏（WiFi + 电池）
//   - 居中标题：天气详情
//   - 顶部大卡片：城市 + 天气描述 + 当前温度 + 体感温度
//   - 下方 2×2 卡片，每卡 3 行数据（不显示分组标题）
//   - 底部横线（贴近屏幕底端） + 页码点
//
// 下方 2×2 卡片内容分组（仅注释说明，UI 不显示分组标题）：
//   [左上] 空气     —— 湿度 / 气压 / 能见度
//   [右上] 风       —— 风向 / 风速 / 紫外线
//   [左下] 今日     —— 最低 / 最高 / 云量
//   [右下] 日照     —— 日出 / 日落 / 更新

#include "ui_weather.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_font.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define SCR_W   400
#define SCR_H   300

// 状态栏
#define STATUS_Y        6
#define WIFI_ICON_X     14
#define BATT_ICON_X     342

// 标题
#define TITLE_Y         30

// 顶部大卡片（当前天气汇总）
#define TOP_CARD_X      9
#define TOP_CARD_Y      54
#define TOP_CARD_W      382
#define TOP_CARD_H      50

// 下方 2×2 网格
#define CARD_W          186
#define CARD_H          72
#define CARD_GAP_X     6
#define CARD_GAP_Y     6
#define CARD_X0        9
#define CARD_Y0        114

// 卡片内布局（不显示卡片标题）
#define ITEM_Y0        10           // 第一行距卡片顶 10px
#define ITEM_H         20           // 行高
#define LABEL_W        36    // 缩小标签宽度（3汉字刚好48px，但留margin）
#define VAL_W          (CARD_W - CARD_PAD_X * 2 - LABEL_W)  // 值宽度 = 186-28-36=122px
#define CARD_PAD_X     14

// 底部横线 + 页码点（贴近屏幕底端）
#define BOTTOM_LINE_Y   278
#define DOT_Y           291
#define DOT_R           3
#define DOT_SPACING     14

// ------------ 页面私有状态 ---------------------------------------------
static lv_obj_t *s_screen = NULL;
static char s_buf[80];
static ui_status_bar_t *s_bar;      // 状态栏（动态刷新）

// 顶部大卡片的 label
static lv_obj_t *lbl_top_city;
static lv_obj_t *lbl_top_weather;
static lv_obj_t *lbl_top_temp;
static lv_obj_t *lbl_top_feels;

// 4 张下方卡片的 label
static lv_obj_t *lbl_humi, *lbl_pressure, *lbl_visibility;
static lv_obj_t *lbl_wind_dir, *lbl_wind_speed, *lbl_uv;
static lv_obj_t *lbl_temp_min, *lbl_temp_max, *lbl_cloud;
static lv_obj_t *lbl_sunrise, *lbl_sunset, *lbl_update;

// 添加卡片内一行（标签 + 值）
static lv_obj_t *card_add_row(lv_obj_t *parent, int card_x, int card_y,
                                int row_index, const char *label)
{
    int y = card_y + ITEM_Y0 + row_index * ITEM_H;
    lv_obj_t *l = ui_make_label(parent, ui_font_cjk_16(), card_x + CARD_PAD_X, y, label);
    lv_obj_set_width(l, LABEL_W);
    lv_obj_set_height(l, 16);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);  // 标签也截断（如"能见度"超长）
    lv_obj_t *v = ui_make_label(parent, ui_font_cjk_16(),
                                 card_x + CARD_PAD_X + LABEL_W, y, "--");
    lv_obj_set_width(v, VAL_W);
    lv_obj_set_height(v, 16);   // 固定高度，防止换行
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    return v;
}

static void make_card(lv_obj_t *parent, int x, int y)
{
    ui_rounded_frame(parent, x, y, CARD_W, CARD_H, 10, 2);
}

// -------- 页面构建 -------------------------------------------------
lv_obj_t *ui_weather_create(void)
{
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    const ui_model_t *m = ui_model_get();
    s_bar = ui_page_create_scaffold(s_screen, "天气详情", 1,
                                    m->wifi_rssi, m->wifi_connected,
                                    m->battery_percent, m->battery_charging);

    // 顶部大卡片：城市在左，天气 & 温度在右
    ui_rounded_frame(s_screen, TOP_CARD_X, TOP_CARD_Y, TOP_CARD_W, TOP_CARD_H, 10, 2);
    // 左半：城市 + 天气描述
    lbl_top_city = ui_make_label(s_screen, ui_font_cjk_16(),
                                  TOP_CARD_X + 16, TOP_CARD_Y + 8, "--");
    lv_obj_set_width(lbl_top_city, 180);
    lv_obj_set_style_text_align(lbl_top_city, LV_TEXT_ALIGN_LEFT, 0);

    lbl_top_weather = ui_make_label(s_screen, ui_font_cjk_16(),
                                     TOP_CARD_X + 16, TOP_CARD_Y + 26, "--");
    lv_obj_set_width(lbl_top_weather, 180);
    lv_obj_set_style_text_align(lbl_top_weather, LV_TEXT_ALIGN_LEFT, 0);

    // 右半：温度 + 体感
    lbl_top_temp = ui_make_label(s_screen, ui_font_cjk_16(),
                                  TOP_CARD_X + 200, TOP_CARD_Y + 8, "--");
    lv_obj_set_width(lbl_top_temp, 170);
    lv_obj_set_style_text_align(lbl_top_temp, LV_TEXT_ALIGN_LEFT, 0);

    lbl_top_feels = ui_make_label(s_screen, ui_font_cjk_16(),
                                   TOP_CARD_X + 200, TOP_CARD_Y + 26, "--");
    lv_obj_set_width(lbl_top_feels, 170);
    lv_obj_set_style_text_align(lbl_top_feels, LV_TEXT_ALIGN_LEFT, 0);

    // 下方 2×2 卡片
    int x1 = CARD_X0;
    int x2 = x1 + CARD_W + CARD_GAP_X;
    int y1 = CARD_Y0;
    int y2 = y1 + CARD_H + CARD_GAP_Y;

    // ---- 左上：空气 —— 湿度 / 气压 / 能见度 ----
    make_card(s_screen, x1, y1);
    lbl_humi       = card_add_row(s_screen, x1, y1, 0, "湿度");
    lbl_pressure   = card_add_row(s_screen, x1, y1, 1, "气压");
    lbl_visibility = card_add_row(s_screen, x1, y1, 2, "能见度");

    // ---- 右上：风 —— 风向 / 风速 / 紫外线 ----
    make_card(s_screen, x2, y1);
    lbl_wind_dir   = card_add_row(s_screen, x2, y1, 0, "风向");
    lbl_wind_speed = card_add_row(s_screen, x2, y1, 1, "风速");
    lbl_uv         = card_add_row(s_screen, x2, y1, 2, "紫外线");

    // ---- 左下：今日 —— 最低 / 最高 / 云量 ----
    make_card(s_screen, x1, y2);
    lbl_temp_min = card_add_row(s_screen, x1, y2, 0, "最低");
    lbl_temp_max = card_add_row(s_screen, x1, y2, 1, "最高");
    lbl_cloud    = card_add_row(s_screen, x1, y2, 2, "云量");

    // ---- 右下：日照 —— 日出 / 日落 / 更新 ----
    make_card(s_screen, x2, y2);
    lbl_sunrise = card_add_row(s_screen, x2, y2, 0, "日出");
    lbl_sunset  = card_add_row(s_screen, x2, y2, 1, "日落");
    lbl_update  = card_add_row(s_screen, x2, y2, 2, "更新");

    ui_weather_apply_locked();
    return s_screen;
}

lv_obj_t *ui_weather_screen(void) { return s_screen; }

// -------- 数据 → UI ------------------------
static void set_or_dash(lv_obj_t *lbl, const char *s)
{
    if (!lbl) return;
    lv_label_set_text(lbl, (s && s[0]) ? s : "--");
}

void ui_weather_apply_locked(void)
{
    if (!s_screen) return;
    const ui_model_t *m = ui_model_get();

    // 状态栏动态刷新
    ui_status_bar_update(s_bar, m->wifi_rssi, m->wifi_connected,
                          m->battery_percent, m->battery_charging);

    // 顶部大卡片
    set_or_dash(lbl_top_city, m->city);
    set_or_dash(lbl_top_weather, m->weather_text);

    if (isnan(m->outdoor_temp)) {
        lv_label_set_text(lbl_top_temp, "-- ℃");
    } else {
        snprintf(s_buf, sizeof(s_buf), "%d ℃", (int)roundf(m->outdoor_temp));
        lv_label_set_text(lbl_top_temp, s_buf);
    }

    if (isnan(m->feels_like_temp)) {
        lv_label_set_text(lbl_top_feels, "体感 --");
    } else {
        snprintf(s_buf, sizeof(s_buf), "体感 %d ℃",
                 (int)roundf(m->feels_like_temp));
        lv_label_set_text(lbl_top_feels, s_buf);
    }

    // 空气卡
    if (isnan(m->outdoor_humi)) {
        lv_label_set_text(lbl_humi, "--");
    } else {
        snprintf(s_buf, sizeof(s_buf), "%d%%", (int)roundf(m->outdoor_humi));
        lv_label_set_text(lbl_humi, s_buf);
    }
    if (m->pressure_hpa > 0) {
        snprintf(s_buf, sizeof(s_buf), "%d hPa", m->pressure_hpa);
        lv_label_set_text(lbl_pressure, s_buf);
    } else lv_label_set_text(lbl_pressure, "--");

    if (m->visibility_km > 0) {
        snprintf(s_buf, sizeof(s_buf), "%d km", m->visibility_km);
        lv_label_set_text(lbl_visibility, s_buf);
    } else lv_label_set_text(lbl_visibility, "--");

    // 风卡
    set_or_dash(lbl_wind_dir, m->wind_dir);
    if (isnan(m->wind_speed_kmh)) {
        lv_label_set_text(lbl_wind_speed, "--");
    } else {
        snprintf(s_buf, sizeof(s_buf), "%d km/h", (int)roundf(m->wind_speed_kmh));
        lv_label_set_text(lbl_wind_speed, s_buf);
    }
    if (m->uv_index >= 0) {
        snprintf(s_buf, sizeof(s_buf), "%d", m->uv_index);
        lv_label_set_text(lbl_uv, s_buf);
    } else lv_label_set_text(lbl_uv, "--");

    // 今日卡
    snprintf(s_buf, sizeof(s_buf), "%d ℃", m->temp_min);
    lv_label_set_text(lbl_temp_min, s_buf);
    snprintf(s_buf, sizeof(s_buf), "%d ℃", m->temp_max);
    lv_label_set_text(lbl_temp_max, s_buf);
    if (m->cloud_pct >= 0) {
        snprintf(s_buf, sizeof(s_buf), "%d %%", m->cloud_pct);
        lv_label_set_text(lbl_cloud, s_buf);
    } else lv_label_set_text(lbl_cloud, "--");

    // 日出日落卡
    set_or_dash(lbl_sunrise, m->sunrise);
    set_or_dash(lbl_sunset, m->sunset);
    set_or_dash(lbl_update, m->weather_update);
}
