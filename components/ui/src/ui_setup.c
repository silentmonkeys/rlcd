// ui_setup —— 配网提示页（简化版：先确保能正确显示）
//
// 布局：
//   - 顶部状态栏
//   - 居中标题：配网提示
//   - 中央大 WiFi 图标（3 层弧 + 底部圆点）+ 禁止斜杠
//   - 图标下方两行：AP 名称 / 浏览器访问地址
//   - 底部横线 + 页码点

#include "ui_setup.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_font.h"

#include <stdio.h>
#include <string.h>

#define SCR_W   400
#define SCR_H   300

// 状态栏
#define STATUS_Y        6
#define WIFI_ICON_X     14
#define BATT_ICON_X     342

// 标题
#define TITLE_Y         36

// 图标位置
#define ICON_CX         200
#define ICON_TOP_Y      80

// 文字
#define TIP1_Y          186
#define VAL1_Y          204
#define TIP2_Y          224
#define VAL2_Y          242

// 底部横线 + 页码点
#define BOTTOM_LINE_Y   270
#define DOT_Y           286
#define DOT_R           3
#define DOT_SPACING     14

// 页面私有状态
static lv_obj_t *s_screen = NULL;
static lv_obj_t *lbl_ap_ssid;
static lv_obj_t *lbl_ap_ip;
static ui_status_bar_t *s_bar;        // 状态栏（动态刷新）

// 大 WiFi 图标 + 禁止斜杠
static void draw_wifi_no_net(lv_obj_t *p, int cx, int y_top)
{
    // 3 层弧线 + 圆点，整体高 60px
    // 最外层弧（宽 60）
    ui_pixel_rect(p, cx - 12, y_top,      24, 3);
    ui_pixel_rect(p, cx - 20, y_top + 4,  8,  3);
    ui_pixel_rect(p, cx + 12, y_top + 4,  8,  3);
    ui_pixel_rect(p, cx - 26, y_top + 8,  6,  3);
    ui_pixel_rect(p, cx + 20, y_top + 8,  6,  3);
    // 中间弧（宽 40）
    ui_pixel_rect(p, cx - 8,  y_top + 20, 16, 3);
    ui_pixel_rect(p, cx - 14, y_top + 24, 6,  3);
    ui_pixel_rect(p, cx + 8,  y_top + 24, 6,  3);
    // 内层弧（宽 20）
    ui_pixel_rect(p, cx - 4,  y_top + 36, 8,  3);
    ui_pixel_rect(p, cx - 8,  y_top + 40, 4,  3);
    ui_pixel_rect(p, cx + 4,  y_top + 40, 4,  3);
    // 底部圆点
    lv_obj_t *dot = ui_pixel_rect(p, cx - 3, y_top + 52, 6, 6);
    lv_obj_set_style_radius(dot, 3, 0);

    // 禁止斜杠：从左下 (cx-30, y_top+62) 到右上 (cx+30, y_top-2)
    // 12 段 5x3 阶梯
    for (int i = 0; i < 12; i++) {
        int x = cx - 30 + i * 5;
        int y = y_top + 62 - i * 5;
        ui_pixel_rect(p, x, y, 5, 3);
    }
}

lv_obj_t *ui_setup_create(void)
{
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    const ui_model_t *m = ui_model_get();
    s_bar = ui_page_create_scaffold(s_screen, "配网提示", UI_PAGE_SETUP,
                                    m->wifi_rssi, m->wifi_connected,
                                    m->battery_percent);

    // 中央大 WiFi 图标 + 禁止斜杠
    draw_wifi_no_net(s_screen, ICON_CX, ICON_TOP_Y);

    // 提示行 1：手机连接
    lv_obj_t *tip1 = ui_make_label(s_screen, ui_font_cjk_16(), 0, TIP1_Y, "手机连接");
    lv_obj_set_width(tip1, SCR_W);
    lv_obj_set_style_text_align(tip1, LV_TEXT_ALIGN_CENTER, 0);

    lbl_ap_ssid = ui_make_label(s_screen, ui_font_cjk_16(), 0, VAL1_Y, "RLCD-Setup");
    lv_obj_set_width(lbl_ap_ssid, SCR_W);
    lv_obj_set_style_text_align(lbl_ap_ssid, LV_TEXT_ALIGN_CENTER, 0);

    // 提示行 2：浏览器打开
    lv_obj_t *tip2 = ui_make_label(s_screen, ui_font_cjk_16(), 0, TIP2_Y, "浏览器打开");
    lv_obj_set_width(tip2, SCR_W);
    lv_obj_set_style_text_align(tip2, LV_TEXT_ALIGN_CENTER, 0);

    lbl_ap_ip = ui_make_label(s_screen, ui_font_cjk_16(), 0, VAL2_Y, "192.168.4.1");
    lv_obj_set_width(lbl_ap_ip, SCR_W);
    lv_obj_set_style_text_align(lbl_ap_ip, LV_TEXT_ALIGN_CENTER, 0);

    ui_setup_apply_locked();
    return s_screen;
}

lv_obj_t *ui_setup_screen(void) { return s_screen; }

void ui_setup_apply_locked(void)
{
    if (!s_screen) return;
    const ui_model_t *m = ui_model_get();

    // 状态栏动态刷新
    ui_status_bar_update(s_bar, m->wifi_rssi, m->wifi_connected,
                          m->battery_percent);

    if (m->ap_ssid[0]) lv_label_set_text(lbl_ap_ssid, m->ap_ssid);
    else               lv_label_set_text(lbl_ap_ssid, "RLCD-Setup");
    if (m->ap_ip[0])   lv_label_set_text(lbl_ap_ip, m->ap_ip);
    else               lv_label_set_text(lbl_ap_ip, "192.168.4.1");
}
