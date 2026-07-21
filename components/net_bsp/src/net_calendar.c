// net_calendar.c —— 日历配置存 SD 卡 /sdcard/rlcd/calendar.conf
//
// 不写 NVS/flash，改存 SD，保存后不重启。文件按行存，每行一条：
//   M <MM-DD>          标注日期
//   E <MM-DD>=<内容>   预定内容
//   L <文字>           随机标签
// 无 SD 时禁止写入（web 侧也会拒绝）。

#include "net_internal.h"
#include "ui_model.h"
#include "ui_pages.h"
#include "ui_calendar.h"
#include "lvgl_bsp.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <esp_log.h>

#define CAL_DIR        "/sdcard/rlcd"
#define CAL_FILE       "/sdcard/rlcd/calendar.conf"
#define CAL_FILE_TMP   "/sdcard/rlcd/calendar.tmp"

// 读 SD 上的日历文件，拆成三段字符串（原有 setter 格式）。
//   marks:  "MM-DD,..."     events: "MM-DD=内容;..."     labels: "文字;..."
// 三个缓冲各自会被清空后填充。SD 未挂载 / 文件不存在 → 三段均为空，返回 false。
bool cal_read_file(char *marks, size_t nm, char *events, size_t ne,
                   char *labels, size_t nl)
{
    marks[0] = events[0] = labels[0] = 0;
    FILE *f = fopen(CAL_FILE, "r");
    if (!f) {
        // conf 不存在但 tmp 在——说明上次保存正好在 remove 与 rename 之间掉电。
        // tmp 已 fsync 落盘，是完整的新数据：扶正成 conf 再读，一条都不丢。
        if (rename(CAL_FILE_TMP, CAL_FILE) == 0) {
            f = fopen(CAL_FILE, "r");
        }
        if (!f) return false;   // SD 未挂载 / 两者都不存在
    }

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        char tag = line[0];
        const char *val = line + 1;
        while (*val == ' ') val++;
        if (tag == 'M') {
            if (marks[0]) strncat(marks, ",", nm - strlen(marks) - 1);
            strncat(marks, val, nm - strlen(marks) - 1);
        } else if (tag == 'E') {
            if (events[0]) strncat(events, ";", ne - strlen(events) - 1);
            strncat(events, val, ne - strlen(events) - 1);
        } else if (tag == 'L') {
            if (labels[0]) strncat(labels, ";", nl - strlen(labels) - 1);
            strncat(labels, val, nl - strlen(labels) - 1);
        }
    }
    fclose(f);
    return true;
}

// 从 SD 加载日历配置并推入 UI。SD 未挂载 / 文件不存在 → 静默（UI 保持空）。
void cal_load_from_sd(void)
{
    static char marks[128], events[512], labels[512];
    if (!cal_read_file(marks, sizeof(marks), events, sizeof(events),
                       labels, sizeof(labels))) {
        ESP_LOGI(NET_TAG, "cal: 无 SD 或文件不存在，日历配置为空");
        return;
    }
    // setter 只写静态数组、不碰 LVGL 对象，无需持锁；apply 才需要锁。
    // 即使这里锁超时、apply 被跳过，1s tick 也会用新数组重绘。
    ui_calendar_set_marks(marks);
    ui_calendar_set_events(events);
    ui_calendar_set_labels(labels);
    if (Lvgl_lock(200)) {
        ui_pages_apply_locked();
        Lvgl_unlock();
    }
    ESP_LOGI(NET_TAG, "cal: 已加载 marks='%s' events='%s' labels='%s'", marks, events, labels);
}

// 把三段字符串写回 SD 文件。成功返回 true。SD 未挂载返回 false。
// marks: "MM-DD,..."；events: "MM-DD=内容;..."；labels: "文字;..."
//
// 先写 calendar.tmp（fflush + fsync 落盘），再 remove 旧文件 + rename 覆盖。
// FATFS 的 rename 不覆盖已存在目标，故必须先 remove。remove 与 rename 之间掉电
// 时 conf 会暂缺，但 tmp 是完整新数据——cal_read_file 会在 conf 缺失时把 tmp
// 扶正回收，因此任何时刻掉电都不丢配置。
bool cal_save_to_sd(const char *marks, const char *events, const char *labels)
{
    mkdir(CAL_DIR, 0777);   // 确保目录存在（已存在无害；SD 未挂载则后续 fopen 失败）
    FILE *f = fopen(CAL_FILE_TMP, "w");
    if (!f) {
        ESP_LOGW(NET_TAG, "cal: 写 %s 失败（SD 只读/满？）", CAL_FILE_TMP);
        return false;
    }
    // 逐条拆分写行 —— 用局部拷贝做 strtok
    char buf[512];
    if (marks && marks[0]) {
        strncpy(buf, marks, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) fprintf(f, "M %s\n", t);
    }
    if (events && events[0]) {
        strncpy(buf, events, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *t = strtok(buf, ";"); t; t = strtok(NULL, ";")) fprintf(f, "E %s\n", t);
    }
    if (labels && labels[0]) {
        strncpy(buf, labels, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *t = strtok(buf, ";"); t; t = strtok(NULL, ";")) fprintf(f, "L %s\n", t);
    }
    // 确保数据真正落到 SD，再做替换
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // FATFS 的 rename 不会覆盖已存在的目标（f_rename 返回 FR_EXIST），
    // 与 POSIX 语义不同——必须先删旧文件再 rename，否则第二次起保存必失败。
    remove(CAL_FILE);
    if (rename(CAL_FILE_TMP, CAL_FILE) != 0) {
        ESP_LOGW(NET_TAG, "cal: rename %s → %s 失败", CAL_FILE_TMP, CAL_FILE);
        remove(CAL_FILE_TMP);   // 清理临时文件
        return false;
    }
    return true;
}
