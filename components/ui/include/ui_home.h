#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 建立主页所有控件（必须在 LVGL 已初始化且当前默认 display 已创建后调用）。
// 创建时会自动 lv_screen_load 到主页。
void ui_home_create(void);

// 主页 screen 句柄（用于页面切换）
lv_obj_t *ui_home_screen(void);

// 把 ui_model 的最新值同步到 UI 控件。可从任何任务调用，内部会加锁。
// 但如果你已经拿到 LVGL 锁，请调用 ui_home_apply_locked() 避免死锁。
void ui_home_request_refresh(void);
void ui_home_apply_locked(void);

#ifdef __cplusplus
}
#endif
