/*
 * sim_console.c —— 模拟器调试控制台后端实现
 *
 * 一个 socket server 线程接受外部连接，把收到的文本行塞进线程安全队列；
 * 主循环调 sim_console_drain() 在主线程逐条执行，避免 LVGL 多线程崩溃。
 */
#include "sim_console.h"

#include "ui_model.h"
#include "ui_home.h"
#include "ui_pages.h"
#include "ui_calendar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>     // offsetof —— 字段表用
#include <errno.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define closesocket close
#define SOCK_ERR WSAGetLastError()
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#define SOCK_ERR errno
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket    close
typedef int SOCKET;
#endif

#define DEFAULT_PORT    9000
#define QUEUE_CAP       64          // 命令队列容量
#define LINE_BUF        256

// ---- 线程安全命令队列 -------------------------------------------
// 每条命令附带一个 response_fd：!= INVALID_SOCKET 时 drain() 会把响应发回
// 该 socket；== INVALID_SOCKET 表示来自 stdin（无需回响应）。
typedef struct {
    char   *items[QUEUE_CAP];
    SOCKET  fds[QUEUE_CAP];      // 与 items 一一对应的响应 fd
    int  head;      // 写入位（生产者：socket 线程 / stdin 线程）
    int  tail;      // 读取位（消费者：主线程 drain）
    int  count;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
} cmd_queue_t;

