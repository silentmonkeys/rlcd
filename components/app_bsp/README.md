# app_bsp — LVGL v9 端口

LVGL 在 ESP32-S3 上的移植层。

- lvgl_bsp.cpp/h — tick 定时器、task handler、互斥（Lvgl_lock/unlock）
- 使用 `LV_DISPLAY_RENDER_MODE_FULL`（整帧刷新）
