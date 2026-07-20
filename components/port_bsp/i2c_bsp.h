// I2C 总线 & SHTC3 温湿度传感器接口。
// 独立于 display_bsp，用纯 C 暴露，方便 user_app.c 直接调用。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"

// 初始化 I2C0 主机总线（SDA/SCL 见 user_config.h）。已初始化则直接返回。
esp_err_t I2cBus_Init(int scl_pin, int sda_pin, int i2c_port);

// SHTC3：唤醒 → 软复位 → 读 ID。返回 ESP_OK 表示传感器在线。
esp_err_t Shtc3_Init(void);

// 一次采样。失败时不修改 *temp / *humi。
esp_err_t Shtc3_ReadTempHumi(float *temp_c, float *humi_pct);

#ifdef __cplusplus
}
#endif