static void queue_init(cmd_queue_t *q)
{
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void queue_push(cmd_queue_t *q, const char *cmd, SOCKET response_fd)
{
    pthread_mutex_lock(&q->mu);
    if (q->count < QUEUE_CAP) {
        q->items[q->head] = strdup(cmd);
        q->fds[q->head]   = response_fd;
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    // 满了就丢：调试场景不应当爆发式投命令
    pthread_mutex_unlock(&q->mu);
}

// 非阻塞弹一条。返回 strdup 的字符串（调用方 free），空队列返回 NULL。
// 同时通过 *out_fd 输出对应的响应 fd。
static char *queue_pop_nb(cmd_queue_t *q, SOCKET *out_fd)
{
    char *c = NULL;
    pthread_mutex_lock(&q->mu);
    if (q->count > 0) {
        c = q->items[q->tail];
        *out_fd = q->fds[q->tail];
        q->items[q->tail] = NULL;
        q->tail = (q->tail + 1) % QUEUE_CAP;
        q->count--;
    } else {
        *out_fd = INVALID_SOCKET;
    }
    pthread_mutex_unlock(&q->mu);
    return c;
}

static cmd_queue_t s_queue;

// ---- override 位图 ----------------------------------------------
// 每位对应 ui_model_t 中"可被外部设置、sim_tick_data 不应覆盖"的字段。
// 位定义在 sim_console.h 的 OVR_* 宏里。
static uint64_t s_override = 0;

// 提供给 main.c：sim_tick_data 查询哪些字段被外部设置
uint64_t sim_console_override_mask(void) { return s_override; }

// ---- 字段表（唯一真源）------------------------------------------
// set / get / dump / sim_console_set_field_override 全部走这张表。
// 加字段只需在这里加一行 + 在 sim_console.h 加一个 OVR_* 位。
typedef enum {
    FT_F32,     // float
    FT_I32,     // int
    FT_I8,      // int8_t
    FT_U8,      // uint8_t
    FT_U32,     // uint32_t
    FT_BOOL,    // bool
    FT_STR,     // char[]，size 为数组容量
} field_type_t;

typedef struct {
    const char  *name;
    field_type_t type;
    size_t       offset;    // offsetof(ui_model_t, xxx)
    size_t       size;      // 仅 FT_STR 用：数组容量（含结尾 0）
    uint64_t     ovr_bit;   // 0 表示该字段不参与 override
} field_desc_t;

#define FLD(n, t, member, bit) \
    { n, t, offsetof(ui_model_t, member), sizeof(((ui_model_t *)0)->member), bit }

static const field_desc_t FIELDS[] = {
    // ---- 时间（6 个字段共用 OVR_TIME 一位）----
    FLD("hour",             FT_I32,  hour,             OVR_TIME),
    FLD("minute",           FT_I32,  minute,           OVR_TIME),
    FLD("year",             FT_I32,  year,             OVR_TIME),
    FLD("month",            FT_I32,  month,            OVR_TIME),
    FLD("day",              FT_I32,  day,              OVR_TIME),
    FLD("weekday",          FT_I32,  weekday,          OVR_TIME),

    // ---- 室内传感器 ----
    FLD("indoor_temp",      FT_F32,  indoor_temp,      OVR_INDOOR_TEMP),
    FLD("indoor_humi",      FT_F32,  indoor_humi,      OVR_INDOOR_HUMI),

    // ---- 室外天气 ----
    FLD("outdoor_temp",     FT_F32,  outdoor_temp,     OVR_OUTDOOR_TEMP),
    FLD("weather_code",     FT_I32,  weather_code,     OVR_WEATHER_CODE),
    FLD("weather_text",     FT_STR,  weather_text,     OVR_WEATHER_TEXT),
    FLD("city",             FT_STR,  city,             OVR_CITY),
    FLD("weather_update",   FT_STR,  weather_update,   OVR_WEATHER_UPD),

    // ---- 天气详情页 ----
    FLD("outdoor_humi",     FT_F32,  outdoor_humi,     OVR_OUTDOOR_HUMI),
    FLD("wind_speed",       FT_F32,  wind_speed_kmh,   OVR_WIND_SPD),
    FLD("wind_dir",         FT_STR,  wind_dir,         OVR_WIND_DIR),
    FLD("cloud",            FT_I32,  cloud_pct,        OVR_CLOUD),
    FLD("pressure",         FT_I32,  pressure_hpa,     OVR_PRESSURE),
    FLD("visibility",       FT_I32,  visibility_km,    OVR_VISIBILITY),
    FLD("feels_like_temp",  FT_F32,  feels_like_temp,  OVR_FEELS_LIKE),
    FLD("uv",               FT_I32,  uv_index,         OVR_UV),
    FLD("temp_min",         FT_I32,  temp_min,         OVR_TEMP_MIN),
    FLD("temp_max",         FT_I32,  temp_max,         OVR_TEMP_MAX),
    FLD("sunrise",          FT_STR,  sunrise,          OVR_SUNRISE),
    FLD("sunset",           FT_STR,  sunset,           OVR_SUNSET),

    // ---- 状态栏 ----
    FLD("wifi_connected",   FT_BOOL, wifi_connected,   OVR_WIFI_CONN),
    FLD("wifi_rssi",        FT_I8,   wifi_rssi,        OVR_WIFI_RSSI),
    FLD("battery_percent",  FT_U8,   battery_percent,  OVR_BAT_PCT),
    FLD("battery_charging", FT_BOOL, battery_charging, OVR_BAT_CHG),

    // ---- 设备信息页 ----
    FLD("ip",               FT_STR,  ip,               OVR_IP),
    FLD("mac",              FT_STR,  mac,              OVR_MAC),
    FLD("ssid",             FT_STR,  ssid,             OVR_SSID),
    FLD("free_heap_kb",     FT_U32,  free_heap_kb,     OVR_FREE_HEAP),
    FLD("flash_size_mb",    FT_U32,  flash_size_mb,    OVR_FLASH_SIZE),
    FLD("uptime_sec",       FT_U32,  uptime_sec,       OVR_UPTIME),
    FLD("chip_model",       FT_STR,  chip_model,       OVR_CHIP_MODEL),
    FLD("cpu_cores",        FT_U8,   cpu_cores,        OVR_CPU_CORES),
    FLD("idf_ver",          FT_STR,  idf_ver,          OVR_IDF_VER),
    FLD("app_ver",          FT_STR,  app_ver,          OVR_APP_VER),

    // ---- 配网页 ----
    FLD("ap_ssid",          FT_STR,  ap_ssid,          OVR_AP_SSID),
    FLD("ap_ip",            FT_STR,  ap_ip,            OVR_AP_IP),
    FLD("ap_active",        FT_BOOL, ap_active,        OVR_AP_ACTIVE),
    FLD("setup_dismissed",  FT_BOOL, setup_dismissed,  OVR_SETUP_DISMISS),

    // ---- SD 卡 / Flash 用量 ----
    FLD("sd_mounted",       FT_BOOL, sd_mounted,       OVR_SD_MOUNTED),
    FLD("sd_total_mb",      FT_U32,  sd_total_mb,      OVR_SD_TOTAL),
    FLD("sd_used_mb",       FT_U32,  sd_used_mb,       OVR_SD_USED),
    FLD("flash_used_kb",    FT_U32,  flash_used_kb,    OVR_FLASH_USED),
    FLD("flash_free_kb",    FT_U32,  flash_free_kb,    OVR_FLASH_FREE),
};

#undef FLD

#define FIELD_COUNT (sizeof(FIELDS) / sizeof(FIELDS[0]))

static const field_desc_t *field_find(const char *name)
{
    for (size_t i = 0; i < FIELD_COUNT; i++)
        if (strcmp(FIELDS[i].name, name) == 0) return &FIELDS[i];
    return NULL;
}

// 把 ui_model 里该字段的地址算出来
static void *field_ptr(const field_desc_t *d)
{
    return (char *)ui_model_get() + d->offset;
}

// 布尔字面量：1 / true / on / yes 均为真
static bool parse_bool(const char *v)
{
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
           strcmp(v, "on") == 0 || strcmp(v, "yes") == 0;
}

// 写字段。val 为原始字符串（FT_STR 时可含空格）。
static void field_write(const field_desc_t *d, const char *val)
{
    void *p = field_ptr(d);
    switch (d->type) {
    case FT_F32:  *(float *)p    = (float)atof(val);      break;
    case FT_I32:  *(int *)p      = atoi(val);             break;
    case FT_I8:   *(int8_t *)p   = (int8_t)atoi(val);     break;
    case FT_U8:   *(uint8_t *)p  = (uint8_t)atoi(val);    break;
    case FT_U32:  *(uint32_t *)p = (uint32_t)strtoul(val, NULL, 10); break;
    case FT_BOOL: *(bool *)p     = parse_bool(val);       break;
    case FT_STR:
        // 截断到数组容量，且保证结尾 0。注意 ui_model 的中文字段
        // （wind_dir 等）按字节截断可能断在汉字中间 —— 容量已按最长
        // 中文值给足（见 ui_model.h 注释），正常输入不会触发。
        snprintf((char *)p, d->size, "%s", val);
        break;
    }
}

// 读字段到 out（人可读文本，与 set 接受的格式一致 → 可回灌）。
static void field_read(const field_desc_t *d, char *out, size_t outlen)
{
    const void *p = field_ptr(d);
    switch (d->type) {
    case FT_F32:  snprintf(out, outlen, "%g", (double)*(const float *)p); break;
    case FT_I32:  snprintf(out, outlen, "%d", *(const int *)p);           break;
    case FT_I8:   snprintf(out, outlen, "%d", (int)*(const int8_t *)p);   break;
    case FT_U8:   snprintf(out, outlen, "%u", (unsigned)*(const uint8_t *)p); break;
    case FT_U32:  snprintf(out, outlen, "%u", (unsigned)*(const uint32_t *)p); break;
    case FT_BOOL: snprintf(out, outlen, "%s", *(const bool *)p ? "true" : "false"); break;
    case FT_STR:  snprintf(out, outlen, "%s", (const char *)p);           break;
    }
}

// ---- 响应输出辅助 ------------------------------------------------
static void send_line(SOCKET fd, const char *prefix, const char *body)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s %s\n", prefix, body);
    if (n > 0) send(fd, buf, n, 0);
}

// 帧结束标记：一个空行。客户端读到空行即知本条命令的响应已完整，
// 不必再靠 recv 超时来判断结束（dump 多行响应也能被正确切分）。
static void send_frame_end(SOCKET fd)
{
    send(fd, "\n", 1, 0);
}

// ---- 命令执行（主线程调用） --------------------------------------
// 返回值：0 正常，-1 退出命令
static int exec_one(const char *line, SOCKET client)
{
    // client == INVALID_SOCKET 表示来自 stdin（无网络客户端回响应）
    #define REPLY(p, b) do { if (client != INVALID_SOCKET) send_line(client, p, b); } while (0)

    char buf[LINE_BUF];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    buf[strcspn(buf, "\r\n")] = 0;

    if (buf[0] == 0) return 0;

    if (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0) {
        REPLY("ok", "bye");
        return -1;
    }
    if (strcmp(buf, "ping") == 0) {
        REPLY("ok", "pong");
        return 0;
    }
    if (strcmp(buf, "clear") == 0) {
        s_override = 0;
        REPLY("ok", "all overrides cleared");
        return 0;
    }
    if (strcmp(buf, "next") == 0) {
        ui_pages_next();
        REPLY("ok", "page next");
        return 0;
    }
    if (strcmp(buf, "prev") == 0) {
        ui_pages_prev();
        REPLY("ok", "page prev");
        return 0;
    }
    if (strcmp(buf, "help") == 0) {
        REPLY("ok", "cmds: set <field> <val> | get <field> | dump | ovr | page N | next | prev | mark MM-DD | unmark | marks CSV | events SPEC | labels SPEC | clear | ping | quit");
        // 字段列表从 FIELDS[] 现算，不再手抄（旧版硬编码列表早已和实际字段脱节）
        char list[1024] = "fields:";
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            size_t used = strlen(list);
            snprintf(list + used, sizeof(list) - used, " %s", FIELDS[i].name);
        }
        REPLY("ok", list);
        return 0;
    }

    // ---- dump —— 一次性回全部字段，省掉 N 次串行 get ----
    if (strcmp(buf, "dump") == 0) {
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            char val[256], msg[320];
            field_read(&FIELDS[i], val, sizeof(val));
            snprintf(msg, sizeof(msg), "%s %s", FIELDS[i].name, val);
            REPLY("val", msg);
        }
        return 0;
    }

    // ---- ovr —— 回当前被 override 的字段名，客户端用来标记 ----
    if (strcmp(buf, "ovr") == 0) {
        char list[1024] = "";
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            if (!FIELDS[i].ovr_bit || !(s_override & FIELDS[i].ovr_bit)) continue;
            size_t used = strlen(list);
            snprintf(list + used, sizeof(list) - used, "%s%s",
                     used ? " " : "", FIELDS[i].name);
        }
        REPLY("ok", list[0] ? list : "(none)");
        return 0;
    }

    // ---- page N ----
    if (strncmp(buf, "page ", 5) == 0) {
        int p = atoi(buf + 5);
        if (p >= 0 && p < (int)UI_PAGE_COUNT) {
            ui_pages_switch_to((ui_page_id_t)p);
            char msg[32]; snprintf(msg, sizeof(msg), "switched to page %d", p);
            REPLY("ok", msg);
        } else {
            REPLY("error", "page out of range");
        }
        return 0;
    }

    // ---- mark / unmark / marks / events / labels ----
    if (strncmp(buf, "mark ", 5) == 0) {
        ui_calendar_mark_date(buf + 5);
        REPLY("ok", "marked");
        return 0;
    }
    if (strcmp(buf, "unmark") == 0 || strcmp(buf, "clearmark") == 0) {
        ui_calendar_mark_date(NULL);
        REPLY("ok", "marks cleared");
        return 0;
    }
    if (strncmp(buf, "marks ", 6) == 0) {
        ui_calendar_set_marks(buf + 6);
        REPLY("ok", "marks set");
        return 0;
    }
    if (strncmp(buf, "events ", 7) == 0) {
        ui_calendar_set_events(buf + 7);
        REPLY("ok", "events set");
        return 0;
    }
    if (strncmp(buf, "labels ", 7) == 0) {
        ui_calendar_set_labels(buf + 7);
        REPLY("ok", "labels set");
        return 0;
    }

    // ---- set <field> <value> ----
    if (strncmp(buf, "set ", 4) == 0) {
        char field[64] = {0};
        const char *rest = buf + 4;
        // 取字段名（到第一个空格为止），其余整段都是值（字符串字段可含空格）
        const char *sp = strchr(rest, ' ');
        if (!sp) { REPLY("error", "usage: set <field> <value>"); return 0; }
        size_t flen = (size_t)(sp - rest);
        if (flen >= sizeof(field)) flen = sizeof(field) - 1;
        memcpy(field, rest, flen);
        field[flen] = 0;
        const char *val = sp + 1;

        const field_desc_t *d = field_find(field);
        if (!d) { REPLY("error", "unknown field"); return 0; }

        field_write(d, val);
        if (d->ovr_bit) s_override |= d->ovr_bit;
        ui_pages_apply_locked();

        char msg[128];
        snprintf(msg, sizeof(msg), "%s = %s", field, val);
        REPLY("ok", msg);
        return 0;
    }

    // ---- get <field> ----
    if (strncmp(buf, "get ", 4) == 0) {
        char field[64] = {0};
        strncpy(field, buf + 4, sizeof(field) - 1);
        field[strcspn(field, " \r\n")] = 0;

        const field_desc_t *d = field_find(field);
        if (!d) { REPLY("error", "unknown field"); return 0; }

        // 响应带字段名（旧格式只回裸值，客户端无法把响应对上控件）
        char val[256], msg[320];
        field_read(d, val, sizeof(val));
        snprintf(msg, sizeof(msg), "%s %s", field, val);
        REPLY("val", msg);
        return 0;
    }

    REPLY("error", "unknown command");
    return 0;
    #undef REPLY
}

