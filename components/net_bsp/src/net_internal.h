// net_internal.h —— net_bsp 组件内部共享声明（不对外，仅 src/ 内各 .c 使用）
//
// net_bsp 组件拆分为多个源文件，共享一批文件级状态与内部函数。这些符号定义在
// net_bsp.c，其余 net_wifi.c / net_portal.c / net_weather.c / net_calendar.c
// 通过本头 extern 引用。对外 API 仍只在 net_bsp.h。
#pragma once

#include "net_bsp.h"

#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_wifi.h>

// ------------ 事件位 --------------------------------------------------
#define BIT_WIFI_CONNECTED   BIT0    // s_wifi_events：STA 拿到 IP
#define BIT_WEATHER_KICK     BIT1    // s_weather_events：手动触发拉天气
#define OFFLINE_SETUP_THRESHOLD_US   ((int64_t)60 * 1000 * 1000)   // 断网 60s 弹 SETUP

// ------------ 共享状态（定义在 net_bsp.c） ----------------------------
extern const char        *NET_TAG;         // 统一日志 TAG "net_bsp"
extern net_config_t       s_cfg;           // 当前配置（NVS 落盘）
extern bool               s_cfg_loaded;
extern EventGroupHandle_t s_wifi_events;
extern EventGroupHandle_t s_weather_events;
extern SemaphoreHandle_t  s_scan_mux;      // 保护 esp_wifi_scan_* 一次一个用户
extern volatile bool      s_scanning;      // web 扫描进行中：disconnect 事件不抢占重连
extern int                s_retry_count;
extern httpd_handle_t     s_httpd;
extern esp_netif_t       *s_netif_sta;
extern esp_netif_t       *s_netif_ap;
extern bool               s_wifi_common_inited;
extern bool               s_want_sta;
extern int64_t            s_last_disconnected_us;  // STA 断开时间戳(us)；0=已连/未启
extern bool               s_offline_setup_shown;   // 已因超时弹过一次 SETUP

// ------------ net_wifi.c ---------------------------------------------
void wifi_common_init(void);      // WiFi 公共初始化（只跑一次）
void fill_ap_config(wifi_config_t *wc);
void fill_sta_config(wifi_config_t *wc);
void sntp_start_once(void);       // STA 拿到 IP 后启动 SNTP（幂等）
// SoftAP 广播开关：仅切 wifi_mode，netif 保留。幂等 —— 处于目标模式时直接返回。
// STA 连上时调用 softap_stop() 熄灭 AP 广播，断网/需要配网时 softap_start() 拉起。
void softap_start(void);
void softap_stop(void);

// ------------ net_portal.c -------------------------------------------
void config_httpd_start(void);    // 启动配网 HTTP 服务器（幂等）

// ------------ net_calendar.c -----------------------------------------
// 读 SD 上的日历文件，拆成三段字符串。SD 未挂载/文件不存在 → 三段空，返回 false。
bool cal_read_file(char *marks, size_t nm, char *events, size_t ne,
                   char *labels, size_t nl);
void cal_load_from_sd(void);      // 从 SD 加载并推入 UI（静默失败）
// 把三段字符串原子写回 SD 文件。SD 未挂载返回 false。
bool cal_save_to_sd(const char *marks, const char *events, const char *labels);

// ------------ net_weather.c ------------------------------------------
void weather_task(void *arg);     // 天气轮询任务（有凭据时由 NetBsp_Start 创建）
