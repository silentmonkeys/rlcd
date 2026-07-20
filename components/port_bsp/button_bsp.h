// 按键 BSP —— 两个按键：
//   BOOT  短按 → 下一页（ui_pages_next）
//   BOOT  长按 → 重建全部页面（ui_pages_rebuild）
//   KEY   短按 → 上一页（ui_pages_prev）
//
// 事件流：
//   用 5ms 周期 esp_timer 驱动 multi_button 状态机；回调在 timer 上下文里
//   立刻调 ui_pages_*() —— 内部会加 LVGL 锁，安全。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ButtonBsp_Init(void);

#ifdef __cplusplus
}
#endif
