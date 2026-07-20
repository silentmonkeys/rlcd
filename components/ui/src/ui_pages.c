// ui_pages —— 页面切换（switch_to / next / prev / rebuild / force_refresh）+ 可见性过滤
#include "ui_pages.h"
#include "ui_home.h"
#include "ui_weather.h"
#include "ui_calendar.h"
#include "ui_device.h"
#include "ui_setup.h"
#include "ui_model.h"
#include "lvgl.h"

#ifdef ESP_PLATFORM
#include "lvgl_bsp.h"
#else
static inline bool Lvgl_lock(int timeout_ms) { (void)timeout_ms; return true; }
static inline void Lvgl_unlock(void) {}
#endif

static ui_page_id_t s_current = UI_PAGE_HOME;

// 页面可见性：SETUP 只在 ap_active=true 时可见/可切；其余页面始终可见
static bool page_visible(ui_page_id_t p)
{
    if (p == UI_PAGE_SETUP) return ui_model_get()->ap_active;
    return true;
}

void ui_pages_create(void)
{
    ui_setup_create();
    ui_device_create();
    ui_calendar_create();
    ui_weather_create();
    ui_home_create();   // home 最后建，会 lv_screen_load 到自己
    s_current = UI_PAGE_HOME;
}

// 重建：把当前页面数据全部刷新一遍（长按 BOOT）
void ui_pages_rebuild(void)
{
    if (!Lvgl_lock(500)) return;
    ui_page_id_t was = s_current;

    // 清掉所有 screen 对象。因为每页 s_screen 是模块级 static，
    // 简单起见：删除后各自 create 检测到 s_screen != NULL 就直接返回，
    // 所以先把 lv 树上的 screen 全删掉，然后重新调 create。
    // 但各页 s_screen 变量拿不到，所以 create() 会 early-return 保持原对象。
    // 这里改为直接把 ui_model 全部字段清零 + apply 一次，达到"重建视觉"目的。
    // （物理重建代价高且无必要）
    ui_pages_apply_locked();
    ui_pages_switch_to_locked(was);
    Lvgl_unlock();
}

// 强制同步（保留入口，但板上无独立 PWR 按键触发）
void ui_pages_force_refresh(void)
{
    if (Lvgl_lock(200)) {
        ui_pages_apply_locked();
        Lvgl_unlock();
    }
}

static lv_obj_t *screen_of(ui_page_id_t p)
{
    switch (p) {
        case UI_PAGE_HOME:     return ui_home_screen();
        case UI_PAGE_WEATHER:  return ui_weather_create();
        case UI_PAGE_CALENDAR: return ui_calendar_create();
        case UI_PAGE_DEVICE:   return ui_device_create();
        case UI_PAGE_SETUP:    return ui_setup_create();
        default:               return ui_home_screen();
    }
}

void ui_pages_switch_to_locked(ui_page_id_t page)
{
    if (page >= UI_PAGE_COUNT) return;
    if (!page_visible(page)) return;   // 目标页不可见，忽略
    s_current = page;
    lv_obj_t *scr = screen_of(page);
    if (scr) lv_screen_load(scr);
    ui_pages_apply_locked();
}

// 在 [0, UI_PAGE_COUNT) 循环里找到下一个可见页面
static ui_page_id_t next_visible(ui_page_id_t from, int step)
{
    // step = +1 前进，-1 后退
    int n = UI_PAGE_COUNT;
    for (int i = 1; i <= n; i++) {
        int idx = ((int)from + i * step) % n;
        if (idx < 0) idx += n;
        if (page_visible((ui_page_id_t)idx)) return (ui_page_id_t)idx;
    }
    return from;    // 一圈都不可见，留在原地
}

void ui_pages_next_locked(void) { ui_pages_switch_to_locked(next_visible(s_current, +1)); }
void ui_pages_prev_locked(void) { ui_pages_switch_to_locked(next_visible(s_current, -1)); }

void ui_pages_switch_to(ui_page_id_t page)
{
    if (Lvgl_lock(200)) { ui_pages_switch_to_locked(page); Lvgl_unlock(); }
}
void ui_pages_next(void)
{
    if (Lvgl_lock(200)) { ui_pages_next_locked(); Lvgl_unlock(); }
}
void ui_pages_prev(void)
{
    if (Lvgl_lock(200)) { ui_pages_prev_locked(); Lvgl_unlock(); }
}

ui_page_id_t ui_pages_current(void) { return s_current; }

void ui_pages_apply_locked(void)
{
    ui_home_apply_locked();
    ui_weather_apply_locked();
    ui_calendar_apply_locked();
    ui_device_apply_locked();
    ui_setup_apply_locked();

    // 如果当前是 SETUP 页但 ap_active 变 false（配网结束了），自动切回 HOME
    if (s_current == UI_PAGE_SETUP && !page_visible(UI_PAGE_SETUP)) {
        s_current = UI_PAGE_HOME;
        lv_obj_t *scr = ui_home_screen();
        if (scr) lv_screen_load(scr);
    }
}
