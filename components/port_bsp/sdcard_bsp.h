// SD 卡 BSP —— SDMMC 1-line 模式（板载 TF 槽）
//
// Waveshare ESP32-S3-RLCD-4.2 的 TF 槽走 SDMMC，参考 02_ESP-IDF/06_SD_Card 的
// 默认接线：CLK=GPIO38 / CMD=GPIO21 / D0=GPIO39，1-bit width。
//
// SdcardBsp_Init() 尝试挂载 /sdcard；未插卡或读卡失败返回非 ESP_OK，UI 层
// 显示"SD 未连接"。挂载后 SdcardBsp_QueryUsage() 用 statvfs 读容量/用量。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>

// 挂载点固定为 "/sdcard"，返回 ESP_OK 表示挂载成功。
esp_err_t SdcardBsp_Init(void);

// 是否已成功挂载
bool      SdcardBsp_IsMounted(void);

// 查询容量（MB）。未挂载返回 false，total_mb/used_mb 不改。
bool      SdcardBsp_QueryUsage(uint32_t *total_mb, uint32_t *used_mb);

// 热插拔感知 —— 建议定期调用（例如每 5 秒）：
//   * 当前已挂载 → statvfs 验证；失败 → 卸载 + 标记未挂载（等于用户拔了卡）
//   * 当前未挂载 → 尝试 Init（等于用户刚插卡）
// 返回操作后的挂载状态。任何 IO 均在本函数内完成，不影响其他任务。
bool      SdcardBsp_ProbeAndRemount(void);

// 挂载点 —— 固定 "/sdcard"，方便上层拼路径
#define SDCARD_MOUNT_POINT   "/sdcard"

#ifdef __cplusplus
}
#endif