// ---- 主线程 drain -----------------------------------------------
int sim_console_drain(void)
{
    int processed = 0;
    for (;;) {
        SOCKET fd;
        char *cmd = queue_pop_nb(&s_queue, &fd);
        if (!cmd) break;
        exec_one(cmd, fd);
        // 一条命令的全部响应发完后补一个空行作为帧结束标记，
        // 客户端据此判断读完（不必等 recv 超时）。
        if (fd != INVALID_SOCKET) send_frame_end(fd);
        free(cmd);
        processed++;
        if (processed > 100) break;     // 防止死循环：一帧最多处理 100 条
    }
    return processed;
}

// ---- stdin 线程提交 ---------------------------------------------
bool sim_console_submit(const char *cmd)
{
    if (!cmd || !cmd[0]) return false;
    queue_push(&s_queue, cmd, INVALID_SOCKET);
    return true;
}

// ---- 主线程直接设字段（启动参数用） -----------------------------
// 只支持数值字段（--temp / --humi 这类命令行参数）；字符串字段请走 set 命令。
bool sim_console_set_field_override(const char *field, float val)
{
    const field_desc_t *d = field_find(field);
    if (!d || d->type == FT_STR || d->type == FT_BOOL) return false;

    char buf[32];
    snprintf(buf, sizeof(buf), "%g", (double)val);
    field_write(d, buf);

    if (d->ovr_bit) s_override |= d->ovr_bit;
    ui_pages_apply_locked();
    return true;
}

