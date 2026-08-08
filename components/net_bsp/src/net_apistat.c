// net_apistat.c —— 外部接口调用次数统计（记 SD，按月分文件）
//
// 目的：看清"这台设备一天/一周/一月/一年往外打了多少次 API"，配额还剩多少。
// 只关心**次数**，不画趋势曲线，所以聚合结果就是「接口名 → 次数」。
//
// ---- 存储布局 --------------------------------------------------------
//   /sdcard/rlcd/api/YYYY-MM.csv
//   timestamp,api,ok
//   2026-08-08 16:42:03,qweather_now,1
//
// 为什么**按月分文件**而不是像 weather_log.csv 那样一份到底：
//   * 查询窗口最长是"本年"，最短是"今日"。按月切，今日/本周/本月只需读 1 个
//     文件（本周跨月最多 2 个），本年最多 12 个 —— 每次查询的读取量都有上界，
//     而单文件方案里"查今天"也得扫过全年的数据。
//   * 一行约 40 字节。最密的节奏是天气 10 分钟一轮 × 2 个端点 ≈ 300 行/天，
//     单月约 9000 行 / 350KB，块扫聚合毫无压力；单文件到年底就是 4MB。
//   * 删除旧数据 = 删掉整个文件，不必重写。
//
// 时间戳存**完整本地时间**（不只存计数），这样任何窗口都能事后重算，也能直接
// 拿文件去核对"某次限流发生在几点"。聚合在查询时做，不维护任何持久化的计数器
// —— 计数器一旦和明细不一致就没法对账，而且掉电会丢增量。
//
// ---- 并发 -------------------------------------------------------------
// 写入方是 weather_task（QWeather / UAPI），读取方是 httpd 任务，两条不同的
// 任务都会碰同一批文件。FATFS 自身对不同 FILE* 是安全的，但"追加"和"块扫"
// 交错时读侧可能读到半行 —— 所以上一把 mutex，临界区只包住文件 IO。
// 写一行是 fopen/fprintf/fclose，几毫秒量级，不会拖住 weather_task。

#include "net_internal.h"
#include "ui_model.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>

// 路径写字面量而不是 include sdcard_bsp.h 的 RLCD_DATA_DIR —— port_bsp 已经
// PRIV_REQUIRES net_bsp（长按刷天气用），反向依赖它会成环。net_calendar.c
// 出于同样的原因也是硬编码 "/sdcard/rlcd"。
#define API_DATA_DIR    "/sdcard/rlcd"
#define API_DIR         API_DATA_DIR "/api"
#define API_HEADER      "timestamp,api,ok\n"
#define API_SCAN_SZ     1024    // 聚合时的读块大小（与 csv_get 的倒读同一量级）
#define API_LINE_MAX    128     // 正常行不到 50B；残行超过这个就当文件被写坏了

// 接口登记表 —— 加一个外部接口就在这里加一行，其余全自动（记录/聚合/门户展示）。
// key 进 CSV（ASCII，稳定不变，改中文名不会让历史数据对不上），name 给门户显示。
static const struct { const char *key; const char *name; } API_TABLE[] = {
    [API_CALL_QWEATHER_GEO]   = { "qweather_geo",   "和风天气 · 城市解析" },
    [API_CALL_QWEATHER_NOW]   = { "qweather_now",   "和风天气 · 实况"     },
    [API_CALL_QWEATHER_DAILY] = { "qweather_daily", "和风天气 · 每日预报" },
    [API_CALL_UAPI_MYIP]      = { "uapi_myip",      "UAPI · 公网 IP 定位" },
};

static SemaphoreHandle_t s_mux = NULL;

