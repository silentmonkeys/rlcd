// user_app —— 把系统里的“数据源”（时钟、温湿度、电量）挂到 ui_model。
//
// 目前：SHTC3 提供真实的室内温湿度；时间仍走 esp_timer 本机时钟（等 RTC 接入
// 再换）；电池百分比暂用固定值（等 ADC 通路接入再换）。
// 参考实现见 02_ESP-IDF/05_I2C_SHTC3。

#include "user_app.h"
#include "ui_model.h"
#include "ui_home.h"
#include "ui_pages.h"
#include "lvgl_bsp.h"
#include "i2c_bsp.h"
#include "sdcard_bsp.h"
#include "user_config.h"

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_idf_version.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

static const char *TAG = "user_app";

static bool s_shtc3_ok = false;
static int64_t s_boot_us = 0;

// 一次性收集不变的设备信息 → ui_model
static void fill_static_device_info(ui_model_t *m)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
        case CHIP_ESP32:    strcpy(m->chip_model, "ESP32");    break;
        case CHIP_ESP32S2:  strcpy(m->chip_model, "ESP32-S2"); break;
        case CHIP_ESP32S3:  strcpy(m->chip_model, "ESP32-S3"); break;
        case CHIP_ESP32C3:  strcpy(m->chip_model, "ESP32-C3"); break;
        default:            snprintf(m->chip_model, sizeof(m->chip_model),
                                     "chip%d", (int)info.model);
    }
    m->cpu_cores = info.cores;

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        m->flash_size_mb = flash_bytes / (1024 * 1024);
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(m->mac, sizeof(m->mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    strncpy(m->idf_ver, esp_get_idf_version(), sizeof(m->idf_ver) - 1);

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strncpy(m->app_ver, desc->version, sizeof(m->app_ver) - 1);
        m->app_ver[sizeof(m->app_ver) - 1] = 0;
    }

    s_boot_us = esp_timer_get_time();
}

static void refresh_dynamic_device_info(ui_model_t *m)
{
    m->free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024;
    m->uptime_sec   = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000000);
    // ip / ssid / wifi_rssi / wifi_connected 由 net_bsp 在事件里写

    // SD 卡使用量（每秒刷新一次；未挂载则清零）
    m->sd_mounted   = SdcardBsp_IsMounted();
    if (m->sd_mounted) {
        uint32_t total_mb = 0, used_mb = 0;
        if (SdcardBsp_QueryUsage(&total_mb, &used_mb)) {
            m->sd_total_mb = total_mb;
            m->sd_used_mb  = used_mb;
        }
    } else {
        m->sd_total_mb = 0;
        m->sd_used_mb  = 0;
    }

    // Flash 用量：遍历所有分区累加得到"已用"，剩余 = 总容量 - 已用。
    // 每秒统计成本可接受（分区表在 flash 顶部小段里，esp_partition_find 走缓存）。
    if (m->flash_size_mb == 0) {
        uint32_t flash_bytes = 0;
        if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
            m->flash_size_mb = flash_bytes / (1024 * 1024);
        }
    }
    {
        uint64_t used_bytes = 0;
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                          ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            const esp_partition_t *p = esp_partition_get(it);
            if (p) used_bytes += p->size;
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
        uint64_t total_bytes = (uint64_t)m->flash_size_mb * 1024 * 1024;
        m->flash_used_kb = (uint32_t)(used_bytes / 1024);
        m->flash_free_kb = (total_bytes > used_bytes)
                           ? (uint32_t)((total_bytes - used_bytes) / 1024) : 0;
    }
}

