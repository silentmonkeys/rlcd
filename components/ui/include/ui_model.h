// UI 数据模型 —— UI 层从这个模型取值渲染；后台任务（传感器、天气、时钟）
// 只需要写入这里的字段并调用 ui_home_request_refresh()。避免 UI 代码直接
// 依赖任何 ESP-IDF / SDL 头文件。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// 整型字段的"无数据"哨兵。float 字段直接用 NaN，整型没有 NaN 可用：
//   * 百分比/指数类（cloud_pct / uv_index）取 -1
//   * 摄氏温度类（temp_min / temp_max）可能是真负数，取 -999
// 未初始化时必须是哨兵而不是 0 —— 否则启动后天气页会显示虚假的 "0 ℃ / 0 %"。
#define UI_INT_NA    (-1)
#define UI_TEMP_NA   (-999)

// ---- BOT 页：xiaozhi 对话接入 -----------------------------------------
// 设备状态（对齐 xiaozhi-esp32 的 DeviceState，只取 UI 关心的子集）
enum {
    UI_BOT_ST_OFFLINE = 0,   // 未配置 / 未连接
    UI_BOT_ST_IDLE,          // 空闲
    UI_BOT_ST_CONNECTING,    // 通道建立中
    UI_BOT_ST_LISTENING,     // 聆听
    UI_BOT_ST_THINKING,      // LLM 处理中
    UI_BOT_ST_SPEAKING,      // TTS 播报中
    UI_BOT_ST_ACTIVATING,    // 等待激活（bot_xz_code 有效）
    UI_BOT_ST_ERROR,         // 网络/协议错误
};
// LLM 情绪（xiaozhi llm.emotion 字符串归一化后的枚举；UI 不碰字符串）
enum {
    UI_BOT_EMO_NEUTRAL = 0,
    UI_BOT_EMO_HAPPY,        // happy/laughing/funny/delicious/winking/confident
    UI_BOT_EMO_SAD,          // sad/crying/embarrassed
    UI_BOT_EMO_ANGRY,        // angry
    UI_BOT_EMO_SURPRISED,    // surprised/shocked/silly
    UI_BOT_EMO_SLEEPY,       // sleepy/relaxed
    UI_BOT_EMO_THINKING,     // thinking
    UI_BOT_EMO_LOVING,       // loving
    UI_BOT_EMO_CONFUSED,     // confused
    UI_BOT_EMO_COOL,         // cool
};
// xiaozhi 服务通道状态（门户展示 + bot 页行为）
enum {
    UI_BOT_XZ_UNSET = 0,     // 未配置（未激活）
    UI_BOT_XZ_ACTIVATING,    // 激活中（bot_xz_code 待用户到控制台输入）
    UI_BOT_XZ_READY,         // 已激活可对话
    UI_BOT_XZ_ERROR,
};

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

    // ---- 天气详情页用（QWeather 拉取时可选填） ----
    float outdoor_humi;             // 室外湿度 %，NaN 表示无数据
    float wind_speed_kmh;           // 风速 km/h，NaN 表示无数据
    // QWeather windDir 是中文风向，最长 "无持续风向" = 15 字节；
    // 原来给 8 字节，"东北风"(9B) 被截成 7B 就断在汉字中间 → 显示 "东北□"
    char  wind_dir[20];             // "东北风" / "无持续风向"
    int   cloud_pct;                // 云量百分比 0..100，UI_INT_NA 表示无数据
    int   pressure_hpa;             // 气压 hPa，0 表示无数据
    int   visibility_km;            // 能见度 km，0 表示无数据
    float feels_like_temp;          // 体感温度 ℃，NaN 表示无数据
    int   uv_index;                 // 紫外线指数 0..11，UI_INT_NA 表示无数据
    int   temp_min;                 // 今日最低温 ℃，UI_TEMP_NA 表示无数据
    int   temp_max;                 // 今日最高温 ℃，UI_TEMP_NA 表示无数据
    char  sunrise[8];               // "06:12"
    char  sunset[8];                // "18:45"

    // 状态
    bool     wifi_connected;
    int8_t   wifi_rssi;             // dBm, 0 表示未连接
    uint8_t  battery_percent;       // 0..100

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
    bool     setup_dismissed;       // 用户按键退出配网页 → 本次开机不再自动弹

    // ---- SD 卡（port_bsp 挂载时填；未挂载则 mounted=false，容量=0）----
    bool     sd_mounted;            // SD 是否成功挂载
    uint32_t sd_total_mb;           // 总容量 MB
    uint32_t sd_used_mb;            // 已用容量 MB

    // ---- Flash 用量（user_app 每秒刷新）----
    uint32_t flash_used_kb;         // App + 分区已用 KB
    uint32_t flash_free_kb;         // 剩余 KB

    // ---- BOT 页用（net_xiaozhi 填；写入后 ui_bot_apply_locked 自动跟上）----
    int8_t   bot_state;             // UI_BOT_ST_*（驱动机器人姿态）
    int8_t   bot_emotion;           // UI_BOT_EMO_*（llm.emotion；说话时覆盖姿态）
    int8_t   bot_xz_status;         // UI_BOT_XZ_*（服务通道状态）
    char     bot_xz_code[8];        // 激活码（未激活时门户/bot 页可展示）
    char     bot_chat_user[96];     // 用户问题（STT 文本 / 门户发送的文本）
    char     bot_chat_reply[192];   // AI 回答（tts sentence_start 的最新一句）
} ui_model_t;

// 全局单例（simulator / device 共享），初始化时字段填合理默认。
ui_model_t *ui_model_get(void);

#ifdef __cplusplus
}
#endif
