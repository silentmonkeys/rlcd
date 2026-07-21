# port_bsp — 硬件抽象层

RLCD 板子（Waveshare ESP32-S3-RLCD-4.2）的底层 C/C++ 驱动。对外暴露纯 C API，
UI 层通过这些 API 访问硬件，不直接碰 ESP-IDF。

## 文件

| 文件 | 职责 |
|---|---|
| display_bsp.cpp/h | SPI3 驱动 RLCD：RLCD_SetPixel + RLCD_Display 整帧刷新（屏不支持局部刷新）。引脚见 main/user_config.h |
| i2c_bsp.c/h | I2C0 主机 + SHTC3 温湿度驱动（SDA=13, SCL=14）。纯 C API |
| adc_bsp.c/h | 电池电压 ADC1_CH3（GPIO4）：oneshot + 曲线校准 + ×3 分压还原。3.0V→0% / 4.12V→100% |
| sdcard_bsp.c/h | SDMMC 1-line（CLK=38 CMD=21 D0=39）挂载 /sdcard；`SdcardBsp_ProbeAndRemount` 做软件热插拔探活（无 CD 引脚，靠 CMD13 探活，由 user_app 每 5s 调用）|
| button_bsp.c/h | 3 按键（BOOT/KEY/PWR 中 PWR 已悬空）+ multi_button + 5ms esp_timer 状态机 |
| multi_button/ | third-party 按键状态机库 |

## 重要提醒

- 按键引脚已内联在 button_bsp.c 顶部（避免 port_bsp 反过来依赖 main）。板上实测需要修改请改那两个宏。
- 原 PRIV_REQUIRES main 已删除（会造成循环依赖）。
