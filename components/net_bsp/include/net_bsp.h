#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

// -------- 配置项 --------------------------------------------------
// 存在 NVS 命名空间 "rlcd_cfg" 下。
typedef struct {
    char ssid[33];
    char pass[65];
    char city[32];              // 天气查询城市 (URL-safe，例如 "Beijing")
    char weather_provider[16];  // "wttr" | "qweather" | "openweather"
    char weather_apikey[64];    // qweather / openweather 用，wttr 留空
    char weather_host[64];      // qweather 用（如 "xxx.qweatherapi.com"）；GeoAPI 城市解析也走此主机
} net_config_t;

// -------- 生命周期 -----------------------------------------------
// 启动网络后台：
//   - 若 NVS 有 ssid：起 STA，连接成功后开始每 10min 拉一次天气；
//   - 否则起 SoftAP + web 配置服务器 http://192.168.4.1
// 内部会创建 1 个 event handler 和 1 个 weather 后台任务。
void NetBsp_Start(void);

// 主动触发一次天气刷新（用于按钮或 debug）
void NetBsp_TriggerWeatherFetch(void);

// 读/写当前配置（线程安全，会立刻落 NVS）
bool NetBsp_LoadConfig(net_config_t *out);
bool NetBsp_SaveConfig(const net_config_t *in);

// 清掉 WiFi 凭据 —— 下次启动会回 SoftAP 配网态
void NetBsp_ForgetWifi(void);

#ifdef __cplusplus
}
#endif
