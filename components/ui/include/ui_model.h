// UI 数据模型 —— UI 层从这个模型取值渲染；后台任务（传感器、天气、时钟）
// 只需要写入这里的字段并调用 ui_home_request_refresh()。避免 UI 代码直接
// 依赖任何 ESP-IDF / SDL 头文件。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // 时间
    int  hour;          // 0..23
    int  minute;        // 0..59
    int  year;          // 2026
    int  month;         // 1..12
    int  day;           // 1..31
    int  weekday;       // 0=Sun ... 6=Sat

    // 室内传感器
    float indoor_temp;  // ℃，NaN 表示无数据
    float indoor_humi;  // %，NaN 表示无数据

    // 室外天气
    float outdoor_temp;
    int   weather_code;             // 0..99 (自定义索引)
    char  weather_text[24];         // "多云"
    char  city[24];                 // "Beijing"
    char  weather_update[16];       // "15:42"

    // ---- 天气详情页用（wttr/qweather 拉取时可选填） ----
    float outdoor_humi;             // 室外湿度 %
    float wind_speed_kmh;           // 风速 km/h
    char  wind_dir[8];              // "NE" / "S" 等
    int   cloud_pct;                // 云量百分比 0..100，-1 表示无数据
    int   pressure_hpa;             // 气压 hPa
    int   visibility_km;            // 能见度 km
    float feels_like_temp;          // 体感温度 ℃
    int   uv_index;                 // 紫外线指数 0..11
    int   temp_min;                 // 今日最低温 ℃
    int   temp_max;                 // 今日最高温 ℃
    char  sunrise[8];               // "06:12"
    char  sunset[8];                // "18:45"

    // 状态
    bool     wifi_connected;
    int8_t   wifi_rssi;             // dBm, 0 表示未连接
    uint8_t  battery_percent;       // 0..100
    bool     battery_charging;

    // ---- 设备信息页用（device / net / user_app 后端填）----
    char     ip[16];                // "192.168.1.87" (STA IP)
    char     mac[18];               // "84:F7:03:6C:AA:BB"
    char     ssid[33];              // 当前连上的 AP（若已连）
    uint32_t free_heap_kb;          // 空闲堆 KB
    uint32_t flash_size_mb;         // Flash 容量 MB
    uint32_t uptime_sec;            // 开机秒数
    char     chip_model[16];        // "ESP32-S3"
    uint8_t  cpu_cores;             // 通常 2
    char     idf_ver[16];           // "v6.0.1"
    char     app_ver[24];           // "RLCD-Home 0.1"

    // ---- 配网页用（net_bsp 在 SoftAP 起来后填）----
    char     ap_ssid[33];           // 提示手机连的 AP 名，如 "RLCD-Setup"
    char     ap_ip[16];             // 提示手机浏览器访问的 IP，如 "192.168.4.1"
    bool     ap_active;             // SoftAP 是否在广播（true 时配网页可见/可切）
} ui_model_t;

// 全局单例（simulator / device 共享），初始化时字段填合理默认。
ui_model_t *ui_model_get(void);

#ifdef __cplusplus
}
#endif
