// ui_device —— 设备信息页（2×2 卡片）
//
// 布局：
//   - 顶部状态栏
//   - 居中标题：系统信息
//   - 2×2 网格 4 张圆角卡片，每张 3 行数据（不显示分组标题）
//   - 底部横线（贴近屏幕底端） + 页码点
//
// 卡片内容分组（仅注释说明，UI 不显示分组标题）：
//   [左上] 网络   —— WiFi / IP / 名称
//   [右上] 系统   —— 运行 / 时间 / 状态
//   [左下] 环境   —— 城市 / 天气 / 读数
//   [右下] 固件   —— 芯片 / 存储 / 版本

#include "ui_device.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_font.h"

#include <stdio.h>
#include <string.h>

// 屏幕尺寸
#define SCR_W   400
#define SCR_H   300

// 状态栏
#define STATUS_Y        6
#define WIFI_ICON_X     14
#define BATT_ICON_X     342

// 标题
#define TITLE_Y         36

// 2×2 卡片网格（3 行 × 16px 行高 + 20px 标题 + 12px padding = 80px）
#define CARD_W          186
#define CARD_H          80
#define CARD_GAP_X     6
#define CARD_GAP_Y     8
#define CARD_X0        9
#define CARD_Y0        68

// 卡片内布局（不显示卡片标题，3 行内容居中垂直分布）
#define ITEM_Y0        12            // 第一行内容距卡片顶 12px
#define ITEM_H         20            // 行高
#define LABEL_W        40            // 标签宽度
#define VAL_W          (CARD_W - CARD_PAD_X * 2 - LABEL_W)  // 值宽度 = 186-28-40=118px
#define CARD_PAD_X     14            // 卡片内左右 padding

// 底部横线 + 页码点（贴近屏幕底端）
#define BOTTOM_LINE_Y   278
#define DOT_Y           291
#define DOT_R           3
#define DOT_SPACING     14

// ------------ 页面私有状态 ---------------------------------------------
static lv_obj_t *s_screen = NULL;
static char s_buf[80];
static ui_status_bar_t *s_bar;      // 状态栏（动态刷新）

// 每张卡片 3 个值 label 句柄
static lv_obj_t *lbl_wifi, *lbl_ip, *lbl_ssid;
static lv_obj_t *lbl_uptime, *lbl_time, *lbl_sensor;
static lv_obj_t *lbl_city, *lbl_weather, *lbl_reading;
static lv_obj_t *lbl_sd, *lbl_flash, *lbl_firmware;

static const char *wifi_status_str(const ui_model_t *m)
{
    if (!m->wifi_connected) return "未连接";
    snprintf(s_buf, sizeof(s_buf), "%d dBm", m->wifi_rssi);
    return s_buf;
}

static const char *sensor_reading_str(const ui_model_t *m)
{
    if (m->indoor_temp != m->indoor_temp) return "无数据";
    snprintf(s_buf, sizeof(s_buf), "%d℃/%d%%",
             (int)(m->indoor_temp + 0.5f), (int)(m->indoor_humi + 0.5f));
    return s_buf;
}

// 把 MB 值格式化成 "3.2/16GB" 或 "512/8MB" —— 大于等于 1024MB 时用 GB
static void format_capacity(char *out, size_t out_n,
                            uint32_t used_mb, uint32_t total_mb)
{
    if (total_mb >= 1024) {
        // GB 显示 —— 用带 1 位小数的格式
        float used_gb  = used_mb  / 1024.0f;
        float total_gb = total_mb / 1024.0f;
        snprintf(out, out_n, "%.1f/%.0fGB", used_gb, total_gb);
    } else {
        snprintf(out, out_n, "%u/%uMB", (unsigned)used_mb, (unsigned)total_mb);
    }
}

// 在指定卡片内添加一行
static lv_obj_t *card_add_row(lv_obj_t *parent, int card_x, int card_y,
                                int row_index, const char *label)
{
    int y = card_y + ITEM_Y0 + row_index * ITEM_H;
    lv_obj_t *l = ui_make_label(parent, ui_font_cjk_16(), card_x + CARD_PAD_X, y, label);
    lv_obj_set_width(l, LABEL_W);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *v = ui_make_label(parent, ui_font_cjk_16(),
                                 card_x + CARD_PAD_X + LABEL_W, y, "--");
    lv_obj_set_width(v, VAL_W);
    lv_obj_set_height(v, 16);   // 固定高度，防止换行
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_LEFT, 0);
    // 长文本模式：超出宽度时截断加省略号（避免 IP 长时溢出格子）
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    return v;
}

// 创建卡片外框（不带标题）
static void make_card(lv_obj_t *parent, int x, int y)
{
    ui_rounded_frame(parent, x, y, CARD_W, CARD_H, 10, 2);
}

