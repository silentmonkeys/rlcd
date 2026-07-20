// ui_calendar —— 月历页面（含节假日高亮、控制台标注日期高亮）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 创建日历页（返回 screen 对象）
lv_obj_t *ui_calendar_create(void);

// 同步 ui_model 日期数据到日历
void ui_calendar_apply_locked(void);

// 控制台命令：标注日期（格式 MM-DD，如 "10-01"，传 NULL 清除所有标注）
void ui_calendar_mark_date(const char *mmdd);

#ifdef __cplusplus
}
#endif
