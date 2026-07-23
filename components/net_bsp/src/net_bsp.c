// net_bsp —— 网络后台入口 + 组件内共享状态定义
//
// 组件按职责拆为多个源文件（对外 API 仍只在 net_bsp.h）：
//   net_bsp.c       本文件：全局状态定义、NVS 配置读写、启动编排、无网看门狗
//   net_wifi.c      WiFi STA/AP 事件、公共初始化、配置填充、SNTP 校时
//   net_portal.c    SoftAP 配网门户 HTTP 服务器（HTML 模板见 portal_page.h）
//   net_weather.c   QWeather API 拉取 + weather_task
//   net_calendar.c  日历配置存 SD 卡（原子写）
//   net_internal.h  组件内共享声明
//
// 启动流程：
//   [boot]
//     ├── NVS 无 ssid   → AP-only，配置门户 http://192.168.4.1
//     └── NVS 有 ssid   → APSTA 同开
//                          - STA 后台反复尝试连（ALL_CHANNEL_SCAN + PMF-capable + WPA3-SAE）
//                          - AP 仍开，方便手机随时改配置
//                          - STA 拿到 IP：SNTP 启动 + 天气轮询 + 打印 STA IP 供门户访问
//
// 关于 "STA 连上后 192.168.4.1 访问不到" 的说明：
//   ESP32-S3 只有一个 2.4 GHz 射频，APSTA 模式下 AP 会被强制切到 STA 所在信道。
//   手机原本连着 "RLCD-Setup"（ch1），一旦 AP 挪信道，手机会掉线。
//   解决方式：改用 **STA IP** 访问同一个配置页（我们把它打到日志 + LVGL），
//   或者手机重新连 "RLCD-Setup" 让它跟着新信道回来。

#include "net_internal.h"
#include "ui_model.h"
#include "ui_pages.h"
#include "lvgl_bsp.h"
#include "user_config.h"

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

// ------------ 组件内共享状态（在 net_internal.h 里 extern 声明） ---------
const char        *NET_TAG = "net_bsp";
net_config_t       s_cfg;
bool               s_cfg_loaded = false;
EventGroupHandle_t s_wifi_events = NULL;
EventGroupHandle_t s_weather_events = NULL;
SemaphoreHandle_t  s_scan_mux = NULL;   // 保护 esp_wifi_scan_* 一次一个用户
volatile bool      s_scanning = false;  // web 扫描进行中：disconnect 不抢占重连
int                s_retry_count = 0;
httpd_handle_t     s_httpd = NULL;
esp_netif_t       *s_netif_sta = NULL;
esp_netif_t       *s_netif_ap  = NULL;
bool               s_wifi_common_inited = false;
bool               s_want_sta   = false;
int64_t            s_last_disconnected_us = 0;  // STA 断开时间戳(us)；0=已连/未启
bool               s_offline_setup_shown  = false;

// ------------ NVS ----------------------------------------------------
bool NetBsp_LoadConfig(net_config_t *out)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nh) != ESP_OK) return false;
    size_t sz;
    memset(out, 0, sizeof(*out));
#define LOAD_STR(field)                                                     \
    do { sz = sizeof(out->field); nvs_get_str(nh, #field, out->field, &sz); } while (0)
    LOAD_STR(ssid);
    LOAD_STR(pass);
    LOAD_STR(city);
    LOAD_STR(weather_apikey);
    LOAD_STR(weather_host);
#undef LOAD_STR
    nvs_close(nh);
    return out->ssid[0] != 0;
}

bool NetBsp_SaveConfig(const net_config_t *in)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) return false;
    nvs_set_str(nh, "ssid",             in->ssid);
    nvs_set_str(nh, "pass",             in->pass);
    nvs_set_str(nh, "city",             in->city);
    nvs_set_str(nh, "weather_apikey",   in->weather_apikey);
    nvs_set_str(nh, "weather_host",     in->weather_host);
    esp_err_t err = nvs_commit(nh);
    nvs_close(nh);
    return err == ESP_OK;
}

void NetBsp_ForgetWifi(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_erase_key(nh, "ssid");
    nvs_erase_key(nh, "pass");
    nvs_commit(nh);
    nvs_close(nh);
}

