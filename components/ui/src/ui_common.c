// ui_common —— 共享绘制原语 + 状态栏模块 + 导航点 + 页面脚手架
#include "ui_common.h"
#include "ui_model.h"
#include "ui_font.h"

#include <stdlib.h>
#include <string.h>

// ─── 几何常量 ─────────────────────────────────────────────────────
#define STATUS_Y        6
#define WIFI_ICON_X     14
#define BATT_ICON_X     342
#define SCR_W           400
#define BOTTOM_LINE_Y   278
#define DOT_Y           291
#define DOT_R           3
#define DOT_SPACING     14

// ─── 绘制原语 ─────────────────────────────────────────────────────

lv_obj_t *ui_pixel_rect(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

lv_obj_t *ui_rounded_frame(lv_obj_t *parent, int x, int y, int w, int h,
                           int radius, int border)
{
    lv_obj_t *f = lv_obj_create(parent);
    lv_obj_set_size(f, w, h);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(f, radius, 0);
    lv_obj_set_style_border_width(f, border, 0);
    lv_obj_set_style_border_color(f, lv_color_black(), 0);
    lv_obj_set_style_border_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_set_style_shadow_width(f, 0, 0);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    return f;
}

lv_obj_t *ui_make_label(lv_obj_t *parent, const lv_font_t *font,
                        int x, int y, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_pos(lbl, x, y);
    lv_label_set_text(lbl, text);
    return lbl;
}

void ui_apply_mono_bg(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_shadow_width(scr, 0, 0);
}

// ─── 内部：WiFi 绘制到容器 ─────────────────────────────────────────
static void wifi_draw(lv_obj_t *parent, int x, int y, int8_t rssi, bool connected)
{
    int segs;
    if (!connected)                       segs = 3;   // 满信号（随后划掉）
    else if (rssi >= -55)                 segs = 3;
    else if (rssi >= -65)                 segs = 2;
    else if (rssi >= -75)                 segs = 1;
    else                                  segs = 0;

    if (segs >= 1) {   // 外弧
        ui_pixel_rect(parent, x + 6,  y,     12, 2);
        ui_pixel_rect(parent, x + 3,  y + 2, 3,  2);
        ui_pixel_rect(parent, x + 18, y + 2, 3,  2);
        ui_pixel_rect(parent, x,      y + 4, 3,  2);
        ui_pixel_rect(parent, x + 21, y + 4, 3,  2);
    }
    if (segs >= 2) {   // 中弧
        ui_pixel_rect(parent, x + 8,  y + 6, 8,  2);
        ui_pixel_rect(parent, x + 5,  y + 8, 3,  2);
        ui_pixel_rect(parent, x + 16, y + 8, 3,  2);
    }
    if (segs >= 3) {   // 内弧
        ui_pixel_rect(parent, x + 9,  y + 11, 6,  2);
        ui_pixel_rect(parent, x + 7,  y + 13, 2,  2);
        ui_pixel_rect(parent, x + 15, y + 13, 2,  2);
    }
    // 圆点（始终存在）
    ui_pixel_rect(parent, x + 10, y + 17, 4, 3);

    // 无信号时：用 "\" 斜线划掉
    if (!connected) {
        for (int i = 0; i < 6; i++) {
            ui_pixel_rect(parent, x + 2 + i * 4, y + 2 + i * 3, 4, 2);
        }
    }
}

// ─── 内部：电池绘制到容器 ─────────────────────────────────────────
// 更新 bar 的 batt_fill / batt_warn 字段
static void battery_draw(lv_obj_t *parent, int x, int y, int percent, bool charging,
                         ui_status_bar_t *bar)
{
    ui_rounded_frame(parent, x, y, 40, 18, 4, 2);
    ui_pixel_rect(parent, x + 40, y + 6, 3, 6);
    int inner_x = x + 4;
    int inner_y = y + 4;
    int inner_h = 10;

    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    int segs;
    if (percent > 75)      segs = 4;
    else if (percent > 50) segs = 3;
    else if (percent > 25) segs = 2;
    else if (percent > 10) segs = 1;
    else                   segs = (percent > 0) ? 1 : 0;

    lv_obj_t *fill = ui_pixel_rect(parent, inner_x, inner_y, 1, inner_h);
    lv_obj_set_style_radius(fill, 2, 0);
    if (segs > 0) {
        lv_obj_set_width(fill, segs * 8 - 2);
    } else {
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
    }
    bar->batt_fill = fill;

    // 低电量警示下划线
    if (percent <= 10 && percent > 0) {
        lv_obj_t *warn = ui_pixel_rect(parent, x + 2, y + 18, 36, 2);
        bar->batt_warn = warn;
    }

    // 充电闪电
    if (charging && segs > 0) {
        int lx = x + 14, ly = y + 5;
        ui_pixel_rect(parent, lx + 4, ly,     4, 2);
        ui_pixel_rect(parent, lx + 2, ly + 2, 4, 2);
        ui_pixel_rect(parent, lx,     ly + 4, 4, 2);
        ui_pixel_rect(parent, lx + 2, ly + 6, 4, 2);
        ui_pixel_rect(parent, lx + 4, ly + 8, 4, 2);
    }
}

// ─── 状态栏 ───────────────────────────────────────────────────────

ui_status_bar_t *ui_status_bar_create(lv_obj_t *parent,
                                      int8_t rssi, bool connected,
                                      int percent, bool charging)
{
    ui_status_bar_t *bar = calloc(1, sizeof(ui_status_bar_t));
    if (!bar) return NULL;
    bar->root = lv_obj_create(parent);
    lv_obj_set_size(bar->root, SCR_W, 30);
    lv_obj_set_pos(bar->root, 0, STATUS_Y);
    lv_obj_set_style_bg_opa(bar->root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(bar->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(bar->root, 0, 0);
    lv_obj_set_style_border_width(bar->root, 0, 0);

    wifi_draw(bar->root, WIFI_ICON_X, 0, rssi, connected);
    battery_draw(bar->root, BATT_ICON_X, 2, percent, charging, bar);
    return bar;
}

void ui_status_bar_update(ui_status_bar_t *bar,
                          int8_t rssi, bool connected,
                          int percent, bool charging)
{
    if (!bar || !bar->root) return;
    lv_obj_clean(bar->root);
    bar->batt_fill = NULL;
    bar->batt_warn = NULL;
    wifi_draw(bar->root, WIFI_ICON_X, 0, rssi, connected);
    battery_draw(bar->root, BATT_ICON_X, 2, percent, charging, bar);
}

void ui_status_bar_destroy(ui_status_bar_t *bar)
{
    if (!bar) return;
    if (bar->root) lv_obj_delete(bar->root);
    free(bar);
}

// ─── 页码点 ───────────────────────────────────────────────────────

void ui_draw_page_dots(lv_obj_t *parent, int my_index,
                       int dot_y, int dot_r, int spacing)
{
    int total = ui_model_get()->ap_active ? 5 : 4;
    int sx = (SCR_W - total * spacing) / 2 + spacing / 2;
    for (int i = 0; i < total; i++) {
        int cx = sx + i * spacing;
        int d  = dot_r * 2;
        if (i == my_index) {
            lv_obj_t *dot = ui_pixel_rect(parent, cx - dot_r, dot_y - dot_r, d, d);
            lv_obj_set_style_radius(dot, dot_r, 0);
        } else {
            ui_rounded_frame(parent, cx - dot_r, dot_y - dot_r, d, d, dot_r, 1);
        }
    }
}

// ─── 子页面通用脚手架 ────────────────────────────────────────────

ui_status_bar_t *ui_page_create_scaffold(lv_obj_t *parent,
                                         const char *title,
                                         int page_index,
                                         int8_t rssi, bool connected,
                                         int percent, bool charging)
{
    ui_apply_mono_bg(parent);

    // 标题（居中）
    lv_obj_t *lbl = ui_make_label(parent, ui_font_cjk_16(), 0, 26, title);
    lv_obj_set_width(lbl, SCR_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    // 状态栏
    ui_status_bar_t *bar = ui_status_bar_create(parent, rssi, connected, percent, charging);

    // 底部横线 + 导航点
    ui_pixel_rect(parent, 9, BOTTOM_LINE_Y, 378, 2);
    ui_draw_page_dots(parent, page_index, DOT_Y, DOT_R, DOT_SPACING);

    return bar;
}
