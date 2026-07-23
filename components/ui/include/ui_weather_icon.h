#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

// 按 weather_code 加载对应 1-bit 图标到 lv_img。
// 自动定位图标根目录：真机 = "/spiffs"，模拟器 = RLCD_FONTS_DIR。
// 加载失败（文件不存在 / 读取错误）时返回 false，调用方自行兜底。
bool ui_weather_icon_load(lv_obj_t *img, int code);

#ifdef __cplusplus
}
#endif