// 懒初始化 mutex。第一次调用发生在 weather_task 或 httpd handler 里，都在
// NetBsp_Start() 之后，没有并发创建的窗口。
static bool api_lock(void)
{
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutex();
        if (!s_mux) return false;
    }
    // 等 2s：写侧只做几毫秒 IO，读侧最长是"本年"（12 个文件块扫）。
    // 拿不到锁就放弃本次记录 —— 统计数据丢一条远好过阻塞 weather_task。
    return xSemaphoreTake(s_mux, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void api_unlock(void) { xSemaphoreGive(s_mux); }

static void api_month_path(char *out, size_t n, int year, int month)
{
    snprintf(out, n, API_DIR "/%04d-%02d.csv", year, month);
}

// =====================================================================
// 记录一次调用
// =====================================================================
void NetBsp_ApiCallRecord(api_call_id_t id, bool ok)
{
    if (id < 0 || id >= API_CALL_COUNT) return;
    if (!ui_model_get()->sd_mounted) return;   // 无卡静默丢弃（和 csv_log 一致）

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    // SNTP 未校时前系统时间是 1970 —— 那样的行归不到任何真实月份，反而会造出
    // 一个 1970-01.csv 干扰"本年"聚合。直接丢掉，联网校时后自然开始记。
    if (lt.tm_year + 1900 < 2020) return;

    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    char path[64];
    api_month_path(path, sizeof(path), lt.tm_year + 1900, lt.tm_mon + 1);

    if (!api_lock()) {
        ESP_LOGW(NET_TAG, "apistat: 取锁超时，丢弃一条 %s", API_TABLE[id].key);
        return;
    }

    mkdir(API_DATA_DIR, 0777);
    mkdir(API_DIR, 0777);          // 已存在无害

    struct stat st;
    bool need_header = (stat(path, &st) != 0);
    FILE *f = fopen(path, "a");
    if (!f) {
        api_unlock();
        ESP_LOGW(NET_TAG, "apistat: 写 %s 失败（SD 满/只读？）", path);
        return;
    }
    if (need_header) fwrite(API_HEADER, 1, strlen(API_HEADER), f);
    fprintf(f, "%s,%s,%d\n", ts, API_TABLE[id].key, ok ? 1 : 0);
    // 不 fsync：统计数据不是账本，掉电丢最后几行可以接受，而每次调用都 fsync
    // 会让 SD 多一次擦写（天气 10 分钟一轮，一年约 10 万次）。fclose 已经把
    // 数据交给 FATFS 缓冲，下一次写入或卸载时落盘。
    fclose(f);
    api_unlock();
}

// =====================================================================
// 聚合
// =====================================================================
// 判断一行的时间戳是否落在窗口里。行首固定 "YYYY-MM-DD HH:MM:SS"，定长 19 字节，
// 直接按位取数比 strptime 快得多也不吃栈。
//
// 返回的是打包成 YYYYMMDD 的**日期**整数（0 = 这行不是数据行，表头就这样被挡掉）。
// 为什么用日期整数而不是 time_t：所有窗口起点都是某天的 00:00:00，所以"这行在不在
// 窗口内"完全等价于比较日期，不需要时分秒。省掉的是每行一次 mktime —— 查"本年"
// 有约 10 万行，而 httpd 是单任务串行的，那些 mktime 会实打实地把门户卡住。
static int line_date_key(const char *line, size_t len)
{
    if (len < 19) return 0;
    for (int i = 0; i < 19; i++) {
        bool digit = (i != 4 && i != 7 && i != 10 && i != 13 && i != 16);
        if (digit && (line[i] < '0' || line[i] > '9')) return 0;
    }
    int y = (line[0]-'0')*1000 + (line[1]-'0')*100 + (line[2]-'0')*10 + (line[3]-'0');
    int mo = (line[5]-'0')*10 + (line[6]-'0');
    int d  = (line[8]-'0')*10 + (line[9]-'0');
    return y * 10000 + mo * 100 + d;
}

// 把一行 "ts,key,ok" 计到 out 里。认不出 key 的行忽略（历史遗留 / 手工编辑）。
static void tally_line(const char *line, api_stat_t *out)
{
    const char *k = line + 19;
    if (*k != ',') return;
    k++;
    const char *e = strchr(k, ',');
    if (!e) return;
    size_t klen = (size_t)(e - k);
    // ok 字段：'0' 才算失败，其余（含缺失）都按成功算
    bool ok = (e[1] != '0');
    for (int i = 0; i < API_CALL_COUNT; i++) {
        if (strlen(API_TABLE[i].key) != klen) continue;
        if (memcmp(API_TABLE[i].key, k, klen) != 0) continue;
        out->count[i]++;
        if (!ok) out->fail[i]++;
        return;
    }
}

// 扫一个月文件，把日期 >= from_key（YYYYMMDD）的行计入 out。文件不存在直接返回。
//
// 顺序读 + 手工切行，不用 fgets：fgets 每次调用都是一趟 FATFS 往返，
// 9000 行的月文件要 9000 次；按 1KB 块读只要 350 次。
static void tally_file(const char *path, int from_key, api_stat_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;

    static char buf[API_SCAN_SZ + API_LINE_MAX];   // 尾部留出跨块残行的空间
    size_t held = 0;                               // 上一块末尾未处理完的残行长度
    for (;;) {
        size_t got = fread(buf + held, 1, API_SCAN_SZ, f);
        if (got == 0) break;
        size_t total = held + got;
        size_t start = 0;
        for (size_t i = 0; i < total; i++) {
            if (buf[i] != '\n') continue;
            size_t len = i - start;
            if (len && buf[start + len - 1] == '\r') len--;
            buf[start + len] = 0;         // tally_line 里要 strchr
            int key = line_date_key(buf + start, len);
            if (key && key >= from_key) tally_line(buf + start, out);
            start = i + 1;
        }
        held = total - start;
        // 残行超过缓冲余量 = 这个文件的行被破坏了（正常行不到 50B），
        // 丢掉残段继续，不让它把后面的块挤出缓冲。
        if (held > API_LINE_MAX) held = 0;
        else if (held) memmove(buf, buf + start, held);
    }
    fclose(f);
}

// 窗口起点：按**自然**日/周/月/年取，不是"往前 N 天"。
// API 配额都是按自然周期重置的，"本月还剩多少"才是真正想问的问题；
// 滚动窗口（近 30 天）反而对不上服务商的账单。周起点取周一。
// 返回打包成 YYYYMMDD 的日期，并回填 out 供门户显示。
static int period_start_key(api_period_t p, const struct tm *now, api_stat_t *out)
{
    struct tm t = *now;
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    switch (p) {
    case API_PERIOD_DAY:
        break;
    case API_PERIOD_WEEK: {
        // tm_wday: 0=周日。以周一为一周之始 → 周日要回退 6 天。
        int back = (t.tm_wday == 0) ? 6 : t.tm_wday - 1;
        t.tm_mday -= back;
        // mktime 归一化跨月/跨年（08-02 周日 → 07-27；2026-01-01 → 2025-12-29）
        mktime(&t);
        break;
    }
    case API_PERIOD_MONTH:
        t.tm_mday = 1;
        break;
    case API_PERIOD_YEAR:
        t.tm_mon = 0;
        t.tm_mday = 1;
        break;
    }
    out->from_year  = t.tm_year + 1900;
    out->from_month = t.tm_mon + 1;
    out->from_day   = t.tm_mday;
    return out->from_year * 10000 + out->from_month * 100 + out->from_day;
}

bool NetBsp_ApiStatQuery(api_period_t period, api_stat_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!ui_model_get()->sd_mounted) return false;

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 < 2020) return false;   // 未校时，无从判断窗口

    const int from_key = period_start_key(period, &lt, out);

    if (!api_lock()) return false;

    // 从窗口起点那个月一路扫到当前月。跨年的只有"本周"（12-29 起、1 月止），
    // year*12+mon 的线性月序号天然处理跨年，不必特判。
    int m0 = out->from_year * 12 + (out->from_month - 1);
    int m1 = (lt.tm_year + 1900) * 12 + lt.tm_mon;
    for (int m = m0; m <= m1; m++) {
        char path[64];
        api_month_path(path, sizeof(path), m / 12, m % 12 + 1);
        tally_file(path, from_key, out);
    }
    api_unlock();
    return true;
}

const char *NetBsp_ApiCallName(api_call_id_t id)
{
    if (id < 0 || id >= API_CALL_COUNT) return "";
    return API_TABLE[id].name;
}

const char *NetBsp_ApiCallKey(api_call_id_t id)
{
    if (id < 0 || id >= API_CALL_COUNT) return "";
    return API_TABLE[id].key;
}
