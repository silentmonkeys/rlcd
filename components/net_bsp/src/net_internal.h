// net_internal.h —— net_bsp 组件内部共享声明（不对外，仅 src/ 内各 .c 使用）
//
// net_bsp 组件拆分为多个源文件，共享一批文件级状态与内部函数。这些符号定义在
// net_bsp.c，其余 net_wifi.c / net_portal.c / net_weather.c / net_calendar.c
// 通过本头 extern 引用。对外 API 仍只在 net_bsp.h。
#pragma once

#include "net_bsp.h"
#include "ui_calendar.h"   // 日历容量上限（CAL_*_BUF 依赖）

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

// ------------ 日历三段字符串缓冲大小 ----------------------------------
// 按 ui_calendar.h 的容量上限算足，保证 UI 能装下的条数一定读得回来。
// 旧代码 marks 只给 128 字节（放不下 32 条 × "MM-DD," = 192），超过 21 条静默丢失。
#define CAL_MARKS_BUF    (UI_CAL_MAX_MARKS  * 6 + 64)                    // "MM-DD," × N
#define CAL_EVENTS_BUF   (UI_CAL_MAX_EVENTS * (6 + UI_CAL_TEXT_MAX) + 64) // "MM-DD=text;" × N
#define CAL_LABELS_BUF   (UI_CAL_MAX_LABELS * (UI_CAL_TEXT_MAX + 1) + 64) // "text;" × N

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
// 天气配置（城市）被改过 —— weather_task 每轮开头检查，置位则重解析 LocationID。
// 城市 → LocationID 的解析结果缓存在 weather_task 的局部变量里，光改 NVS 不会生效；
// 有了这个标志，改城市就不必重启设备。
extern volatile bool      s_weather_city_dirty;
// 门户的「重新定位」/「公网 IP · 刷新」按钮 → 置位，weather_task 下一轮强制
// 重跑 UAPI 定位（无视每日节拍，也无视城市是否手填）。
extern volatile bool      s_uapi_city_kick;
// UAPI 最近一次定位结果（供门户「网络」页展示公网 IP / 归属地）。
// 由 weather_task 写、httpd 任务读；都是整块小结构的字段级读写，
// 读到"半新半旧"最坏只是显示上短暂不一致，不值得为它上锁。
extern uapi_myip_t        s_uapi_info;
extern bool               s_uapi_valid;

// ------------ net_http.c（组件内共享 HTTP/gzip/JSON 工具） -------------
// 一次 GET 的输入输出参数：请求前填 timeout_ms / 可选单个请求头；
// 返回后 status = HTTP 状态码（0 表示连接失败），retry_after_s = Retry-After 秒数。
typedef struct {
    int         timeout_ms;
    const char *hdr_name;      // 可选请求头名（如 "Authorization"）
    const char *hdr_val;       // 可选请求头值（如 "Bearer uapi-xxx"）
    int         status;        // 出参：HTTP 状态码
    int         retry_after_s; // 出参：429 的 Retry-After（秒），无则 0
} net_http_req_t;

// 一次 HTTPS GET → malloc 的明文 body（需要时已 gunzip），caller free。
// **非 2xx 也返回 body**（错误 JSON 在里面），状态码看 io->status。
char *net_http_get_text(const char *url, net_http_req_t *io, size_t *out_len);
char *net_gunzip(const char *gz, size_t gz_len, size_t *out_len);
void  net_url_encode(char *out, size_t out_n, const char *in);
bool  net_json_str(const char *body, const char *key, char *out, size_t out_n);
void  net_copy_utf8(char *dst, size_t cap, const char *src);

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
// 统一 JSON 回执，net_ota.c 复用（定义在 net_portal.c）
esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg);
esp_err_t send_ok(httpd_req_t *req);

// ------------ net_ota.c ----------------------------------------------
// POST /api/ota —— 接收 raw .bin 流式写入备用 app 槽，成功后切分区并重启。
esp_err_t ota_post(httpd_req_t *req);

// ------------ net_calendar.c -----------------------------------------
// 读 SD 上的日历文件，拆成三段字符串。SD 未挂载/文件不存在 → 三段空，返回 false。
bool cal_read_file(char *marks, size_t nm, char *events, size_t ne,
                   char *labels, size_t nl);
void cal_load_from_sd(void);      // 从 SD 加载并推入 UI（静默失败）
// 把三段字符串原子写回 SD 文件。SD 未挂载返回 false。
bool cal_save_to_sd(const char *marks, const char *events, const char *labels);

// ------------ net_weather.c ------------------------------------------
void weather_task(void *arg);     // 天气轮询任务（有凭据时由 NetBsp_Start 创建）

// ------------ net_uapi.c ---------------------------------------------
// 拉一次 UAPI /network/myip?source=commercial。成功返回 true 并填满 out
// （ip/region/isp/district + 归一化出的 city）。失败已在内部打日志。
bool NetBsp_FetchPublicIp(uapi_myip_t *out);