// ------------ 无网看门狗 ----------------------------------------------
// STA 断开 60s 仍未拿到 IP → 弹出 SETUP 页（前提用户未 dismiss）。
// 用户如果按键 dismiss，本次开机不再自动弹（setup_dismissed=true 由 UI 层置）。
//
// 不再自带任务：逻辑做成一次性 tick，由 user_app 的 tick_task 5s 慢节拍调用
// （见 NetBsp_OfflineWatchdogTick 声明于 net_bsp.h）。
void NetBsp_OfflineWatchdogTick(void)
{
    ui_model_t *m = ui_model_get();
    if (m->wifi_connected)           { s_last_disconnected_us = 0; s_offline_setup_shown = false; return; }
    if (s_last_disconnected_us == 0) return;
    if (m->setup_dismissed)          return;   // 用户主动隐藏了本次开机不再弹
    if (s_offline_setup_shown)       return;

    int64_t now = esp_timer_get_time();
    if (now - s_last_disconnected_us < OFFLINE_SETUP_THRESHOLD_US) return;

    // 60s 已过 —— 弹 SETUP
    s_offline_setup_shown = true;
    softap_start();   // 断网太久了，把 SoftAP 广播重新拉起来方便配网
    if (Lvgl_lock(200)) {
        m->ap_active = true;
        ui_pages_switch_to_locked(UI_PAGE_SETUP);
        Lvgl_unlock();
    }
    ESP_LOGW(NET_TAG, "offline > 60s → switch to SETUP page");
}

// ------------ 启动编排 -----------------------------------------------
void NetBsp_Start(void)
{
    if (!s_cfg_loaded) {
        strcpy(s_cfg.city, "Beijing");
        s_cfg_loaded = NetBsp_LoadConfig(&s_cfg);
        if (s_cfg.city[0] == 0)              strcpy(s_cfg.city, "Beijing");
    }
    ESP_LOGI(NET_TAG, "cfg loaded: host='%s' city='%s' apikey=%s",
             s_cfg.weather_host, s_cfg.city,
             s_cfg.weather_apikey[0] ? "set" : "EMPTY");
    // 日历配置从 SD 卡加载并推入 UI（不再走 NVS）
    cal_load_from_sd();
    s_wifi_events = xEventGroupCreate();
    if (!s_weather_events) s_weather_events = xEventGroupCreate();
    if (!s_scan_mux) s_scan_mux = xSemaphoreCreateMutex();

    wifi_common_init();

    // AP netif（配置门户永远在）
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg;
    fill_ap_config(&ap_cfg);

    s_want_sta = (s_cfg.ssid[0] != 0);

    if (!s_want_sta) {
        ESP_LOGW(NET_TAG, "no ssid in NVS → AP-only setup mode (RLCD-Setup / 192.168.4.1)");
        // 即便无 STA 也开 APSTA —— 这样 /scan 依然可用（scan 需要 STA netif）
        if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        // 一个空 STA 配置，不真的去连（ssid=""）
        wifi_config_t sta_cfg = {};
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    } else {
        ESP_LOGI(NET_TAG, "APSTA — STA target='%s' (all-channel scan, WPA3-SAE ready)", s_cfg.ssid);
        if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

        wifi_config_t sta_cfg;
        fill_sta_config(&sta_cfg);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // 无论何种模式，SoftAP 都在广播 —— 但 SETUP 页默认隐藏：
    //   * 无 NVS 凭据（s_want_sta=false）→ 立即弹 SETUP（用户必须配网）
    //   * 有凭据但暂未连上 → 等 60s 无网看门狗触发才弹
    // STA 连上时 wifi_evt 会把 ap_active 关掉
    {
        ui_model_t *m = ui_model_get();
        strncpy(m->ap_ssid, "RLCD-Setup", sizeof(m->ap_ssid) - 1);
        strncpy(m->ap_ip,   "192.168.4.1", sizeof(m->ap_ip)   - 1);
        m->ap_active = !s_want_sta;   // 无凭据 → 直接进配网态
        m->setup_dismissed = false;
        s_last_disconnected_us = esp_timer_get_time();
        s_offline_setup_shown  = !s_want_sta;   // 无凭据时"已弹"，避免看门狗再次切页
    }

    config_httpd_start();

    if (s_want_sta) {
        // 栈：mbedtls TLS 握手 + esp_crt_bundle 峰值 ~12 KiB，zlib inflate 走
        // heap 分配（内部工作缓冲不占栈），加上局部 url[320] 等，16 KiB 足够。
        xTaskCreatePinnedToCore(weather_task, "weather", 16 * 1024, NULL, 3, NULL, 0);
    }

    // 无网看门狗不再自带任务 —— 由 user_app tick_task 的 5s 慢节拍调
    // NetBsp_OfflineWatchdogTick()（无论是否有 SSID 都会跑；无凭据时启动即已切 SETUP）。

    // 无凭据 → 立即把当前页面切到 SETUP（用户必须走配网流程）
    if (!s_want_sta) {
        if (Lvgl_lock(500)) {
            ui_pages_switch_to_locked(UI_PAGE_SETUP);
            Lvgl_unlock();
        }
    }
}

void NetBsp_TriggerWeatherFetch(void)
{
    if (s_weather_events) {
        xEventGroupSetBits(s_weather_events, BIT_WEATHER_KICK);
    }
}
