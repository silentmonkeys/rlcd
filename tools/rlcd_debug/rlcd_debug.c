/*
 * rlcd_debug —— RLCD 模拟器独立调试客户端
 *
 * 通过 TCP 连接模拟器（默认 127.0.0.1:9000），提供交互式命令行来读写
 * ui_model 字段、切页、标注日历等。所有命令以文本行发送，响应同样文本行。
 *
 * 用途：
 *   - 解决原 stdin 控制台"数据只显示一秒"问题（override 机制保留用户设置）
 *   - 解决原 stdin 多次输入崩溃问题（LVGL 操作全在模拟器主线程）
 *   - 独立的调试工具，不影响项目主体
 *
 * 构建：
 *   gcc -O2 -o rlcd_debug rlcd_debug.c
 *   # 或 cmake（见 CMakeLists.txt）
 *
 * 用法：
 *   ./rlcd_debug                 连接本地 :9000
 *   ./rlcd_debug 127.0.0.1:9100  指定地址
 *   ./rlcd_debug --batch "set indoor_temp 35" "get indoor_temp"  非交互单发
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#define closesocket closesocket
#define SOCK_ERR   WSAGetLastError()
#define sock_errno WSAGetLastError()
#define POLL       WSAPoll
typedef ULONG nfds_t;
#define INIT_NET() do { \
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); } while (0)
#define CLEANUP_NET() WSACleanup()
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
typedef int SOCKET;
#define SOCK_ERR errno
#define closesocket close
#define POLL poll
typedef nfds_t nfds_t;
#define INIT_NET()   do {} while (0)
#define CLEANUP_NET() do {} while (0)
#endif

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9000

static SOCKET s_fd = INVALID_SOCKET;
static volatile bool s_running = true;

static void on_signal(int sig) { (void)sig; s_running = false; }

// ---- 网络辅助 ---------------------------------------------------
static SOCKET tcp_connect(const char *host, int port)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he) { closesocket(fd); return INVALID_SOCKET; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

static int send_cmd(SOCKET fd, const char *cmd)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    return (int)send(fd, buf, n, 0);
}

// 读一行响应（以 \n 结尾）。返回 0 成功，-1 出错。
static int recv_line(SOCKET fd, char *out, int maxlen)
{
    int len = 0;
    while (len < maxlen - 1) {
        char c;
        int n = (int)recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        out[len++] = c;
    }
    out[len] = 0;
    // 去 \r
    while (len > 0 && out[len-1] == '\r') out[--len] = 0;
    return 0;
}

// ---- 交互模式 ---------------------------------------------------
static void print_banner(void)
{
    printf(
        "╔══════════════════════════════════════════════════╗\n"
        "║  RLCD Debug Client — 模拟器远程调试工具        ║\n"
        "╠══════════════════════════════════════════════════╣\n"
        "║  set <field> <value>   写字段（override 保 留）║\n"
        "║  get <field>           读字段                   ║\n"
        "║  page N / next / prev  切页                     ║\n"
        "║  mark MM-DD / unmark   日历标注                 ║\n"
        "║  marks CSV / events SPEC / labels SPEC          ║\n"
        "║  clear                 清除 override，恢复自动  ║\n"
        "║  ping / help / quit                              ║\n"
        "╚══════════════════════════════════════════════════╝\n"
    );
}

static int interactive(SOCKET fd)
{
    print_banner();
    char line[1024];
    while (s_running) {
        printf("rlcd> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;

        if (send_cmd(fd, line) <= 0) {
            fprintf(stderr, "[err] 发送失败，连接断开\n");
            break;
        }

        // 读响应直到遇到空行或超时
        struct pollfd p = { .fd = fd, .events = POLLIN };
        char resp[1024];
        while (1) {
            int pr = POLL(&p, 1, 300);
            if (pr <= 0) break;
            if (recv_line(fd, resp, sizeof(resp)) < 0) { s_running = false; break; }
            if (resp[0] == 0) break;
            printf("  ← %s\n", resp);
            if (!s_running) break;
        }

        if (strcmp(line, "quit") == 0) break;
    }
    return 0;
}

// ---- 批量模式 ---------------------------------------------------
static int batch(SOCKET fd, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (send_cmd(fd, argv[i]) <= 0) {
            fprintf(stderr, "[err] send failed\n");
            return 1;
        }
        char resp[1024];
        // 每条命令可能有多行响应，读到空行
        struct pollfd p = { .fd = fd, .events = POLLIN };
        while (1) {
            int pr = POLL(&p, 1, 500);
            if (pr <= 0) break;
            if (recv_line(fd, resp, sizeof(resp)) < 0) return 1;
            if (resp[0] == 0) break;
            printf("%s\n", resp);
        }
    }
    return 0;
}

// ---- 入口 -------------------------------------------------------
int main(int argc, char **argv)
{
    INIT_NET();
    signal(SIGINT, on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    bool batch_mode = false;

    // 解析参数
    int argi = 1;
    for (; argi < argc; argi++) {
        if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            printf("usage: %s [host:port] [--batch] [commands...]\n", argv[0]);
            printf("  default host:port = %s:%d\n", DEFAULT_HOST, DEFAULT_PORT);
            CLEANUP_NET();
            return 0;
        } else if (strcmp(argv[argi], "--batch") == 0) {
            batch_mode = true;
            argi++;
            break;
        } else if (argv[argi][0] != '-') {
            // 解析 host:port
            char *colon = strrchr(argv[argi], ':');
            if (colon) {
                *colon = 0;
                host = argv[argi];
                port = atoi(colon + 1);
            } else {
                port = atoi(argv[argi]);
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            CLEANUP_NET();
            return 1;
        }
    }

    s_fd = tcp_connect(host, port);
    if (s_fd == INVALID_SOCKET) {
        fprintf(stderr, "[err] 无法连接 %s:%d —— 模拟器是否已启动？\n", host, port);
        CLEANUP_NET();
        return 1;
    }
    printf("[已连接 %s:%d]\n", host, port);

    int rc;
    if (batch_mode) {
        rc = batch(s_fd, argc - argi, argv + argi);
    } else {
        rc = interactive(s_fd);
    }

    closesocket(s_fd);
    CLEANUP_NET();
    return rc;
}
