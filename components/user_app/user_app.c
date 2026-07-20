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
#include "user_config.h"

#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
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
}

static void tick_task(void *arg)
{
    ui_model_t *m = ui_model_get();
    // 传感器 ~1s 采一次已足够，同时避免 SHTC3 频繁唤醒。
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
}
