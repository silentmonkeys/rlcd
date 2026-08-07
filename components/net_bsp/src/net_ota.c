// net_ota.c —— 门户网页上传固件做 OTA（POST /api/ota）
//
// 流程：浏览器把 .bin 以 raw body（Content-Type: application/octet-stream）
// POST 上来，这里边收边写另一个 app 槽，收完校验、置启动分区、回 JSON 后重启。
//
// 设计要点：
//   * **流式写入，不缓存整个固件**。app 有 1.6MB+，设备可用堆撑不住整份缓存；
//     每收一块（1KB）立刻 esp_ota_write()，内存占用恒定。
//   * 缓冲放**静态区**而非任务栈 —— httpd 任务栈只有 8KB。同一时刻只允许一个
//     OTA 在跑（s_busy 互斥），静态缓冲不存在竞争。
//   * **连续停顿超 45s 就放弃**。httpd 没有 TCP keepalive，掉线/半开连接时
//     recv() 会无限返回 TIMEOUT；不累计判死就会把单任务的 httpd 永久卡住。
//   * **先校验魔数再擦写**。固件头首字节必须是 0xE9（ESP 镜像魔数），传错文件
//     （比如把 fonts.bin 或压缩包传上来）在擦掉备用槽之前就拒掉，设备不受影响。
//   * 失败一律 esp_ota_abort() 放弃会话，**不动 otadata**，当前运行的固件毫发无损。
//   * 成功后 esp_ota_set_boot_partition() 只是改指针；新固件首次启动处于
//     PENDING_VERIFY，要连续稳定跑满 60s 才由 NetBsp_OtaSelfTestTick() 确认，
//     否则 bootloader 自动回滚到上一个槽（详见该函数）。
//
// 安全说明：本接口不做身份认证 —— 与门户其余接口（改 WiFi、清凭据、重启）
// 一致，安全边界是「能连上这台设备的局域网/AP」。不要把门户暴露到公网。

#include "net_internal.h"

#include <string.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define OTA_BUF_SZ      1024        // 单次 recv 块大小；httpd 栈小，缓冲放静态区
#define OTA_MIN_SZ      (64 * 1024) // 小于这个的不可能是完整 app，直接拒

// 连续这么久收不到**任何**字节就放弃整个会话。
//
// 必须有这道闸：httpd 的 recv_wait_timeout 只让单次 recv() 返回 TIMEOUT，
// 它自己不累计、也没有 TCP keepalive（keep_alive_enable=false）。上传中途
// 掉线或半开连接（拔网线 / 手机休眠，对端不发 FIN）时 recv() 会一直返回
// TIMEOUT，`continue` 就变成死循环 —— 而 httpd 是**单任务**串行处理，那意味着
// 整个门户永久失效 + s_busy 卡死，只能物理重启。
#define OTA_STALL_US    ((int64_t)45 * 1000 * 1000)

// 单次 recv 块大小的静态缓冲。httpd 任务栈只有 8KB，放栈上不安全；
// 同一时刻只允许一个 OTA（s_busy 互斥）+ httpd 单任务，所以不存在竞争。
static char s_buf[OTA_BUF_SZ];

// 同一时刻只允许一个 OTA 会话。httpd 默认单任务处理 handler，这里再上一道
// 保险，避免两个浏览器同时上传把同一个槽写花。
static volatile bool s_busy = false;

// 曾经这里有个 s_progress/s_recved + GET /api/ota_status 让前端查"设备侧
// 写进去多少"。已删除：httpd 是**单任务串行**的，ota_post 阻塞收包期间根本
// 轮不到别的 handler，那个请求只会堆在 backlog 里等上传结束 —— 想查的时候
// 查不到，查得到的时候已经没用了。前端改为只用 XHR 的 upload.onprogress。

// =====================================================================
// OTA 自检 / 回滚确认
// =====================================================================
// 新固件被 esp_ota_set_boot_partition() 选中后，首次启动处于
// ESP_OTA_IMG_PENDING_VERIFY（需 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y）。
// 此状态下若 panic / 看门狗复位，bootloader 会自动切回上一个槽 —— 这是
// "刷了个能过 SHA256 但起不来的固件" 唯一的自动救命通道。
//
// 关键是**别急着 mark**：如果在 app_main 末尾直接确认，那些"能启动但几秒后
// 崩"的固件同样会被认定可用，回滚就白设了。这里要求连续稳定运行满
// OTA_SELFTEST_HOLD_S 秒（期间没崩、没被喂狗复位）才确认。
#define OTA_SELFTEST_HOLD_S   60

