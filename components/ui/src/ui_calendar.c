// ui_calendar —— 月历页面
//
// 布局：
//   - 顶部状态栏（WiFi + 电池）
//   - 标题行：YYYY年MM月（居中）
//   - 6×7 月历网格（周日～周六，周日首列）
//   - 底部横线 + 页码点
//
// 高亮规则：
//   - 今日：实心黑圆
//   - 法定节假日：空心方框（红色边框，单色屏下为加粗黑框）
//   - 控制台标注日期：实心方块（与今日区分）

#include "ui_calendar.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_font.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCR_W   400
#define SCR_H   300

// 状态栏
#define STATUS_Y        6
#define WIFI_ICON_X     14
#define BATT_ICON_X     342

// 标题
#define TITLE_Y         26

// 月历网格（6行×7列，每格宽50高28，居中）
#define CAL_X0          25
#define CAL_Y0          52
#define CELL_W          50
#define CELL_H          28

// 底部横线 + 页码点（与 weather / device 对齐）
#define BOTTOM_LINE_Y   278
#define DOT_Y           291
#define DOT_R           3
#define DOT_SPACING     14

// 容量上限统一在 ui_calendar.h 定义（配网门户校验时共用同一份值）
#define MAX_MARK_DATES  UI_CAL_MAX_MARKS
#define MAX_EVENTS      UI_CAL_MAX_EVENTS
#define MAX_LABELS      UI_CAL_MAX_LABELS
#define EVENT_TEXT_MAX  UI_CAL_TEXT_MAX

// -------- 页面私有状态 -------------------------------------------
static lv_obj_t *s_screen = NULL;
static lv_obj_t *lbl_title;
static ui_status_bar_t *s_bar;        // 状态栏（动态刷新）
static lv_obj_t *lbl_wday[7];         // 周日～周六表头
static lv_obj_t *lbl_day[6][7];       // 6行×7列日期
static char s_title[32];
static lv_obj_t *lbl_footer;          // 底部标签（预定 / 随机预设文字）

// 控制台标注日期列表（格式 MMDD 整数，如 1001 = 10月1日）
static uint16_t s_mark_dates[MAX_MARK_DATES];
static int s_mark_count = 0;

// 预定内容：某天固定显示的自定义文字（date=MMDD → text）
static uint16_t s_event_dates[MAX_EVENTS];
static char     s_event_texts[MAX_EVENTS][EVENT_TEXT_MAX];
static int      s_event_count = 0;

// 随机预设标签池：无当天预定时，按日期做种子固定选一条
static char     s_labels[MAX_LABELS][EVENT_TEXT_MAX];
static int      s_label_count = 0;

// 内置节假日表（简化，格式 MMDD，不含年份）
static const uint16_t HOLIDAYS[] = {
    101, 102, 103,      // 元旦
    210, 211, 212, 213, 214, 215, 216, 217,  // 春节（示例）
    404, 405, 406,      // 清明
    501, 502, 503, 504, 505,   // 五一
    610, 611, 612,      // 端午
    1001, 1002, 1003, 1004, 1005, 1006, 1007,  // 国庆
    0
};

static int is_holiday(int month, int day)
{
    uint16_t md = (uint16_t)(month * 100 + day);
    for (int i = 0; HOLIDAYS[i]; i++) {
        if (HOLIDAYS[i] == md) return 1;
    }
    return 0;
}

static int is_marked(int month, int day)
{
    uint16_t md = (uint16_t)(month * 100 + day);
    for (int i = 0; i < s_mark_count; i++) {
        if (s_mark_dates[i] == md) return 1;
    }
    return 0;
}

// 控制台命令：标注日期
void ui_calendar_mark_date(const char *mmdd)
{
    if (!mmdd || !mmdd[0]) {
        // NULL 清空所有标注
        s_mark_count = 0;
        return;
    }
    // 解析 MM-DD
    int m = 0, d = 0;
    if (sscanf(mmdd, "%d-%d", &m, &d) != 2) return;
    if (m < 1 || m > 12 || d < 1 || d > 31) return;

    uint16_t md = (uint16_t)(m * 100 + d);
    // 去重
    for (int i = 0; i < s_mark_count; i++) {
        if (s_mark_dates[i] == md) return;
    }
    if (s_mark_count < MAX_MARK_DATES) {
        s_mark_dates[s_mark_count++] = md;
    }
}

