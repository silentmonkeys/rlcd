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
    char city[32];              // QWeather 城市名或 LocationID（如 "北京" / "101180106"）
                                // **留空 = 自动定位**：用 UAPI 取公网 IP 的 district
    char weather_apikey[64];    // QWeather API Key
    char weather_host[64];      // QWeather API Host（如 "xxx.re.qweatherapi.com"）；GeoAPI 城市解析也走此主机
    char uapi_key[64];          // UAPI（uapis.cn）Key，形如 "uapi-xxx"；留空则按访客配额调用
} net_config_t;

// -------- UAPI /network/myip 结果 ---------------------------------
// 字段对应文档 200 响应；district/time_zone 仅 source=commercial 时可能返回。
// city 不是接口字段，是我们从 district（缺失时退 region 末段）归一化出来的
// 「自动城市」—— 仅当用户没手填城市时用于喂 QWeather 城市解析。
typedef struct {
    char ip[46];                // 公网 IP（留足 IPv6 空间）
    char region[64];            // "中国 广西 南宁市"
    char isp[80];               // 运营商
    char district[32];          // 行政区 "青秀区"（仅 commercial）
    char city[32];              // 归一化出的自动城市（非接口字段）
    int  retry_after_s;         // 429 时服务端给的 Retry-After 秒数，否则 0
} uapi_myip_t;

// -------- 生命周期 -----------------------------------------------
// 启动网络后台：
//   - 若 NVS 有 ssid：起 STA，连接成功后开始每 10min 拉一次天气；
//   - 否则起 SoftAP + web 配置服务器 http://192.168.4.1
// 内部会创建 1 个 event handler 和 1 个 weather 后台任务。
void NetBsp_Start(void);

// 主动触发一次天气刷新（用于按钮或 debug）
void NetBsp_TriggerWeatherFetch(void);

// 主动触发一次「自动城市」定位（门户天气页的刷新城市按钮）。
// 仅在配置里的城市为空时有效 —— 用户手填的城市永远优先，不会被自动结果覆盖。
// 实际请求由 weather_task 下一轮执行（本函数只置标志 + kick，立即返回）。
void NetBsp_TriggerCityRefresh(void);

// 读最近一次 UAPI 定位结果（公网 IP / 归属地）。没成功拉过返回 false。
bool NetBsp_GetPublicIp(uapi_myip_t *out);

// 无网看门狗的一次性检查：STA 断开超 60s 未连上 → 弹 SETUP 页。
// 不自带任务，由调用方周期性调用（user_app tick_task 的 5s 慢节拍）。
void NetBsp_OfflineWatchdogTick(void);

// OTA 自检的一次性检查：新固件首次启动处于 PENDING_VERIFY，连续稳定运行
// 满 OTA_SELFTEST_HOLD_S 秒后调 esp_ota_mark_app_valid_cancel_rollback()
// 确认可用；在那之前若 panic/看门狗复位，bootloader 自动回滚到上一个槽。
// 和无网看门狗一样不自带任务，由 user_app tick_task 的 5s 慢节拍调用；
// 非 PENDING_VERIFY 状态（含正常烧写启动）时函数自身 early-return。
void NetBsp_OtaSelfTestTick(void);

// 读/写当前配置（线程安全，会立刻落 NVS）
bool NetBsp_LoadConfig(net_config_t *out);
bool NetBsp_SaveConfig(const net_config_t *in);

// 清掉 WiFi 凭据 —— 下次启动会回 SoftAP 配网态
void NetBsp_ForgetWifi(void);

#ifdef __cplusplus
}
#endif