// -------- 页面构建 -------------------------------------------------
lv_obj_t *ui_device_create(void)
{
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    const ui_model_t *m = ui_model_get();
    s_bar = ui_page_create_scaffold(s_screen, "系统信息", 3,
                                    m->wifi_rssi, m->wifi_connected,
                                    m->battery_percent, m->battery_charging);

    int x1 = CARD_X0;
    int x2 = x1 + CARD_W + CARD_GAP_X;
    int y1 = CARD_Y0;
    int y2 = y1 + CARD_H + CARD_GAP_Y;

    // ---- 左上：网络 —— WiFi / IP / 名称 ----
    make_card(s_screen, x1, y1);
    lbl_wifi = card_add_row(s_screen, x1, y1, 0, "WiFi");
    lbl_ip   = card_add_row(s_screen, x1, y1, 1, "IP");
    lv_label_set_long_mode(lbl_ip, LV_LABEL_LONG_DOT);  // IP 超长时截断显示（无动画）
    lbl_ssid = card_add_row(s_screen, x1, y1, 2, "名称");
    lv_label_set_long_mode(lbl_ssid, LV_LABEL_LONG_DOT); // SSID 超长时截断显示（无动画）

    // ---- 右上：系统 —— 运行 / 时间 / 状态 ----
    make_card(s_screen, x2, y1);
    lbl_uptime = card_add_row(s_screen, x2, y1, 0, "运行");
    lbl_time   = card_add_row(s_screen, x2, y1, 1, "时间");
    lbl_sensor = card_add_row(s_screen, x2, y1, 2, "状态");

    // ---- 左下：环境 —— 城市 / 天气 / 读数 ----
    make_card(s_screen, x1, y2);
    lbl_city    = card_add_row(s_screen, x1, y2, 0, "城市");
    lbl_weather = card_add_row(s_screen, x1, y2, 1, "天气");
    lbl_reading = card_add_row(s_screen, x1, y2, 2, "读数");

    // ---- 右下：固件 —— SD / 存储 / 版本 ----
    make_card(s_screen, x2, y2);
    lbl_sd       = card_add_row(s_screen, x2, y2, 0, "SD");
    lbl_flash    = card_add_row(s_screen, x2, y2, 1, "存储");
    lbl_firmware = card_add_row(s_screen, x2, y2, 2, "版本");

    ui_device_apply_locked();
    return s_screen;
}

// -------- 数据 → UI ------------------------
void ui_device_apply_locked(void)
{
    if (!s_screen) return;
    const ui_model_t *m = ui_model_get();

    // 状态栏动态刷新
    ui_status_bar_update(s_bar, m->wifi_rssi, m->wifi_connected,
                          m->battery_percent, m->battery_charging);

    // 左上 - 网络
    // 未连接 STA 时，IP/SSID 显示本机 SoftAP 的信息（RLCD-Setup / 192.168.4.1）
    // ——这样用户按键关闭配网页之后，网络卡片不会残留上一次连接的陈旧数据。
    lv_label_set_text(lbl_wifi, (char *)wifi_status_str(m));
    if (m->wifi_connected && m->ip[0]) {
        lv_label_set_text(lbl_ip, m->ip);
    } else {
        lv_label_set_text(lbl_ip, m->ap_ip[0] ? m->ap_ip : "--");
    }
    if (m->wifi_connected && m->ssid[0]) {
        lv_label_set_text(lbl_ssid, m->ssid);
    } else {
        lv_label_set_text(lbl_ssid, m->ap_ssid[0] ? m->ap_ssid : "--");
    }

    // 右上 - 系统
    {
        uint32_t s = m->uptime_sec;
        uint32_t hh = s / 3600, mm = (s / 60) % 60, ss = s % 60;
        snprintf(s_buf, sizeof(s_buf), "%02u:%02u:%02u",
                 (unsigned)hh, (unsigned)mm, (unsigned)ss);
        lv_label_set_text(lbl_uptime, s_buf);
    }
    snprintf(s_buf, sizeof(s_buf), "%02d:%02d", m->hour, m->minute);
    lv_label_set_text(lbl_time, s_buf);
    lv_label_set_text(lbl_sensor,
                      (m->indoor_temp != m->indoor_temp) ? "故障" : "正常");

    // 左下 - 位置
    lv_label_set_text(lbl_city, m->city[0] ? m->city : "--");
    lv_label_set_text(lbl_weather, m->weather_text[0] ? m->weather_text : "--");
    lv_label_set_text(lbl_reading, (char *)sensor_reading_str(m));

    // 右下 - 固件 / 存储
    // 卡片行左侧标签 "SD" 已经由 card_add_row 画出来了，值只需要 "3.2/32GB"
    // 或 "未连接"，不要再前缀 "SD"，否则会出现 "SD SD 3.2/32GB" 的重复。
    if (m->sd_mounted && m->sd_total_mb > 0) {
        char cap[16];
        format_capacity(cap, sizeof(cap), m->sd_used_mb, m->sd_total_mb);
        snprintf(s_buf, sizeof(s_buf), "%s", cap);
    } else {
        snprintf(s_buf, sizeof(s_buf), "未连接");
    }
    lv_label_set_text(lbl_sd, s_buf);

    // 存储：Flash 已用/总容量。总容量走 esp_flash_get_size()（真实 flash 值）
    {
        uint32_t total_mb = m->flash_size_mb;
        uint32_t total_kb = total_mb * 1024;
        // Flash 一般 <= 32 MB —— 用 MB 表示，一位小数。
        // 已用 KB → MB（浮点），保留 1 位小数
        float used_mb  = m->flash_used_kb / 1024.0f;
        snprintf(s_buf, sizeof(s_buf), "%.1f/%uMB", used_mb, (unsigned)total_mb);
        (void)total_kb;
    }
    lv_label_set_text(lbl_flash, s_buf);
    lv_label_set_text(lbl_firmware, m->app_ver);
}