// 后台批量设置标注日期（覆盖现有列表）。格式 "MM-DD,MM-DD,..."。
void ui_calendar_set_marks(const char *csv)
{
    s_mark_count = 0;
    if (!csv || !csv[0]) return;
    const char *p = csv;
    while (*p && s_mark_count < MAX_MARK_DATES) {
        int m = 0, d = 0;
        if (sscanf(p, "%d-%d", &m, &d) == 2 &&
            m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            uint16_t md = (uint16_t)(m * 100 + d);
            // 去重
            int dup = 0;
            for (int i = 0; i < s_mark_count; i++) {
                if (s_mark_dates[i] == md) { dup = 1; break; }
            }
            if (!dup) s_mark_dates[s_mark_count++] = md;
        }
        // 跳到下一个逗号之后
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
}

// 从 [start, end) 复制文字到 dst（去首尾空格，截断到 max-1 字节，末尾补 0）
static void copy_trimmed(char *dst, size_t max, const char *start, const char *end)
{
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r' || end[-1] == '\n')) end--;
    size_t n = (size_t)(end - start);
    if (n >= max) n = max - 1;
    memcpy(dst, start, n);
    dst[n] = 0;
}

// 后台设置预定内容。格式 "MM-DD=内容;MM-DD=内容;..."。
void ui_calendar_set_events(const char *spec)
{
    s_event_count = 0;
    if (!spec || !spec[0]) return;
    const char *p = spec;
    while (*p && s_event_count < MAX_EVENTS) {
        // 一段 = 到 ';' 或串尾
        const char *seg_end = strchr(p, ';');
        const char *stop = seg_end ? seg_end : (p + strlen(p));
        const char *eq = memchr(p, '=', (size_t)(stop - p));
        if (eq) {
            int m = 0, d = 0;
            if (sscanf(p, "%d-%d", &m, &d) == 2 &&
                m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                s_event_dates[s_event_count] = (uint16_t)(m * 100 + d);
                copy_trimmed(s_event_texts[s_event_count],
                             EVENT_TEXT_MAX, eq + 1, stop);
                if (s_event_texts[s_event_count][0]) s_event_count++;
            }
        }
        if (!seg_end) break;
        p = seg_end + 1;
    }
}

// 后台设置随机预设标签池。格式 "文字1;文字2;文字3"。
void ui_calendar_set_labels(const char *spec)
{
    s_label_count = 0;
    if (!spec || !spec[0]) return;
    const char *p = spec;
    while (*p && s_label_count < MAX_LABELS) {
        const char *seg_end = strchr(p, ';');
        const char *stop = seg_end ? seg_end : (p + strlen(p));
        copy_trimmed(s_labels[s_label_count], EVENT_TEXT_MAX, p, stop);
        if (s_labels[s_label_count][0]) s_label_count++;
        if (!seg_end) break;
        p = seg_end + 1;
    }
}

// 查找某天的预定内容，返回文字指针；无则 NULL。
static const char *event_for(int month, int day)
{
    uint16_t md = (uint16_t)(month * 100 + day);
    for (int i = 0; i < s_event_count; i++) {
        if (s_event_dates[i] == md) return s_event_texts[i];
    }
    return NULL;
}

// 计算当月第一天是星期几（0=周日，1=周一...）—— Zeller's congruence 变体
static int first_day_of_month(int year, int month)
{
    int y = year, m = month;
    if (m < 3) { m += 12; y--; }
    int K = y % 100;
    int J = y / 100;
    // Zeller: h = 0=Saturday, 1=Sunday, 2=Monday, ... 6=Friday
    int h = (1 + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    // 转换为 0=周日
    int wday = (h + 6) % 7;
    return wday;
}

// 计算当月天数
static int days_in_month(int year, int month)
{
    static const int days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int d = days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        d = 29;
    }
    return d;
}

// -------- 页面构建 -------------------------------------------------
lv_obj_t *ui_calendar_create(void)
{
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    const ui_model_t *m = ui_model_get();
    // 标题（居中）—— 在 scaffold 之前先把 title label 建好，这样 scaffold
    // 不会覆盖它。或者更简单：先 scaffold，然后覆盖标题。
    lv_obj_t *title = ui_make_label(s_screen, ui_font_cjk_16(), 0, 26, "----年--月");
    lv_obj_set_width(title, 400);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    s_bar = ui_status_bar_create(s_screen, m->wifi_rssi, m->wifi_connected,
                                  m->battery_percent, m->battery_charging);
    // 底部横线 + 导航点（scaffold 不做是因为标题要复用）
    ui_pixel_rect(s_screen, 9, 278, 378, 2);
    ui_draw_page_dots(s_screen, 2, 291, 3, 14);
    lbl_title = title;

    // 周日～周六表头
    const char *WDAY_NAMES[] = { "日", "一", "二", "三", "四", "五", "六" };
    for (int i = 0; i < 7; i++) {
        lbl_wday[i] = ui_make_label(s_screen, ui_font_cjk_16(),
                                     CAL_X0 + i * CELL_W, CAL_Y0,
                                     (char *)WDAY_NAMES[i]);
        lv_obj_set_width(lbl_wday[i], CELL_W);
        lv_obj_set_style_text_align(lbl_wday[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // 6×7 日期格子
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 7; c++) {
            int x = CAL_X0 + c * CELL_W;
            int y = CAL_Y0 + 26 + r * CELL_H;
            lbl_day[r][c] = ui_make_label(s_screen, ui_font_cjk_16(),
                                           x, y + 4, "");
            lv_obj_set_size(lbl_day[r][c], CELL_W, 20);
            lv_obj_set_style_text_align(lbl_day[r][c], LV_TEXT_ALIGN_CENTER, 0);
            // 内边距，让高亮背景看起来像圆点/方框而不是撑满一整格
            lv_obj_set_style_pad_hor(lbl_day[r][c], 0, 0);
            lv_obj_set_style_pad_ver(lbl_day[r][c], 2, 0);
        }
    }

    // 底部标签（预定 / 随机预设文字）—— 居中，横线上方
    lbl_footer = ui_make_label(s_screen, ui_font_cjk_16(), 0, 256, "");
    lv_obj_set_width(lbl_footer, 400);
    lv_obj_set_style_text_align(lbl_footer, LV_TEXT_ALIGN_CENTER, 0);

    ui_calendar_apply_locked();
    return s_screen;
}

// -------- 数据 → UI 同步 -------------------------------------------
void ui_calendar_apply_locked(void)
{
    if (!s_screen) return;
    const ui_model_t *m = ui_model_get();

    // 状态栏动态刷新
    ui_status_bar_update(s_bar, m->wifi_rssi, m->wifi_connected,
                          m->battery_percent, m->battery_charging);

    int year = m->year;
    int month = m->month;
    int today = m->day;

    // 空数据保护：模拟器/设备启动初期 year 可能是 0
    if (year < 1970 || year > 2200 || month < 1 || month > 12) return;

    // 更新标题
    snprintf(s_title, sizeof(s_title), "%d年%d月", year, month);
    lv_label_set_text(lbl_title, s_title);

    // 清空所有日期 label
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 7; c++) {
            lv_label_set_text(lbl_day[r][c], "");
        }
    }

    int first_wday = first_day_of_month(year, month);
    int total_days = days_in_month(year, month);

    // 填充日期
    int day = 1;
    for (int r = 0; r < 6 && day <= total_days; r++) {
        int start_col = (r == 0) ? first_wday : 0;
        for (int c = start_col; c < 7 && day <= total_days; c++) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", day);
            lv_label_set_text(lbl_day[r][c], buf);

            // 清除之前的样式（重要：文字颜色也要复位到黑色，否则曾被
            // 高亮成"今日/标注"的格子在 today 变化后会残留白色字，白底白字看不见）
            lv_obj_set_style_bg_opa(lbl_day[r][c], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(lbl_day[r][c], 0, 0);
            lv_obj_set_style_text_color(lbl_day[r][c], lv_color_black(), 0);
            lv_obj_set_style_radius(lbl_day[r][c], 0, 0);

            // 检查是否需要高亮
            int is_today = (day == today);
            int holiday = is_holiday(month, day);
            int marked = is_marked(month, day);

            if (is_today) {
                // 今日：实心黑圆背景，白字
                lv_obj_set_style_bg_color(lbl_day[r][c], lv_color_black(), 0);
                lv_obj_set_style_bg_opa(lbl_day[r][c], LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(lbl_day[r][c], lv_color_white(), 0);
                lv_obj_set_style_radius(lbl_day[r][c], 8, 0);
            } else if (marked) {
                // 标注日期：实心方块背景，白字
                lv_obj_set_style_bg_color(lbl_day[r][c], lv_color_black(), 0);
                lv_obj_set_style_bg_opa(lbl_day[r][c], LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(lbl_day[r][c], lv_color_white(), 0);
                lv_obj_set_style_radius(lbl_day[r][c], 2, 0);
            } else if (holiday) {
                // 节假日：空心方框
                lv_obj_set_style_border_width(lbl_day[r][c], 2, 0);
                lv_obj_set_style_border_color(lbl_day[r][c], lv_color_black(), 0);
                lv_obj_set_style_border_opa(lbl_day[r][c], LV_OPA_COVER, 0);
                lv_obj_set_style_radius(lbl_day[r][c], 4, 0);
            }
            day++;
        }
    }

    // ---- 底部标签：预定优先，否则随机 ----
    const char *footer_text = event_for(month, today);
    if (!footer_text && s_label_count > 0) {
        // 按日期做种子固定选一条（同一天不变，跨天才换）
        int idx = (year * 10000 + month * 100 + today) % s_label_count;
        footer_text = s_labels[idx];
    }
    lv_label_set_text(lbl_footer, footer_text ? footer_text : "");
}
