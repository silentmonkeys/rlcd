// user_app —— 把系统里的“数据源”（时钟、温湿度、电量）挂到 ui_model。
//
// 目前：SHTC3 提供真实的室内温湿度；电池电量由 ADC1_CH3(GPIO4) 实测，充电状态
// 用电压趋势启发式推断；时间走系统本地时钟（SNTP 校时，RTC 待硬件到货再接）。
// 参考实现见 02_ESP-IDF/05_I2C_SHTC3、03_ADC_Test。

#include "user_app.h"
#include "ui_model.h"
#include "ui_home.h"
#include "ui_pages.h"
#include "lvgl_bsp.h"
#include "i2c_bsp.h"
#include "sdcard_bsp.h"
#include "adc_bsp.h"
#include "net_bsp.h"
#include "user_config.h"

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
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

// Flash 用量：分区表运行时不变，启动时算一次即可（原来每秒遍历一遍纯属浪费）。
static uint32_t s_flash_used_kb = 0;
static uint32_t s_flash_free_kb = 0;

// SD 用量缓存：f_getfree 是真实 IO（几十 ms），只在 5s 慢节拍的**锁外**刷新，
// 锁内只拷贝缓存值，避免阻塞优先级更高的 LVGL 渲染任务。
static uint32_t s_sd_total_cache = 0;
static uint32_t s_sd_used_cache  = 0;

// 遍历分区表累加"已用"，剩余 = 总容量 - 已用。仅启动时调一次。
static void compute_flash_usage(uint32_t flash_size_mb)
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

    uint64_t total_bytes = (uint64_t)flash_size_mb * 1024 * 1024;
    s_flash_used_kb = (uint32_t)(used_bytes / 1024);
    s_flash_free_kb = (total_bytes > used_bytes)
                      ? (uint32_t)((total_bytes - used_bytes) / 1024) : 0;
}

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

    // 分区表不变 → Flash 用量启动时算一次，之后 tick 只读缓存
    compute_flash_usage(m->flash_size_mb);
    m->flash_used_kb = s_flash_used_kb;
    m->flash_free_kb = s_flash_free_kb;

    s_boot_us = esp_timer_get_time();
}

// 慢节拍（5s）里在 LVGL 锁**外**做 SD 用量查询 —— f_getfree 会真的读卡。
static void refresh_sd_usage_cache(void)
{
    if (SdcardBsp_IsMounted()) {
        uint32_t total_mb = 0, used_mb = 0;
        if (SdcardBsp_QueryUsage(&total_mb, &used_mb)) {
            s_sd_total_cache = total_mb;
            s_sd_used_cache  = used_mb;
        }
    } else {
        s_sd_total_cache = 0;
        s_sd_used_cache  = 0;
    }
}

static void refresh_dynamic_device_info(ui_model_t *m)
{
    m->free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024;
    m->uptime_sec   = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000000);
    // ip / ssid / wifi_rssi / wifi_connected 由 net_bsp 在事件里写

    // SD 卡：挂载标志可以即时读（纯内存），容量用慢节拍在锁外刷新的缓存
    m->sd_mounted  = SdcardBsp_IsMounted();
    m->sd_total_mb = m->sd_mounted ? s_sd_total_cache : 0;
    m->sd_used_mb  = m->sd_mounted ? s_sd_used_cache  : 0;

    // Flash 用量：启动时算好的缓存（分区表运行时不变）
    m->flash_used_kb = s_flash_used_kb;
    m->flash_free_kb = s_flash_free_kb;
}

// ------------ 电池采样 + 充电趋势推断 --------------------------------
// 硬件只能测电压（无充电检测引脚），用电压趋势启发式判充电：
//   连续 CHARGE_CONFIRM 次采样净上升 > CHARGE_STEP_V → 判为充电中；
//   出现一次明显下降 → 立即清除充电态。
// 采样有噪声（ADC 侧已做 8 次均值滤波，见 adc_bsp.c），阈值仍取得保守
//（20mV / 连续 3 次），宁可漏报不误报。
//
// 采样本身（ADC IO）在 LVGL 锁**外**做，结果存到这两个缓存里；锁内只赋值。
#define CHARGE_STEP_V     0.02f
#define CHARGE_CONFIRM    3

static bool  s_adc_ok = false;
static float s_last_vbat = 0.0f;
static int   s_rise_streak = 0;

static uint8_t s_batt_pct_cache      = 80;     // ADC 不可用时的占位值
static bool    s_batt_charging_cache = false;

// 锁外：读一次电压 → 更新百分比 + 充电趋势缓存
static void sample_battery_unlocked(void)
{
    if (!s_adc_ok) return;   // 保持占位值，避免状态栏画出 0%

    // 一次采样同时用于百分比和趋势判断 —— 不要分别调 Voltage/Level，
    // 那会走两轮 ADC，两个读数还可能落在噪声的不同侧。
    float vbat = Adc_GetBatteryVoltage();
    if (vbat <= 0.0f) return;               // 采样失败：这一轮什么都不改

    s_batt_pct_cache = Adc_LevelFromVoltage(vbat);

    if (s_last_vbat > 0.0f) {
        if (vbat > s_last_vbat + CHARGE_STEP_V) {
            if (s_rise_streak < CHARGE_CONFIRM) s_rise_streak++;
        } else if (vbat < s_last_vbat - CHARGE_STEP_V) {
            s_rise_streak = 0;                 // 明显下降 → 放电
            s_batt_charging_cache = false;
        }
        // 介于两阈值之间：维持当前判断（平台期）
        if (s_rise_streak >= CHARGE_CONFIRM) s_batt_charging_cache = true;
    }
    s_last_vbat = vbat;
}

