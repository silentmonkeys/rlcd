#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include <esp_err.h>
// 挂载 fonts 分区 → /spiffs（本函数在 main.cpp 里显示初始化前调用）
esp_err_t UiFont_MountFs(void);
// 加载三份 binfont。UI create 前调用一次。
esp_err_t UiFont_Load(void);
#else
#include <stdbool.h>
// 模拟器：从磁盘目录加载三份 binfont（dir 里应有 ui_font_*.bin）
bool UiFont_LoadFromDir(const char *dir);
#endif

// 主界面文字（标签 / 天气 / 星期）—— 16 px CJK
const lv_font_t *ui_font_cjk_16(void);
// 时钟 HH:MM 用的 96 px 粗数字
const lv_font_t *ui_font_digit_big(void);
// 卡片数值 24℃ / 68% 用的 28 px 粗数字
const lv_font_t *ui_font_digit_mid(void);
// 主页心情表情（" (^▽^) 舒适 "）—— 16 px，自包含 ASCII + ○▽● + 标签中文字
const lv_font_t *ui_font_mood_16(void);

#ifdef __cplusplus
}
#endif