static void tick_task(void *arg)
{
    ui_model_t *m = ui_model_get();
    // 传感器 ~1s 采一次已足够，同时避免 SHTC3 频繁唤醒。
    // 每 5 秒顺带做一次 SD 卡热插拔探活（拔卡自动卸载 / 插卡自动挂载）。
    int sd_probe_countdown = 0;
    for (;;) {
        time_t now = time(NULL);
        struct tm tm_local;
        localtime_r(&now, &tm_local);

        float temp = NAN, humi = NAN;
        if (s_shtc3_ok) {
            if (Shtc3_ReadTempHumi(&temp, &humi) != ESP_OK) {
                temp = NAN;
                humi = NAN;
            }
        }

        // SD 探活（不持锁；IO 在这里做，避免阻塞 LVGL）
        if (--sd_probe_countdown <= 0) {
            SdcardBsp_ProbeAndRemount();
            sd_probe_countdown = 5;      // 每 5 秒一次
        }

        if (Lvgl_lock(100)) {
            m->hour    = tm_local.tm_hour;
            m->minute  = tm_local.tm_min;
            m->year    = tm_local.tm_year + 1900;
            m->month   = tm_local.tm_mon + 1;
            m->day     = tm_local.tm_mday;
            m->weekday = tm_local.tm_wday;

            m->indoor_temp = temp;
            m->indoor_humi = humi;

            // 电池：等 ADC 接入之前用占位（后续会换成真实采样）。
            if (m->battery_percent == 0) m->battery_percent = 80;
            m->battery_charging = false;

            refresh_dynamic_device_info(m);
            ui_pages_apply_locked();
            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================================
// CSV 日志：独立 10 分钟节奏，无网也会写。
//   * 天气字段 (outdoor_*, weather_text, city) 有值就写，没值留空
//   * SD 未插 / 未挂载 → fopen 失败静默跳过
//   * 时间用系统本地时间：SNTP 未同步前是 1970-01-01，写出的行也认；等联网 SNTP
//     校时后自然会切到真实时间。
// ============================================================================
#define CSV_LOG_PATH   RLCD_DATA_DIR "/weather_log.csv"
#define CSV_HEADER     "timestamp,indoor_temp,indoor_humi,outdoor_temp,outdoor_humi,weather,city,wifi_rssi\n"
#define CSV_LOG_PERIOD_MS   (10 * 60 * 1000)

static void csv_field_f(char *dst, size_t n, float v)
{
    if (v != v) { dst[0] = 0; return; }   // NaN → 空字段
    snprintf(dst, n, "%.1f", v);
}

static void csv_log_append_once(const ui_model_t *m)
{
    // SD 未挂载就别 fopen（避免 VFS 报一堆 no-op 错误）
    if (!SdcardBsp_IsMounted()) {
        ESP_LOGD(TAG, "csv_log: SD 未挂载，跳过");
        return;
    }

    struct stat st;
    bool need_header = (stat(CSV_LOG_PATH, &st) != 0);

    mkdir(RLCD_DATA_DIR, 0777);   // 确保数据目录存在（已存在无害）
    FILE *f = fopen(CSV_LOG_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "csv_log: fopen 失败（SD 空间满 / 只读？）");
        return;
    }
    if (need_header) fwrite(CSV_HEADER, 1, strlen(CSV_HEADER), f);

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    char it[12], ih[12], ot[12], oh[12];
    csv_field_f(it, sizeof(it), m->indoor_temp);
    csv_field_f(ih, sizeof(ih), m->indoor_humi);
    csv_field_f(ot, sizeof(ot), m->outdoor_temp);
    csv_field_f(oh, sizeof(oh), m->outdoor_humi);

    fprintf(f, "%s,%s,%s,%s,%s,%s,%s,%d\n",
            ts, it, ih, ot, oh,
            m->weather_text[0] ? m->weather_text : "",
            m->city[0] ? m->city : "",
            (int)m->wifi_rssi);
    fclose(f);
    ESP_LOGI(TAG, "csv_log 追加一行 (%s)", ts);
}

static void csv_log_task(void *arg)
{
    (void)arg;
    // 首次延迟 15s，让 SHTC3 有值、SNTP 尽量校完
    vTaskDelay(pdMS_TO_TICKS(15 * 1000));
    for (;;) {
        csv_log_append_once(ui_model_get());
        vTaskDelay(pdMS_TO_TICKS(CSV_LOG_PERIOD_MS));
    }
}

void UserApp_AppInit(void)
{
    // 先收集静态设备信息（免受锁影响）
    fill_static_device_info(ui_model_get());

    // I2C 主机总线（SDA=13, SCL=14），SHTC3 挂在上面
    esp_err_t err = I2cBus_Init(I2C_SCL_PIN, I2C_SDA_PIN, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init fail: %d", err);
        return;
    }
    err = Shtc3_Init();
    if (err == ESP_OK) {
        s_shtc3_ok = true;
        ESP_LOGI(TAG, "SHTC3 online");
    } else {
        ESP_LOGW(TAG, "SHTC3 init fail: %d，室内温湿度将保持 NaN", err);
    }
    // TODO: PCF85063 RTC + ADC 电池电压采样
}

void UserApp_TaskInit(void)
{
    xTaskCreatePinnedToCore(tick_task, "user_tick", 4 * 1024, NULL, 2, NULL, 1);
    // CSV 日志独立任务：每 10 分钟一次，无网也写；SD 未插时静默跳过。
    // 栈 4 KiB 够用（fopen/fprintf/localtime + 几个小缓冲）。
    xTaskCreatePinnedToCore(csv_log_task, "csv_log", 4 * 1024, NULL, 1, NULL, 0);
}
