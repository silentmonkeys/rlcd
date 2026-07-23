// net_wifi.c —— WiFi STA/AP 事件处理、公共初始化、配置填充、SNTP 校时
//
// wifi_evt 处理 STA 连接/断开/拿 IP 与 AP station 加入；拿到 IP 时置
// BIT_WIFI_CONNECTED 并启动 SNTP。STA/AP 的 wifi_config 由 fill_*_config 填充。

#include "net_internal.h"
#include "ui_model.h"

#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>

// ------------ SNTP 校时 ----------------------------------------------
static bool s_sntp_started = false;

static void sntp_sync_cb(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm lt;
    localtime_r(&now, &lt);
    ESP_LOGI(NET_TAG, "SNTP sync: %04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
}

void sntp_start_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    // 中国大陆时区（无夏令时）；改地区就改这里
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = sntp_sync_cb;
    cfg.start   = true;
    esp_netif_sntp_init(&cfg);
    // 备用服务器（次要优先级）
    esp_sntp_setservername(1, "ntp1.aliyun.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    ESP_LOGI(NET_TAG, "SNTP started (TZ=CST-8)");
}

// ------------ WiFi 事件 ----------------------------------------------
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ui_model_t *m = ui_model_get();
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(NET_TAG, "STA_START → connect()");
                if (s_last_disconnected_us == 0) s_last_disconnected_us = esp_timer_get_time();
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                m->wifi_connected = false;
                m->wifi_rssi = 0;
                m->ip[0] = 0;       // 清 IP 显示
                m->ssid[0] = 0;     // 清 SSID —— 设备信息页会回落到本机 AP 名
                s_retry_count++;
                if (s_last_disconnected_us == 0) s_last_disconnected_us = esp_timer_get_time();
                wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
                ESP_LOGW(NET_TAG, "STA disconnected (reason=%d), retry #%d",
                         ev ? ev->reason : -1, s_retry_count);
                // 扫描进行中不要抢占—— scan_get 结束后会自己调 esp_wifi_connect()
                if (!s_scanning) esp_wifi_connect();
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(NET_TAG, "AP: station joined");
                break;
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_last_disconnected_us = 0;
        s_offline_setup_shown  = false;
        m->wifi_connected = true;
        m->ap_active = false;    // STA 连上了，配网页自动隐藏
        m->setup_dismissed = false;  // 下次断网 60s 后可以再弹
        // 已经在线 —— 关掉 SoftAP 广播，避免 "RLCD-Setup" 一直出现在附近扫描列表
        softap_stop();
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            m->wifi_rssi = ap.rssi;
            // 回填 SSID —— 设备信息页 NETWORK 字段
            strncpy(m->ssid, (const char *)ap.ssid, sizeof(m->ssid) - 1);
            m->ssid[sizeof(m->ssid) - 1] = 0;
        }
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        // 回填 IP —— 设备信息页 IP ADDRESS 字段
        snprintf(m->ip, sizeof(m->ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(NET_TAG, "STA got IP " IPSTR " rssi=%d  → 设置页也可从 http://" IPSTR "/ 访问",
                 IP2STR(&ev->ip_info.ip), (int)m->wifi_rssi, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, BIT_WIFI_CONNECTED);
        sntp_start_once();
    }
}

// ------------ WiFi 公共初始化（只跑一次） ------------------------------
void wifi_common_init(void)
{
    if (s_wifi_common_inited) return;
    s_wifi_common_inited = true;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,
                                                        wifi_evt, NULL, NULL));
}

// ------------ WiFi 配置填充 -------------------------------------------
void fill_ap_config(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    strcpy((char *) wc->ap.ssid, "RLCD-Setup");
    wc->ap.ssid_len       = strlen("RLCD-Setup");
    wc->ap.channel        = 1;              // 会在 APSTA 下自动跟随 STA 信道
    wc->ap.max_connection = 3;
    wc->ap.authmode       = WIFI_AUTH_OPEN;
}

void fill_sta_config(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    strncpy((char *) wc->sta.ssid,     s_cfg.ssid, sizeof(wc->sta.ssid) - 1);
    strncpy((char *) wc->sta.password, s_cfg.pass, sizeof(wc->sta.password) - 1);
    // ALL_CHANNEL_SCAN + 按信号排序：找不同信道的 AP 更稳
    wc->sta.scan_method         = WIFI_ALL_CHANNEL_SCAN;
    wc->sta.sort_method         = WIFI_CONNECT_AP_BY_SIGNAL;
    wc->sta.threshold.authmode  = WIFI_AUTH_OPEN;
    wc->sta.threshold.rssi      = -127;
    // WPA2-PMF & WPA3-transition & WPA3-only
    wc->sta.pmf_cfg.capable     = true;
    wc->sta.pmf_cfg.required    = false;
    wc->sta.sae_pwe_h2e         = WPA3_SAE_PWE_BOTH;
    wc->sta.failure_retry_cnt   = 5;
}

// ------------ SoftAP 广播开关（幂等） ---------------------------------
// 只切 wifi_mode，AP netif / AP 配置都保留 —— 切回 APSTA 后 SSID/信道无需重设。
// STA 尚未 start（s_want_sta=false 且 mode!=APSTA 的特殊场景）时不做动作。
void softap_stop(void)
{
    if (!s_wifi_common_inited) return;
    wifi_mode_t cur;
    if (esp_wifi_get_mode(&cur) != ESP_OK) return;
    if (cur == WIFI_MODE_STA) return;                   // 已经关了
    if (cur != WIFI_MODE_APSTA && cur != WIFI_MODE_AP) return;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        ESP_LOGI(NET_TAG, "SoftAP off (mode → STA)");
    } else {
        ESP_LOGW(NET_TAG, "SoftAP off failed: %s", esp_err_to_name(err));
    }
}

void softap_start(void)
{
    if (!s_wifi_common_inited) return;
    wifi_mode_t cur;
    if (esp_wifi_get_mode(&cur) != ESP_OK) return;
    if (cur == WIFI_MODE_APSTA || cur == WIFI_MODE_AP) return;   // 已经在广播
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        ESP_LOGI(NET_TAG, "SoftAP on (mode → APSTA)");
    } else {
        ESP_LOGW(NET_TAG, "SoftAP on failed: %s", esp_err_to_name(err));
    }
}
