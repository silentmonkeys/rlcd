// net_portal.c —— SoftAP 配网门户 HTTP 服务器
//
// HTTP endpoints：
//   GET  /          配置表单（网络+天气 / 日历 双标签页，附 WiFi 扫描）
//   GET  /scan      触发一次 WiFi 扫描，返回 JSON: [{ssid,rssi,auth}]
//   POST /save      保存网络/天气配置到 NVS 并 esp_restart
//   POST /save_cal  日历数据存 SD 卡，立即应用，不重启
//
// HTML/JS 模板见 portal_page.h。

#include "net_internal.h"
#include "portal_page.h"
#include "ui_model.h"
#include "ui_pages.h"
#include "ui_calendar.h"
#include "lvgl_bsp.h"
#include "user_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_log.h>
#include <nvs.h>

// ------------ 表单/JSON 辅助 -----------------------------------------
static esp_err_t url_unescape_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && r[1] && r[2]) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char) strtol(h, NULL, 16);
            r += 3;
        } else *w++ = *r++;
    }
    *w = 0;
    return ESP_OK;
}

static void parse_form_field(const char *body, const char *key, char *out, size_t max)
{
    out[0] = 0;
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= max) l = max - 1;
            memcpy(out, v, l);
            out[l] = 0;
            url_unescape_inplace(out);
            return;
        }
        const char *n = strchr(p, '&');
        if (!n) return;
        p = n + 1;
    }
}

// 转义 JS 单引号字符串里的危险字符（' \ 换行）—— 用于 INIT_* 注入。
static void js_escape(char *dst, size_t cap, const char *src)
{
    size_t w = 0;
    for (const char *p = src; *p && w < cap - 2; p++) {
        if (*p == '\'' || *p == '\\') { dst[w++] = '\\'; dst[w++] = *p; }
        else if (*p == '\n' || *p == '\r') { continue; }
        else dst[w++] = *p;
    }
    dst[w] = 0;
}

// 把字符串输出到 JSON —— 转义 " \ 和控制字符
static int append_json_str(char *dst, int cap, const char *s)
{
    int n = 0;
    if (n < cap - 1) dst[n++] = '"';
    for (; *s && n < cap - 8; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') {
            dst[n++] = '\\'; dst[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(dst + n, cap - n, "\\u%04x", c);
        } else {
            dst[n++] = c;
        }
    }
    if (n < cap - 1) dst[n++] = '"';
    dst[n] = 0;
    return n;
}

// ------------ HTTP handlers ------------------------------------------
static esp_err_t root_get(httpd_req_t *req)
{
    // 从 SD 读日历配置，拆成三段（无 SD → 三段为空）
    static char cm[128], ce[512], cl[512];
    cal_read_file(cm, sizeof(cm), ce, sizeof(ce), cl, sizeof(cl));
    // JS 注入前转义
    static char cm_js[160], ce_js[640], cl_js[640];
    js_escape(cm_js, sizeof(cm_js), cm);
    js_escape(ce_js, sizeof(ce_js), ce);
    js_escape(cl_js, sizeof(cl_js), cl);
    int sd_ok = ui_model_get()->sd_mounted ? 1 : 0;

    // 页面含较多 JS（标签页 + 逐条增删逻辑），用 12KB 避免 snprintf 截断
    char *page = (char *) malloc(12288);
    if (!page) return httpd_resp_send_500(req);
    snprintf(page, 12288, PAGE_TEMPLATE,
        s_cfg.ssid, s_cfg.pass, s_cfg.city,
        s_cfg.weather_apikey,
        s_cfg.weather_host,
        sd_ok, cm_js, ce_js, cl_js);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, page);
    free(page);
    return ESP_OK;
}

