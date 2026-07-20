// ui_setup —— 配网提示页
//
// 中间画大 WiFi 图标（带禁止斜杠），下方显示 AP SSID 和 IP。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

lv_obj_t *ui_setup_create(void);
lv_obj_t *ui_setup_screen(void);
void      ui_setup_apply_locked(void);

#ifdef __cplusplus
}
#endif
