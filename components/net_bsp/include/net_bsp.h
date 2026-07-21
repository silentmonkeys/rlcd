#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

// -------- 配置项 --------------------------------------------------
// 存在 NVS 命名空间 "rlcd_cfg" 下。天气固定走 QWeather。
typedef struct {
    char ssid[33];
    char pass[65];
    char city[32];              // QWeather 城市名或 LocationID（如 "新郑" / "101180106"）
    char weather_apikey[64];    // QWeather API Key
    char weather_host[64];      // QWeather API Host（如 "xxx.re.qweatherapi.com"）；GeoAPI 城市解析也走此主机
} net_config_t;

// -------- 生命周期 -----------------------------------------------
// 启动网络后台：
//   - 若 NVS 有 ssid：起 STA，连接成功后开始每 10min 拉一次天气；
//   - 否则起 SoftAP + web 配置服务器 http://192.168.4.1
// 内部会创建 1 个 event handler 和 1 个 weather 后台任务。
void NetBsp_Start(void);

// 主动触发一次天气刷新（用于按钮或 debug）
void NetBsp_TriggerWeatherFetch(void);

// 无网看门狗的一次性检查：STA 断开超 60s 未连上 → 弹 SETUP 页。
// 不自带任务，由调用方周期性调用（user_app tick_task 的 5s 慢节拍）。
void NetBsp_OfflineWatchdogTick(void);

// 读/写当前配置（线程安全，会立刻落 NVS）
bool NetBsp_LoadConfig(net_config_t *out);
bool NetBsp_SaveConfig(const net_config_t *in);

// 清掉 WiFi 凭据 —— 下次启动会回 SoftAP 配网态
void NetBsp_ForgetWifi(void);

#ifdef __cplusplus
}
#endif