void NetBsp_OtaSelfTestTick(void)
{
    // 一旦确认过（或本来就不是待确认状态）就彻底不再进来
    static bool s_done = false;
    if (s_done) return;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        s_done = true;
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        s_done = true;   // 正常烧写启动 / 早已确认过，无需自检
        return;
    }

    // 首次发现处于待确认态：记下起点，攒够时长再确认
    static int64_t s_first_seen_us = 0;
    const int64_t  now = esp_timer_get_time();
    if (s_first_seen_us == 0) {
        s_first_seen_us = now;
        ESP_LOGW(NET_TAG, "OTA 新固件首次启动（%s），自检 %d 秒后确认；"
                          "期间若复位将自动回滚到上一个槽",
                 running->label, OTA_SELFTEST_HOLD_S);
        return;
    }
    if (now - s_first_seen_us < (int64_t) OTA_SELFTEST_HOLD_S * 1000 * 1000)
        return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    s_done = true;   // 无论成败都不再重试：失败说明状态机已不在待确认态
    if (err == ESP_OK)
        ESP_LOGW(NET_TAG, "OTA 自检通过：%s 已标记为可用，回滚取消", running->label);
    else
        ESP_LOGE(NET_TAG, "esp_ota_mark_app_valid_cancel_rollback: %s",
                 esp_err_to_name(err));
}

// 统一的失败出口：放弃 OTA 会话 + 清状态 + 回错误 JSON
static esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t h,
                          const char *status, const char *msg)
{
    if (h) esp_ota_abort(h);
    s_busy = false;
    ESP_LOGE(NET_TAG, "ota 失败：%s", msg);
    return send_err(req, status, msg);
}

// 拒绝一个还没开始收的请求（体积不对、没有可用分区、已有会话在跑……）。
//
// 麻烦在于 body 还挂在 socket 上没读。三种处理各有代价：
//   a) 直接 return 让 httpd 自己 purge —— 它用的是 CONFIG_HTTPD_PURGE_BUF_LEN
//      （本项目 32 字节）的小桶，误传一个 5MB 文件要 16 万次 recv，门户全程阻塞；
//   b) return ESP_FAIL 让 httpd 关连接 —— 未读 body 会触发 TCP RST，
//      浏览器大概率**丢掉刚发出的错误 JSON**，用户只看到"连接被重置"，
//      而"传错文件"恰恰是最需要清晰提示的场景；
//   c) 自己用 1KB 桶把 body 读掉再回 JSON —— recv 次数降到 1/32，
//      响应也能干净送达。这里取 (c)，并对超大 body 退回 (b)。
//
// 注意顺序：**先排空再回响应**。反过来的话，部分客户端收到早期响应就停止
// 上传，drain 会一路等到停顿超时才收摊。
//
// `left` = 还挂在 socket 上没读的字节数，由调用方算准（魔数失败那条路已经
// 读掉了第一块）。httpd_req_recv() 内部把请求钳在自己的 remaining_len 上，
// 读完后返回 0 —— 而返回 0 同时也是"对端断开"的信号。若这里按 content_len
// 而不是真实剩余量循环，最后那次读到 0 会被误判成断线，退化成 RST 把响应打掉。
#define OTA_DRAIN_MAX   (3 * 1024 * 1024)   // 超过这个就不陪着排空了，直接断

static esp_err_t ota_reject(httpd_req_t *req, const char *status,
                            const char *msg, int left)
{
    if (left > OTA_DRAIN_MAX) {
        // 排空的代价超过一次 RST：先尽力回响应，再让 httpd 关掉连接。
        // 前端 XHR 的 onerror 分支会给出兜底提示。
        ESP_LOGE(NET_TAG, "ota 拒绝（剩余 %d 字节，过大不排空，直接断开）：%s", left, msg);
        send_err(req, status, msg);
        return ESP_FAIL;
    }

    const int drained = left;
    int64_t   last_rx = esp_timer_get_time();
    while (left > 0) {
        int want = (left > OTA_BUF_SZ) ? OTA_BUF_SZ : left;
        int got  = httpd_req_recv(req, s_buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            // 客户端可能已经放弃上传，别无限等
            if (esp_timer_get_time() - last_rx > OTA_STALL_US) return ESP_FAIL;
            continue;
        }
        if (got <= 0) return ESP_FAIL;   // 对端已断，响应也没人收了
        left    -= got;
        last_rx  = esp_timer_get_time();
    }

    ESP_LOGE(NET_TAG, "ota 拒绝（已排空 %d 字节）：%s", drained, msg);
    return send_err(req, status, msg);
}

