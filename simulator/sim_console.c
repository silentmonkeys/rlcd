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
// 位定义在 sim_console.h 的 OVR_* 宏里，这里用前缀 F_ 避免和宏重名。
enum {
    F_INDOOR_TEMP  = OVR_INDOOR_TEMP,
    F_INDOOR_HUMI  = OVR_INDOOR_HUMI,
    F_OUTDOOR_TEMP = OVR_OUTDOOR_TEMP,
    F_WEATHER_CODE = OVR_WEATHER_CODE,
    F_WEATHER_TEXT = OVR_WEATHER_TEXT,
    F_CITY         = OVR_CITY,
    F_WIFI_CONN    = OVR_WIFI_CONN,
    F_WIFI_RSSI    = OVR_WIFI_RSSI,
    F_BAT_PCT      = OVR_BAT_PCT,
    F_BAT_CHG      = OVR_BAT_CHG,
    F_FEELS_LIKE   = OVR_FEELS_LIKE,
    F_OUTDOOR_HUMI = OVR_OUTDOOR_HUMI,
    F_WIND_SPD     = OVR_WIND_SPD,
    F_WIND_DIR     = OVR_WIND_DIR,
    F_CLOUD        = OVR_CLOUD,
    F_PRESSURE     = OVR_PRESSURE,
    F_VISIBILITY   = OVR_VISIBILITY,
    F_UV           = OVR_UV,
    F_TEMP_MIN     = OVR_TEMP_MIN,
    F_TEMP_MAX     = OVR_TEMP_MAX,
    F_SUNRISE      = OVR_SUNRISE,
    F_SUNSET       = OVR_SUNSET,
};

static uint32_t s_override = 0;

// 提供给 main.c：sim_tick_data 查询哪些字段被外部设置
uint32_t sim_console_override_mask(void) { return s_override; }

