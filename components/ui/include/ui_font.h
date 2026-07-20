#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 主界面文字（标签 / 天气 / 星期）—— 16 px CJK
const lv_font_t *ui_font_cjk_16(void);
// 时钟 HH:MM 用的 96 px 粗数字
const lv_font_t *ui_font_digit_big(void);
// 卡片数值 24℃ / 68% 用的 28 px 粗数字
const lv_font_t *ui_font_digit_mid(void);

#ifdef __cplusplus
}
#endif