// ---- socket server 线程 -----------------------------------------
static SOCKET      s_srv = INVALID_SOCKET;
static volatile bool s_running = false;
static pthread_t  s_thread;

static void set_nonblocking(SOCKET fd)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void *server_thread(void *arg)
{
    int port = *(int *)arg;
    free(arg);

    s_srv = socket(AF_INET, SOCK_STREAM, 0);
    if (s_srv == INVALID_SOCKET) {
        fprintf(stderr, "[console] socket() 失败\n");
        return NULL;
    }

    int yes = 1;
    setsockopt(s_srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 仅本地
    addr.sin_port        = htons((uint16_t)port);

    if (bind(s_srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[console] bind(:%d) 失败：%s —— 调试客户端不可用\n",
                port, strerror(SOCK_ERR));
        closesocket(s_srv);
        s_srv = INVALID_SOCKET;
        return NULL;
    }
    if (listen(s_srv, 4) == SOCKET_ERROR) {
        fprintf(stderr, "[console] listen 失败\n");
        closesocket(s_srv);
        s_srv = INVALID_SOCKET;
        return NULL;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "[console] tcp server listening on 127.0.0.1:%d\n", port);
    fprintf(stderr, "%s", msg);

    while (s_running) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        SOCKET cfd = accept(s_srv, (struct sockaddr *)&cli, &clen);
        if (cfd == INVALID_SOCKET) {
#ifdef _WIN32
            if (SOCK_ERR == WSAEWOULDBLOCK) { Sleep(50); continue; }
#else
            if (SOCK_ERR == EAGAIN || SOCK_ERR == EWOULDBLOCK) {
                struct pollfd p = { .fd = s_srv, .events = POLLIN };
                poll(&p, 1, 50);
                continue;
            }
#endif
            if (s_running)
                fprintf(stderr, "[console] accept 错误：%s\n", strerror(SOCK_ERR));
            continue;
        }

        fprintf(stderr, "[console] client connected\n");
        // 一次性服务一个客户端（调试场景不会并发多客户端）
        // 注意：只 recv 文本行并塞进队列，绝不在此线程调 exec_one → 避免 LVGL 多线程崩溃
        char accum[1024];
        int  accum_len = 0;
        set_nonblocking(cfd);

        while (s_running) {
            struct pollfd p = { .fd = cfd, .events = POLLIN };
            int pr = poll(&p, 1, 100);
            if (pr <= 0) continue;

            char tmp[512];
            int n = (int)recv(cfd, tmp, sizeof(tmp) - 1, 0);
            if (n <= 0) break;      // 出错 / 断开
            tmp[n] = 0;

            // 累加并按 \n 切分
            if (accum_len + n < (int)sizeof(accum)) {
                memcpy(accum + accum_len, tmp, n);
                accum_len += n;
            }
            accum[accum_len] = 0;

            char *start = accum;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = 0;
                // 去掉可能的 \r
                size_t len = strlen(start);
                while (len > 0 && start[len-1] == '\r') start[--len] = 0;

                // 塞进队列，主线程 drain 时执行并发回响应
                // quit 命令：发完后关闭连接退出
                if (strcmp(start, "quit") == 0 || strcmp(start, "exit") == 0) {
                    queue_push(&s_queue, start, cfd);
                    closesocket(cfd);
                    goto done;
                }
                queue_push(&s_queue, start, cfd);
                start = nl + 1;
            }
            // 把未处理完的残渣移到头部
            int left = accum_len - (int)(start - accum);
            if (left > 0) memmove(accum, start, left);
            accum_len = left;
        }
        closesocket(cfd);
        fprintf(stderr, "[console] client disconnected\n");
    }
done:
    return NULL;
}

// ---- 公开 API ----------------------------------------------------
bool sim_console_init(int port)
{
    if (port <= 0) port = DEFAULT_PORT;
    queue_init(&s_queue);
    s_running = true;

    int *portp = malloc(sizeof(int));
    *portp = port;
    if (pthread_create(&s_thread, NULL, server_thread, portp) != 0) {
        fprintf(stderr, "[console] pthread_create 失败\n");
        free(portp);
        s_running = false;
        return false;
    }
    pthread_detach(s_thread);
    return true;
}

void sim_console_shutdown(void)
{
    s_running = false;
    if (s_srv != INVALID_SOCKET) {
        closesocket(s_srv);
        s_srv = INVALID_SOCKET;
    }
}