// ---- 响应输出辅助 ------------------------------------------------
static void send_line(SOCKET fd, const char *prefix, const char *body)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s %s\n", prefix, body);
    if (n > 0) send(fd, buf, n, 0);
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
        REPLY("ok", "cmds: set <field> <val> | get <field> | page N | next | prev | mark MM-DD | unmark | marks CSV | events SPEC | labels SPEC | clear | ping | quit");
        REPLY("ok", "fields: indoor_temp indoor_humi outdoor_temp weather_code weather_text city wifi_connected wifi_rssi battery_percent battery_charging feels_like outdoor_humi wind_speed wind_dir cloud pressure visibility uv temp_min temp_max sunrise sunset");
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
        // 取字段名（到空格为止）
        const char *sp = strchr(rest, ' ');
        if (!sp) { REPLY("error", "usage: set <field> <value>"); return 0; }
        size_t flen = (size_t)(sp - rest);
        if (flen >= sizeof(field)) flen = sizeof(field) - 1;
        memcpy(field, rest, flen);
        field[flen] = 0;
        const char *val = sp + 1;

        ui_model_t *m = ui_model_get();
        uint32_t bit = 0;
        char msg[128];
        bool matched = false;

        // 数值字段 —— 用 if/else if 链匹配字段名并写值
        if (strcmp(field, "indoor_temp") == 0) {
            m->indoor_temp = (float)atof(val); bit = OVR_INDOOR_TEMP; matched = true;
        } else if (strcmp(field, "indoor_humi") == 0) {
            m->indoor_humi = (float)atof(val); bit = OVR_INDOOR_HUMI; matched = true;
        } else if (strcmp(field, "outdoor_temp") == 0) {
            m->outdoor_temp = (float)atof(val); bit = OVR_OUTDOOR_TEMP; matched = true;
        } else if (strcmp(field, "weather_code") == 0) {
            m->weather_code = atoi(val); bit = OVR_WEATHER_CODE; matched = true;
        } else if (strcmp(field, "weather_text") == 0) {
            strncpy(m->weather_text, val, sizeof(m->weather_text) - 1);
            m->weather_text[sizeof(m->weather_text) - 1] = 0; bit = OVR_WEATHER_TEXT; matched = true;
        } else if (strcmp(field, "city") == 0) {
            strncpy(m->city, val, sizeof(m->city) - 1);
            m->city[sizeof(m->city) - 1] = 0; bit = OVR_CITY; matched = true;
        } else if (strcmp(field, "wifi_connected") == 0) {
            m->wifi_connected = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "on") == 0);
            bit = OVR_WIFI_CONN; matched = true;
        } else if (strcmp(field, "wifi_rssi") == 0) {
            m->wifi_rssi = (int8_t)atoi(val); bit = OVR_WIFI_RSSI; matched = true;
        } else if (strcmp(field, "battery_percent") == 0) {
            m->battery_percent = (uint8_t)atoi(val); bit = OVR_BAT_PCT; matched = true;
        } else if (strcmp(field, "battery_charging") == 0) {
            m->battery_charging = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 || strcmp(val, "on") == 0);
            bit = OVR_BAT_CHG; matched = true;
        } else if (strcmp(field, "feels_like_temp") == 0) {
            m->feels_like_temp = (float)atof(val); bit = OVR_FEELS_LIKE; matched = true;
        } else if (strcmp(field, "outdoor_humi") == 0) {
            m->outdoor_humi = (float)atof(val); bit = OVR_OUTDOOR_HUMI; matched = true;
        } else if (strcmp(field, "wind_speed") == 0) {
            m->wind_speed_kmh = (float)atof(val); bit = OVR_WIND_SPD; matched = true;
        } else if (strcmp(field, "wind_dir") == 0) {
            strncpy(m->wind_dir, val, sizeof(m->wind_dir) - 1);
            m->wind_dir[sizeof(m->wind_dir) - 1] = 0; bit = OVR_WIND_DIR; matched = true;
        } else if (strcmp(field, "cloud") == 0) {
            m->cloud_pct = atoi(val); bit = OVR_CLOUD; matched = true;
        } else if (strcmp(field, "pressure") == 0) {
            m->pressure_hpa = atoi(val); bit = OVR_PRESSURE; matched = true;
        } else if (strcmp(field, "visibility") == 0) {
            m->visibility_km = atoi(val); bit = OVR_VISIBILITY; matched = true;
        } else if (strcmp(field, "uv") == 0) {
            m->uv_index = atoi(val); bit = OVR_UV; matched = true;
        } else if (strcmp(field, "temp_min") == 0) {
            m->temp_min = atoi(val); bit = OVR_TEMP_MIN; matched = true;
        } else if (strcmp(field, "temp_max") == 0) {
            m->temp_max = atoi(val); bit = OVR_TEMP_MAX; matched = true;
        } else if (strcmp(field, "sunrise") == 0) {
            strncpy(m->sunrise, val, sizeof(m->sunrise) - 1);
            m->sunrise[sizeof(m->sunrise) - 1] = 0; bit = OVR_SUNRISE; matched = true;
        } else if (strcmp(field, "sunset") == 0) {
            strncpy(m->sunset, val, sizeof(m->sunset) - 1);
            m->sunset[sizeof(m->sunset) - 1] = 0; bit = OVR_SUNSET; matched = true;
        }

        if (!matched) {
            REPLY("error", "unknown field");
            return 0;
        }

        if (bit) s_override |= bit;
        ui_pages_apply_locked();
        snprintf(msg, sizeof(msg), "%s = %s", field, val);
        REPLY("ok", msg);
        return 0;
    }

    // ---- get <field> ----
    if (strncmp(buf, "get ", 4) == 0) {
        char field[64] = {0};
        strncpy(field, buf + 4, sizeof(field) - 1);
        field[strcspn(field, " \r\n")] = 0;

        ui_model_t *m = ui_model_get();
        char out[256] = {0};
        bool matched = false;

        if (strcmp(field, "indoor_temp") == 0) {
            snprintf(out, sizeof(out), "%g", (double)m->indoor_temp); matched = true;
        } else if (strcmp(field, "indoor_humi") == 0) {
            snprintf(out, sizeof(out), "%g", (double)m->indoor_humi); matched = true;
        } else if (strcmp(field, "outdoor_temp") == 0) {
            snprintf(out, sizeof(out), "%g", (double)m->outdoor_temp); matched = true;
        } else if (strcmp(field, "weather_code") == 0) {
            snprintf(out, sizeof(out), "%d", m->weather_code); matched = true;
        } else if (strcmp(field, "weather_text") == 0) {
            snprintf(out, sizeof(out), "%s", m->weather_text); matched = true;
        } else if (strcmp(field, "city") == 0) {
            snprintf(out, sizeof(out), "%s", m->city); matched = true;
        } else if (strcmp(field, "wifi_connected") == 0) {
            snprintf(out, sizeof(out), "%s", m->wifi_connected ? "true" : "false"); matched = true;
        } else if (strcmp(field, "wifi_rssi") == 0) {
            snprintf(out, sizeof(out), "%d", (int)m->wifi_rssi); matched = true;
        } else if (strcmp(field, "battery_percent") == 0) {
            snprintf(out, sizeof(out), "%d", (int)m->battery_percent); matched = true;
        } else if (strcmp(field, "battery_charging") == 0) {
            snprintf(out, sizeof(out), "%s", m->battery_charging ? "true" : "false"); matched = true;
        } else if (strcmp(field, "feels_like_temp") == 0) {
            snprintf(out, sizeof(out), "%g", (double)m->feels_like_temp); matched = true;
        } else if (strcmp(field, "outdoor_humi") == 0) {
            snprintf(out, sizeof(out), "%g", (double)m->outdoor_humi); matched = true;
        } else if (strcmp(field, "wind_speed") == 0) {
            snprintf(out, sizeof(out), "%g", (double)m->wind_speed_kmh); matched = true;
        } else if (strcmp(field, "wind_dir") == 0) {
            snprintf(out, sizeof(out), "%s", m->wind_dir); matched = true;
        } else if (strcmp(field, "cloud") == 0) {
            snprintf(out, sizeof(out), "%d", m->cloud_pct); matched = true;
        } else if (strcmp(field, "pressure") == 0) {
            snprintf(out, sizeof(out), "%d", m->pressure_hpa); matched = true;
        } else if (strcmp(field, "visibility") == 0) {
            snprintf(out, sizeof(out), "%d", m->visibility_km); matched = true;
        } else if (strcmp(field, "uv") == 0) {
            snprintf(out, sizeof(out), "%d", m->uv_index); matched = true;
        } else if (strcmp(field, "temp_min") == 0) {
            snprintf(out, sizeof(out), "%d", m->temp_min); matched = true;
        } else if (strcmp(field, "temp_max") == 0) {
            snprintf(out, sizeof(out), "%d", m->temp_max); matched = true;
        } else if (strcmp(field, "sunrise") == 0) {
            snprintf(out, sizeof(out), "%s", m->sunrise); matched = true;
        } else if (strcmp(field, "sunset") == 0) {
            snprintf(out, sizeof(out), "%s", m->sunset); matched = true;
        }

        if (!matched) { REPLY("error", "unknown field"); return 0; }

        REPLY("val", out);
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
bool sim_console_set_field_override(const char *field, float val)
{
    ui_model_t *m = ui_model_get();
    uint32_t bit = 0;

    if (strcmp(field, "indoor_temp") == 0) {
        m->indoor_temp = val; bit = OVR_INDOOR_TEMP;
    } else if (strcmp(field, "indoor_humi") == 0) {
        m->indoor_humi = val; bit = OVR_INDOOR_HUMI;
    } else if (strcmp(field, "outdoor_temp") == 0) {
        m->outdoor_temp = val; bit = OVR_OUTDOOR_TEMP;
    } else if (strcmp(field, "feels_like_temp") == 0) {
        m->feels_like_temp = val; bit = OVR_FEELS_LIKE;
    } else if (strcmp(field, "outdoor_humi") == 0) {
        m->outdoor_humi = val; bit = OVR_OUTDOOR_HUMI;
    } else if (strcmp(field, "wind_speed") == 0) {
        m->wind_speed_kmh = val; bit = OVR_WIND_SPD;
    } else {
        return false;
    }

    if (bit) s_override |= bit;
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
