// lcd_clock —— 用 LVGL rectangle 拼的 DSEG 风 7-segment HH:MM 时钟
//
// 全部段体用纯黑填充无边框 —— 匹配单色反射式 LCD 的"黑/不显示"两态。
// 组件尺寸和实际数字宽度由源文件里的常量决定，主界面通过 lcd_clock_width()/
// lcd_clock_height() 读取以便水平居中。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

// 建时钟；返回容器对象（widget 结构挂在 user_data 上）
lv_obj_t *lcd_clock_create(lv_obj_t *parent, int x, int y);

// 更新时间：hh 0..23, mm 0..59, colon_on 用于闪烁效果
void lcd_clock_set_time(lv_obj_t *widget, int hh, int mm, bool colon_on);

// 组件尺寸（用于居中计算）
int lcd_clock_width(void);
int lcd_clock_height(void);

#ifdef __cplusplus
}
#endif
