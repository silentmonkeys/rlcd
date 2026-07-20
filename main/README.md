# main — 设备入口 & 硬件引脚定义

- main.cpp：初始化 NVS / WiFi / 网卡天气 / port_bsp / app_bsp / ui_pages / user_app；注册 LVGL flush_cb（RGB565→mono 二值化，阈值 <0x7fff 黑）+ periodic 1Hz tick
- user_config.h：全板引脚定义（SPI / I2C / 按键）+ NVS 命名空间