// GET /scan  → JSON [{ssid,rssi,auth}]
// 正在做 web 扫描时置 s_scanning —— wifi_evt 的 disconnect 处理里查询它决定要不要
// 抢占 esp_wifi_connect()。
static esp_err_t scan_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");

    if (!s_wifi_common_inited) {
        ESP_LOGW(NET_TAG, "scan: wifi not inited");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    // 一次一个客户端扫描
    if (xSemaphoreTake(s_scan_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(NET_TAG, "scan: mux busy");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    // 关键：先停掉 STA 的 auto-reconnect 循环 —— 否则 STA 处在 CONNECTING 状态，
    // esp_wifi_scan_start 会立刻返回 ESP_ERR_WIFI_STATE 拒绝扫描。
    // disconnect 是幂等的，未连接时也无副作用。
    s_scanning = true;
    esp_wifi_disconnect();
    // 给 STA 一小段时间从 CONNECTING/CONNECTED 掉到 IDLE，扫描才能启动
    vTaskDelay(pdMS_TO_TICKS(150));

    // ALL_CHANNEL_SCAN 主动扫可以看到隐藏之外的绝大多数 AP
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 120;
    sc.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&sc, true);   // blocking
    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "scan_start fail: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        s_scanning = false;
        // 让 STA 恢复重连（若有凭据）
        if (s_want_sta) esp_wifi_connect();
        xSemaphoreGive(s_scan_mux);
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(NET_TAG, "scan done: %u APs", (unsigned)n);
    if (n > 24) n = 24;                                // 前 24 个够用
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        s_scanning = false;
        if (s_want_sta) esp_wifi_connect();
        xSemaphoreGive(s_scan_mux);
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    // 扫描结束 —— 恢复 STA 自动重连（有凭据时）
    s_scanning = false;
    if (s_want_sta) esp_wifi_connect();
    xSemaphoreGive(s_scan_mux);

    // 组 JSON —— 一次装完发出去，简单可靠
    // 每条最多 ~120B，24 条 < 4KB
    char *out = malloc(4096);
    if (!out) { free(recs); httpd_resp_sendstr(req, "[]"); return ESP_OK; }
    int p = 0;
    out[p++] = '[';
    // 去重（部分路由会在多个信道回应）
    for (int i = 0; i < n && p < 4090; i++) {
        // skip duplicates
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((const char *)recs[j].ssid, (const char *)recs[i].ssid) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;
        if (recs[i].ssid[0] == 0) continue;   // 隐藏 SSID
        if (out[p - 1] != '[') out[p++] = ',';
        out[p++] = '{';
        p += snprintf(out + p, 4096 - p, "\"ssid\":");
        p += append_json_str(out + p, 4096 - p, (const char *)recs[i].ssid);
        p += snprintf(out + p, 4096 - p, ",\"rssi\":%d,\"auth\":%d}",
                      (int)recs[i].rssi, (int)recs[i].authmode);
    }
    if (p < 4090) out[p++] = ']';
    out[p] = 0;

    httpd_resp_sendstr(req, out);
    free(out);
    free(recs);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[512] = {0};
    int received = 0, r;
    while (received < (int) sizeof(body) - 1) {
        r = httpd_req_recv(req, body + received, sizeof(body) - 1 - received);
        if (r <= 0) break;
        received += r;
    }
    body[received] = 0;

    net_config_t c = {0};
    parse_form_field(body, "ssid",     c.ssid,             sizeof(c.ssid));
    parse_form_field(body, "pass",     c.pass,             sizeof(c.pass));
    parse_form_field(body, "city",     c.city,             sizeof(c.city));
    parse_form_field(body, "apikey",   c.weather_apikey,   sizeof(c.weather_apikey));
    parse_form_field(body, "host",     c.weather_host,     sizeof(c.weather_host));

    if (c.ssid[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }
    bool save_ok = NetBsp_SaveConfig(&c);
    // 立刻读回验证是否真正落盘
    char vh_host[64] = {0}; size_t vsz = sizeof(vh_host);
    nvs_handle_t vh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &vh) == ESP_OK) {
        nvs_get_str(vh, "weather_host", vh_host, &vsz);
        nvs_close(vh);
    }
    ESP_LOGI(NET_TAG, "config saved ok=%d ssid='%s' host='%s' city='%s' key=%s | verify_read host='%s', rebooting…",
             save_ok, c.ssid, c.weather_host, c.city,
             c.weather_apikey[0] ? "set" : "EMPTY", vh_host[0] ? vh_host : "EMPTY");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body><h3>Saved. Rebooting in 2s...</h3></body></html>");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// POST /save_cal —— 日历数据（标记/预定/标签）存 SD 卡，立即应用，不重启。
// 表单字段：cal_marks / cal_events / cal_labels（与旧格式一致，前端负责编码）。
// 中文 URL 编码后体积约 3 倍，用 4KB 动态缓冲。SD 未挂载返回 409。
static esp_err_t save_cal_post(httpd_req_t *req)
{
    char *body = (char *) malloc(4096);
    if (!body) return httpd_resp_send_500(req);
    int received = 0, r;
    while (received < 4096 - 1) {
        r = httpd_req_recv(req, body + received, 4096 - 1 - received);
        if (r <= 0) break;
        received += r;
    }
    body[received] = 0;

    static char marks[128], events[512], labels[512];
    parse_form_field(body, "cal_marks",  marks,  sizeof(marks));
    parse_form_field(body, "cal_events", events, sizeof(events));
    parse_form_field(body, "cal_labels", labels, sizeof(labels));
    free(body);

    if (!ui_model_get()->sd_mounted) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "无 SD 卡，无法保存日历数据");
        return ESP_OK;
    }

    bool ok = cal_save_to_sd(marks, events, labels);
    if (ok) {
        // setter 只写静态数组、不碰 LVGL，无需持锁；锁只保护 apply。
        // 锁超时也不影响：数组已更新，1s tick 会用新数据重绘。
        ui_calendar_set_marks(marks);
        ui_calendar_set_events(events);
        ui_calendar_set_labels(labels);
        if (Lvgl_lock(200)) {
            ui_pages_apply_locked();
            Lvgl_unlock();
        }
    }
    ESP_LOGI(NET_TAG, "cal saved ok=%d marks='%s' events='%s' labels='%s'",
             ok, marks, events, labels);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, ok ? "OK" : "写 SD 失败");
    return ESP_OK;
}

void config_httpd_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8 * 1024;   // JSON 生成 + snprintf 需要点栈
    // scan_get 阻塞 ~2-3s，把发送/接收 timeout 拉长避免浏览器提前断连
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) return;
    httpd_uri_t u_root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t u_scan = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t u_cal  = { .uri = "/save_cal", .method = HTTP_POST, .handler = save_cal_post };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_cal);
}