// 锁内：只做赋值，无 IO
static void apply_battery_locked(ui_model_t *m)
{
    m->battery_percent  = s_batt_pct_cache;
    m->battery_charging = s_adc_ok ? s_batt_charging_cache : false;
}

static void tick_task(void *arg)
{
    ui_model_t *m = ui_model_get();
    // 传感器 ~1s 采一次已足够，同时避免 SHTC3 频繁唤醒。
    // 每 5 秒同一节拍：SD 卡热插拔探活（拔卡自动卸载 / 插卡自动挂载）+ SD 用量
    // 查询 + 电池采样（电压变化慢，无需每秒读）+ 无网看门狗 + OTA 自检确认。
    //
    // **所有 IO（I2C / SD / ADC）都在锁外做**，锁内只做结构体赋值 + apply，
    // 否则会把优先级 5 的 LVGL 渲染任务按在互斥上，肉眼可见掉帧。
    int slow_countdown = 0;
    // 首轮先把 SD 用量填上，别让设备信息页在头 5 秒显示 0 MB
    refresh_sd_usage_cache();
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

        // 5 秒慢节拍：SD 探活 + SD 用量 + 电池采样 + 无网看门狗 + OTA 自检确认
        bool do_slow = (--slow_countdown <= 0);
        if (do_slow) {
            slow_countdown = 5;
            // SD 探活 + 容量查询（都是真实 IO，锁外做）
            SdcardBsp_ProbeAndRemount();
            refresh_sd_usage_cache();
            // 电池 ADC 采样（8 次 oneshot，锁外做）
            sample_battery_unlocked();
            // 无网看门狗（内部自行加锁切页；断网超 60s 弹 SETUP）
            NetBsp_OfflineWatchdogTick();
            // OTA 自检：新固件稳定跑满 60s 才确认可用，否则复位后自动回滚
            NetBsp_OtaSelfTestTick();
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

            apply_battery_locked(m);
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
    // 确保这一行真正落到 SD —— 拔卡/掉电最多丢未 fsync 的当前行，不破坏已有内容
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    ESP_LOGI(TAG, "csv_log 追加一行 (%s)", ts);
}

static void csv_log_task(void *arg)
{
    (void)arg;
    // 首次延迟 15s，让 SHTC3 有值、SNTP 尽量校完
    vTaskDelay(pdMS_TO_TICKS(15 * 1000));
    for (;;) {
        // 持锁**只做整体复制**，然后锁外写文件。直接把 ui_model_get() 传下去
        // 会在 weather_task 改 weather_text[24] / city[24] 的同时读，写出半新
        // 半旧的撕裂字符串；而持锁做 SD IO 又会阻塞 LVGL（见 tick_task 注释）。
        ui_model_t snap;
        if (Lvgl_lock(100)) {
            snap = *ui_model_get();
            Lvgl_unlock();
            csv_log_append_once(&snap);
        } else {
            ESP_LOGW(TAG, "csv_log: 取 LVGL 锁超时，跳过本轮");
        }
        vTaskDelay(pdMS_TO_TICKS(CSV_LOG_PERIOD_MS));
    }
}

void UserApp_AppInit(void)
{
    // 先收集静态设备信息（免受锁影响）
    fill_static_device_info(ui_model_get());

    // I2C 主机总线（SDA=13, SCL=14），SHTC3 挂在上面。
    // 失败**不能 return** —— 电池 ADC 与 I2C 完全无关，早期版本在这里直接
    // 返回，导致 SHTC3 一坏就连电量一起丢（状态栏永远显示占位 80%）。
    bool i2c_ok = false;
    esp_err_t err = I2cBus_Init(I2C_SCL_PIN, I2C_SDA_PIN, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init fail: %d（SHTC3 不可用，室内温湿度保持 NaN）", err);
    } else {
        i2c_ok = true;
    }

    if (i2c_ok) {
        err = Shtc3_Init();
        if (err == ESP_OK) {
            s_shtc3_ok = true;
            ESP_LOGI(TAG, "SHTC3 online");
        } else {
            ESP_LOGW(TAG, "SHTC3 init fail: %d，室内温湿度将保持 NaN", err);
        }
    }

    // 电池电压 ADC（ADC1_CH3/GPIO4）；独立于 I2C，始终初始化。失败则回落占位值
    if (Adc_PortInit() == ESP_OK) {
        s_adc_ok = true;
        ESP_LOGI(TAG, "电池 ADC online");
        sample_battery_unlocked();   // 立刻取一次，别让状态栏先显示占位值
    } else {
        ESP_LOGW(TAG, "电池 ADC init fail，电量将用占位值");
    }
    // TODO: PCF85063 RTC（硬件到货后接入，见 _logs/plan-refactor-2026-07-21.md）
}

void UserApp_TaskInit(void)
{
    xTaskCreatePinnedToCore(tick_task, "user_tick", 4 * 1024, NULL, 2, NULL, 1);
    // CSV 日志独立任务：每 10 分钟一次，无网也写；SD 未插时静默跳过。
    // 栈 4 KiB 够用（fopen/fprintf/localtime + 几个小缓冲）。
    xTaskCreatePinnedToCore(csv_log_task, "csv_log", 4 * 1024, NULL, 1, NULL, 0);
}
