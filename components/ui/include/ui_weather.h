// ui_weather —— 天气详情页
//
// 展示天气各项指标，风格与主页/设备信息页一致（圆角卡片）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

lv_obj_t *ui_weather_create(void);
lv_obj_t *ui_weather_screen(void);
void      ui_weather_apply_locked(void);

#ifdef __cplusplus
}
#endif
