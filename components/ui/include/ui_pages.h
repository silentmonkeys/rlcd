// ui_pages —— 多页面切换管理
//
// 页面顺序：
//   0. HOME     —— 时钟 + 传感器 + 天气
//   1. WEATHER  —— 天气详情
//   2. CALENDAR —— 日历月历 + 节假日 / 标注日期高亮
//   3. DEVICE   —— 设备信息
//   4. BOT      —— bloub 机器人动画
//   5. SETUP    —— 配网提示（仅在 ap_active=true 时可见 / 可切）
//
// 按键：
//   BOOT 短按     —— 下一页
//   KEY  短按     —— 上一页
//   BOOT 长按     —— ui_pages_rebuild()（重建所有页面）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_WEATHER,
    UI_PAGE_CALENDAR,
    UI_PAGE_DEVICE,
    UI_PAGE_BOT,
    UI_PAGE_SETUP,
    UI_PAGE_COUNT
} ui_page_id_t;

// 建立所有页面。必须在 LVGL 初始化后调用；返回后 HOME 已激活。
void ui_pages_create(void);

// 销毁并重建所有页面（长按 BOOT）
void ui_pages_rebuild(void);

// 强制把 ui_model 同步一遍（保留外部调用入口，但板上无独立 PWR 按键）
void ui_pages_force_refresh(void);

// 切到指定页 / 下一页 / 上一页（跳过不可见页面，如 SETUP 在 ap_active=false 时）
void ui_pages_switch_to(ui_page_id_t page);
void ui_pages_next(void);
void ui_pages_prev(void);
void ui_pages_switch_to_locked(ui_page_id_t page);
void ui_pages_next_locked(void);
void ui_pages_prev_locked(void);

// 当前页 id
ui_page_id_t ui_pages_current(void);

// 让所有页面把 ui_model 同步进各自的控件（调用方持锁）
void ui_pages_apply_locked(void);

// 用户按键在 SETUP 页 → 记录 setup_dismissed=true 并返回 HOME。
// 本次开机不再自动弹配网页（除非重启）。
void ui_pages_dismiss_setup(void);

#ifdef __cplusplus
}
#endif
