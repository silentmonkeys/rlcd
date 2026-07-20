// ui_device —— 设备信息页
//
// 与 ui_home 共享状态栏和绘制风格。展示：
//   芯片型号、核数、Flash 大小、剩余堆、IDF 版本、固件版本、
//   SSID、IP、MAC、RSSI、系统时间、开机时长
//
// 由 ui_pages 统一管理：pages.device.screen 是一整块 lv_obj，
// 通过 lv_screen_load() 在多个页面之间切换。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 创建设备信息页并返回其 screen 对象（未激活）
lv_obj_t *ui_device_create(void);

// 数据 → UI 同步（外部持锁调用）
void ui_device_apply_locked(void);

#ifdef __cplusplus
}
#endif