esp_err_t ota_post(httpd_req_t *req)
{
    const int total = req->content_len;

    if (s_busy)
        return ota_reject(req, "409 Conflict", "已有固件正在上传，请稍后重试", total);

    if (total <= 0)
        return send_err(req, "400 Bad Request", "请求未包含固件数据");
    if (total < OTA_MIN_SZ)
        return ota_reject(req, "400 Bad Request", "文件过小，不是有效的固件镜像", total);

    // 目标槽 = 当前没在运行的那个（ota_0 ↔ ota_1 轮换）
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);
    if (!target)
        return ota_reject(req, "500 Internal Server Error",
                          "未找到可用的 OTA 分区，请确认分区表已启用双 app 槽", total);
    if ((size_t) total > target->size)
        return ota_reject(req, "400 Bad Request", "固件体积超出分区容量", total);

    ESP_LOGI(NET_TAG, "ota 开始：%d 字节 → %s（当前运行 %s）",
             total, target->label, running ? running->label : "?");

    s_busy = true;

    esp_ota_handle_t handle  = 0;
    bool             begun   = false;
    int              recved  = 0;
    int64_t          last_rx = esp_timer_get_time();   // 最后一次真正收到字节的时刻

    while (recved < total) {
        int want = total - recved;
        if (want > OTA_BUF_SZ) want = OTA_BUF_SZ;

        int got = httpd_req_recv(req, s_buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            // 单次 recv 超时不代表连接断了（慢网络很正常），但**连续停顿**
            // 超过 OTA_STALL_US 就当对端已经消失，主动收摊 —— 否则这里是死循环。
            if (esp_timer_get_time() - last_rx > OTA_STALL_US)
                return ota_fail(req, begun ? handle : 0, "408 Request Timeout",
                                "上传超时中断，固件未写入");
            continue;
        }
        if (got <= 0)
            return ota_fail(req, begun ? handle : 0, "400 Bad Request",
                            "上传中断，固件未写入");
        last_rx = esp_timer_get_time();

        // 第一块：先验魔数，通过了再 esp_ota_begin（begin 会擦除整个分区，
        // 所以务必放在校验之后 —— 传错文件不该让备用槽先被擦掉）
        if (!begun) {
            if ((uint8_t) s_buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                // 传错文件（fonts.bin / 压缩包 / 别的芯片的镜像）走这条路，
                // 是实际最常见的失败。此时已读掉 got 字节，剩下几 MB 仍在
                // socket 上 —— 交给 ota_reject 用 1KB 桶排空后再回 JSON，
                // 别让 httpd 用 32 字节桶慢慢啃（见 ota_reject 注释）。
                s_busy = false;
                return ota_reject(req, "400 Bad Request",
                                  "文件不是 ESP32 固件镜像（魔数校验未通过）",
                                  total - got);
            }

            esp_err_t err = esp_ota_begin(target, total, &handle);
            if (err != ESP_OK) {
                ESP_LOGE(NET_TAG, "esp_ota_begin: %s", esp_err_to_name(err));
                return ota_fail(req, 0, "500 Internal Server Error",
                                "无法启动升级会话");
            }
            begun = true;
        }

        esp_err_t err = esp_ota_write(handle, s_buf, (size_t) got);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return ota_fail(req, handle, "500 Internal Server Error",
                            "写入 Flash 失败");
        }

        recved    += got;
    }

    // esp_ota_end 会校验镜像完整性（含 SHA256），坏包在这里被挡下
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        const char *msg = (err == ESP_ERR_OTA_VALIDATE_FAILED)
                          ? "固件校验失败，镜像已损坏或不完整"
                          : "结束升级会话失败";
        ESP_LOGE(NET_TAG, "esp_ota_end: %s", esp_err_to_name(err));
        // handle 已被 esp_ota_end 释放，不能再 abort
        s_busy = false;
        return send_err(req, "400 Bad Request", msg);
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "set_boot_partition: %s", esp_err_to_name(err));
        s_busy = false;
        return send_err(req, "500 Internal Server Error", "无法切换启动分区");
    }

    ESP_LOGW(NET_TAG, "ota 成功：%d 字节已写入 %s，即将重启", recved, target->label);

    // 先把响应发完再重启，否则浏览器只会看到"连接被重置"而不知道成功了
    send_ok(req);
    s_busy = false;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}
