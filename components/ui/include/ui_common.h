// 通用绘制原语 —— 供多个页面共享，保持像素风格一致
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

// 纯黑实心矩形（画线/画方块用）
lv_obj_t *ui_pixel_rect(lv_obj_t *parent, int x, int y, int w, int h);

// 空心圆角矩形（卡片/电池外框用）
lv_obj_t *ui_rounded_frame(lv_obj_t *parent, int x, int y, int w, int h,
                           int radius, int border);

// 标签（默认黑色、指定字体/位置/文本）
lv_obj_t *ui_make_label(lv_obj_t *parent, const lv_font_t *font,
                        int x, int y, const char *text);

// 把一个固定宽度的 label 设成「超长就横向循环滚动」。
// 文本没超宽时 LVGL 自己不会起动画，等同静态显示。
// speed_px_s = 每秒滚动像素数（0 用默认 40px/s）。
void ui_label_marquee(lv_obj_t *label, uint32_t speed_px_s);

// 只在文本真的变了才写。走马灯 label 必须用这个：lv_label_set_text 会重启
// 滚动动画并把进度归零，而各页 apply 是周期性调用的（1s/500ms 一次），
// 无条件重写会让动画永远停在起点。
void ui_label_set_text_if_changed(lv_obj_t *label, const char *text);

// 通用白底/黑字/无阴影/无 padding
void ui_apply_mono_bg(lv_obj_t *scr);

// ─── 状态栏（WiFi + 电池）───────────────────────────────────────────
// 状态栏由多个 pixel_rect 组成，动态刷新靠 lv_obj_clean 后重绘。
typedef struct {
    lv_obj_t *root;         // 容器，内部放 wifi + battery 的所有 pixel_rect
    lv_obj_t *batt_fill;    // 电池填充块（供快速更新）
    lv_obj_t *batt_warn;    // 低电量警示下划线（≤10% 时创建，否则 NULL）
} ui_status_bar_t;

// 创建状态栏（parent 是 screen）。返回结构体（在堆上，页面持有指针）
ui_status_bar_t *ui_status_bar_create(lv_obj_t *parent,
                                      int8_t rssi, bool connected,
                                      int percent);

// 更新状态栏（根据最新 model 值重绘）
void ui_status_bar_update(ui_status_bar_t *bar,
                          int8_t rssi, bool connected,
                          int percent);

// 销毁状态栏（页面销毁时调用）
void ui_status_bar_destroy(ui_status_bar_t *bar);

// 底部导航点 —— 根据 ap_active 动态决定总点数（5 或 4），各页面 my_index 传自身页码
void ui_draw_page_dots(lv_obj_t *parent, int my_index,
                       int dot_y, int dot_r, int spacing);

// 子页面通用脚手架：白底 + 居中标题 + 状态栏 + 底部横线 + 导航点
// 返回状态栏结构体指针（页面持有，供 apply_locked 里动态更新）。
// 标题 label 可通过 bar->root 的子对象访问，或另存句柄。
ui_status_bar_t *ui_page_create_scaffold(lv_obj_t *parent,
                                         const char *title,
                                         int page_index,
                                         int8_t rssi, bool connected,
                                         int percent);

#ifdef __cplusplus
}
#endif
