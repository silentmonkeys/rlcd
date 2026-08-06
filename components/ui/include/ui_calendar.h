// ui_calendar —— 月历页面（含节假日高亮、控制台标注日期高亮）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// -------- 容量上限（唯一来源）------------------------------------
// 配网门户（net_portal.c）用这些值校验提交的数据，超限直接回 400，
// 而不是存进 SD 后在下面的 setter 里被静默丢弃。改这里即两端同步。
#define UI_CAL_MAX_MARKS       32   // 标注日期条数
#define UI_CAL_MAX_EVENTS      16   // 预定内容条数
#define UI_CAL_MAX_LABELS      16   // 随机预设标签条数
#define UI_CAL_TEXT_MAX        32   // 单条预定/标签文字最大字节数（含结尾 0）

// 创建日历页（返回 screen 对象）
lv_obj_t *ui_calendar_create(void);

// 同步 ui_model 日期数据到日历
void ui_calendar_apply_locked(void);

// 控制台命令：标注日期（格式 MM-DD，如 "10-01"，传 NULL 清除所有标注）
void ui_calendar_mark_date(const char *mmdd);

// 后台批量设置标注日期（覆盖现有列表）。
// 格式："MM-DD,MM-DD,..."，如 "10-01,05-01,02-14"。传 NULL / "" 清空。
void ui_calendar_set_marks(const char *csv);

// 后台设置"预定内容"：指定日期显示自定义文字（优先级高于随机预设标签）。
// 格式："MM-DD=内容;MM-DD=内容;..."，如 "01-01=元旦快乐;02-14=情人节"。
// 传 NULL / "" 清空。
void ui_calendar_set_events(const char *spec);

// 后台设置底部随机预设标签池。无当天预定时，按日期做种子固定选一条显示
//（同一天不变，跨天才换）。格式："文字1;文字2;文字3"。传 NULL / "" 清空。
void ui_calendar_set_labels(const char *spec);

#ifdef __cplusplus
}
#endif
