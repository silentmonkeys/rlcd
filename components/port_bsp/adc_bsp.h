// 电池电压 ADC 采样接口。移植自 02_ESP-IDF/03_ADC_Test，纯 C 暴露。
//
// 通道：ADC1 CH3（GPIO4），12dB 衰减，12bit，curve-fitting 校准。
// 板上电池经分压接入，读数需 ×3 还原实际电压（见参考例程）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 初始化 ADC1 oneshot + 曲线拟合校准。重复调用安全（只初始化一次）。
esp_err_t Adc_PortInit(void);

// 读一次电池电压（V）。内部取 8 次采样均值抑制 ADC 噪声，分压已还原（×3）。
// 未初始化或任一次采样失败返回 0。
float Adc_GetBatteryVoltage(void);

// 电池百分比 0..100：3.0V→0%，4.12V→100%，线性夹取。
uint8_t Adc_GetBatteryLevel(void);

// 同上，但直接用调用方已有的电压值换算 —— 避免"先读电压再读电量"重复采两轮。
uint8_t Adc_LevelFromVoltage(float volts);

#ifdef __cplusplus
}
#endif
