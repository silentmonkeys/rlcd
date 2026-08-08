// net_portal.c —— 设备管理 / 配网门户 HTTP 服务器
//
// HTTP endpoints：
//   GET  /                   完整页面（HTML+CSS+JS 内联，gzip 预压缩，单请求）
//   GET  /chart.umd.min.js   Chart.js（gzip 预压缩，仅"数据"页按需加载）
//   GET  /api/status         实时状态看板数据（只含会变的字段）
//   GET  /api/sysinfo        静态设备信息（型号/固件/MAC…），前端只取一次
//   GET  /api/limits         日历容量上限（来源 ui_calendar.h）
//   GET  /api/config         配置回显 —— 密码/APIKey 只回布尔，绝不回明文
//   POST /api/config         局部更新配置；仅 ssid/pass 变化才重启
//   GET  /api/calendar       读 SD 上的日历数据
//   POST /api/calendar       校验后写 SD，立即生效，不重启
//   GET  /api/scan           WiFi 扫描 {ok,aps:[{ssid,rssi,auth}]}
//   POST /api/weather_refresh  触发一次天气拉取
//   POST /api/city_refresh   触发一次「自动城市」定位（仅城市留空时有效）
//   GET  /api/pubip          最近一次 UAPI 定位结果（公网 IP / 归属地 / 运营商）
//   POST /api/reboot         重启
//   POST /api/forget         清 WiFi 凭据并重启
//   GET  /api/data/csv       CSV 数据（默认最新若干行；?full=1 为完整文件）
//   GET  /api/apistat        外部接口调用次数统计（?p=day|week|month|year）
//   POST /api/ota            上传固件做 OTA（见 net_ota.c），成功后自动重启
//   404 → 302 /              让手机的 Captive Portal 探测自动弹出本页
//
// 请求/响应统一 JSON。相比旧版的 x-www-form-urlencoded：
//   * 不再有"按编码后长度截断缓冲"的问题（旧版城市字段 32B 实际只能存 3 个汉字，
//     且会在 UTF-8 中间截断产生乱码）
//   * 配置值不再插进 HTML 属性，HTML 注入面消失
//   * 超长/非法输入回 400 并说明原因，不再静默存坏数据
//
// 前端资源见 portal_assets.h —— **自动生成**，改前端要改 components/net_bsp/portal/
// 下的源文件再跑 tools/gen_portal.py。资源已 gzip 预压缩：ESP32-S3 的 TCP 窗口只有
// 5760B，页面从 25KB 压到 9.5KB 是首屏最大的一笔优化，且解压在浏览器侧、设备 0 开销。

#include "net_internal.h"
#include "portal_assets.h"

#include "ui_model.h"
#include "ui_pages.h"
#include "ui_calendar.h"
#include "lvgl_bsp.h"
#include "user_config.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <nvs.h>

#define BODY_MAX        8192    // POST body 上限（超出回 413，不再截断）
#define CAL_TEXT_LIMIT  (UI_CAL_TEXT_MAX - 1)   // 单条文字最大字节数（不含结尾 0）

// =====================================================================
// JSON 输出（带容量检查，溢出置 ovf 由调用方处理，不静默截断）
// =====================================================================
typedef struct { char *buf; int cap; int len; bool ovf; } jw_t;

static void jw_putc(jw_t *w, char c)
{
    if (w->len + 1 >= w->cap) { w->ovf = true; return; }
    w->buf[w->len++] = c;
    w->buf[w->len] = 0;
}

static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    int room = w->cap - w->len;
    if (room <= 1) { w->ovf = true; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= room) { w->ovf = true; w->len = w->cap - 1; w->buf[w->len] = 0; }
    else w->len += n;
}

// 带引号的 JSON 字符串，转义 " \ 与控制字符
static void jw_qstr(jw_t *w, const char *s)
{
    jw_putc(w, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\')      { jw_putc(w, '\\'); jw_putc(w, (char) c); }
        else if (c == '\n')             { jw_putc(w, '\\'); jw_putc(w, 'n'); }
        else if (c == '\r')             { jw_putc(w, '\\'); jw_putc(w, 'r'); }
        else if (c == '\t')             { jw_putc(w, '\\'); jw_putc(w, 't'); }
        else if (c < 0x20)              { jw_fmt(w, "\\u%04x", c); }
        else                            { jw_putc(w, (char) c); }
    }
    jw_putc(w, '"');
}

// 写 key —— 除紧跟 { [ 外自动补逗号
static void jw_key(jw_t *w, const char *k)
{
    if (w->len > 0) {
        char last = w->buf[w->len - 1];
        if (last != '{' && last != '[') jw_putc(w, ',');
    }
    jw_qstr(w, k);
    jw_putc(w, ':');
}

static void jw_kv_str(jw_t *w, const char *k, const char *v) { jw_key(w, k); jw_qstr(w, v ? v : ""); }
static void jw_kv_int(jw_t *w, const char *k, long v)        { jw_key(w, k); jw_fmt(w, "%ld", v); }
static void jw_kv_bool(jw_t *w, const char *k, bool v)       { jw_key(w, k); jw_fmt(w, "%s", v ? "true" : "false"); }

// NaN / inf 在 JSON 里非法 —— 一律写 null，前端显示 "—"
static void jw_kv_f(jw_t *w, const char *k, float v, int dec)
{
    jw_key(w, k);
    if (isnan(v) || isinf(v)) jw_fmt(w, "null");
    else                      jw_fmt(w, "%.*f", dec, (double) v);
}

// 整型 + 哨兵：v <= na 视为"无数据"写 null（前端 `!=null` 判断后显示 "—"）。
// 否则 UI_TEMP_NA(-999) / UI_INT_NA(-1) 会被当成真实数值渲染出来。
static void jw_kv_int_na(jw_t *w, const char *k, long v, long na)
{
    jw_key(w, k);
    if (v <= na) jw_fmt(w, "null");
    else         jw_fmt(w, "%ld", v);
}

// 逗号分隔的元素前缀（数组内用）
static void jw_comma(jw_t *w)
{
    if (w->len > 0) {
        char last = w->buf[w->len - 1];
        if (last != '[' && last != '{') jw_putc(w, ',');
    }
}

// =====================================================================
// JSON 解析（极简，只覆盖门户前端发来的形态；IDF 6.0 已不含 json 组件）
// =====================================================================
static const char *js_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// p 指向开引号，返回闭引号之后；格式错返回 NULL
static const char *js_skip_string(const char *p)
{
    if (*p != '"') return NULL;
    for (p++; *p; p++) {
        if (*p == '\\') { if (!p[1]) return NULL; p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *js_skip_value(const char *p)
{
    p = js_ws(p);
    if (*p == '"') return js_skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = js_skip_string(p); if (!p) return NULL; continue; }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    // number / true / false / null
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p;
}

// 在对象里找成员，返回值起始位置；不存在返回 NULL。只扫本层，键不含转义。
static const char *js_member(const char *obj, const char *key)
{
    if (!obj) return NULL;
    obj = js_ws(obj);
    if (*obj != '{') return NULL;
    const char *p = obj + 1;
    size_t klen = strlen(key);
    for (;;) {
        p = js_ws(p);
        if (*p != '"') return NULL;              // '}' 或异常
        const char *ks = p + 1;
        const char *ke = js_skip_string(p);
        if (!ke) return NULL;
        bool match = ((size_t)(ke - 1 - ks) == klen && strncmp(ks, key, klen) == 0);
        p = js_ws(ke);
        if (*p != ':') return NULL;
        p = js_ws(p + 1);
        if (match) return p;
        p = js_skip_value(p);
        if (!p) return NULL;
        p = js_ws(p);
        if (*p != ',') return NULL;
        p++;
    }
}

// 解码 JSON 字符串到 out（UTF-8）。返回字节数；格式错或超出 cap 返回 -1。
// 返回 -1 时调用方回 400 并指明字段 —— 绝不截断后照样保存。
static int js_str(const char *p, char *out, size_t cap)
{
    if (!p) return -1;
    p = js_ws(p);
    if (*p != '"' || cap == 0) return -1;
    p++;
    size_t w = 0;
    while (*p != '"') {
        if (!*p) return -1;
        unsigned cp;
        if (*p != '\\') {
            if (w >= cap - 1) return -1;
            out[w++] = *p++;
            continue;
        }
        p++;
        switch (*p) {
            case 'n': cp = '\n'; p++; goto lit;
            case 'r': cp = '\r'; p++; goto lit;
            case 't': cp = '\t'; p++; goto lit;
            case 'b': cp = '\b'; p++; goto lit;
            case 'f': cp = '\f'; p++; goto lit;
            case '"': case '\\': case '/': cp = (unsigned char) *p++; goto lit;
            case 'u': {
                char h[5] = {0};
                for (int i = 0; i < 4; i++) { if (!p[1 + i]) return -1; h[i] = p[1 + i]; }
                cp = (unsigned) strtoul(h, NULL, 16);
                p += 5;
                // UTF-16 代理对（JSON.stringify 对 emoji 会这样编码）
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    char h2[5] = {0};
                    for (int i = 0; i < 4; i++) { if (!p[2 + i]) return -1; h2[i] = p[2 + i]; }
                    unsigned lo = (unsigned) strtoul(h2, NULL, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                break;
            }
            default: return -1;
        }
        // 按 UTF-8 编码码点
        {
            int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (w + need >= cap) return -1;
            if (need == 1) out[w++] = (char) cp;
            else if (need == 2) {
                out[w++] = (char) (0xC0 | (cp >> 6));
                out[w++] = (char) (0x80 | (cp & 0x3F));
            } else if (need == 3) {
                out[w++] = (char) (0xE0 | (cp >> 12));
                out[w++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                out[w++] = (char) (0x80 | (cp & 0x3F));
            } else {
                out[w++] = (char) (0xF0 | (cp >> 18));
                out[w++] = (char) (0x80 | ((cp >> 12) & 0x3F));
                out[w++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                out[w++] = (char) (0x80 | (cp & 0x3F));
            }
        }
        continue;
    lit:
        if (w >= cap - 1) return -1;
        out[w++] = (char) cp;
    }
    out[w] = 0;
    return (int) w;
}

// 数组迭代：*it 初始指向 '['，每次返回下个元素起始，结束返回 NULL
static const char *js_arr_next(const char **it)
{
    const char *p = js_ws(*it);
    if (*p == '[' || *p == ',') p = js_ws(p + 1);
    else return NULL;
    if (*p == ']' || !*p) return NULL;
    const char *end = js_skip_value(p);
    if (!end) return NULL;
    *it = js_ws(end);
    return p;
}

// =====================================================================
// 请求/响应辅助
// =====================================================================
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

// 错误回执：{"ok":false,"err":"中文原因"} —— 前端直接 toast 出来
// 非 static：net_ota.c 复用同一套回执格式（声明在 net_internal.h）
esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[256];
    jw_t w = { buf, sizeof(buf), 0, false };
    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok", false);
    jw_kv_str(&w, "err", msg);
    jw_putc(&w, '}');
    ESP_LOGW(NET_TAG, "portal %s: %s", status, msg);
    return send_json(req, status, buf);
}

esp_err_t send_ok(httpd_req_t *req)
{
    return send_json(req, "200 OK", "{\"ok\":true}");
}

// 按 content_len 完整读取 body。返回 malloc 的缓冲（调用方 free），失败返回 NULL
// 并已发出错误响应。旧版用固定 char[512] 读满即停，超长请求会静默丢字段。
static char *read_body(httpd_req_t *req, esp_err_t *out_err)
{
    *out_err = ESP_OK;
    int total = req->content_len;
    if (total <= 0)      { *out_err = send_err(req, "400 Bad Request", "请求体为空"); return NULL; }
    if (total > BODY_MAX) { *out_err = send_err(req, "413 Payload Too Large", "请求体过大"); return NULL; }

    char *body = (char *) malloc(total + 1);
    if (!body) { *out_err = send_err(req, "500 Internal Server Error", "设备内存不足"); return NULL; }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;   // 继续等
        if (r <= 0) {
            free(body);
            *out_err = send_err(req, "400 Bad Request", "请求体接收中断");
            return NULL;
        }
        got += r;
    }
    body[total] = 0;
    return body;
}

// 取可选字符串成员：不存在 → present=false；存在但非法/超长 → 返回 false
static bool opt_str(const char *root, const char *key, char *out, size_t cap, bool *present)
{
    const char *v = js_member(root, key);
    *present = (v != NULL);
    if (!v) { out[0] = 0; return true; }
    return js_str(v, out, cap) >= 0;
}

// =====================================================================
// 静态资源 —— 全部 gzip 预压缩，分块发送
//
// 为什么分块：一次 httpd_resp_send() 会让 httpd 试图把整段塞进 socket，而 TCP 窗口
// 只有 5760B；Chart.js 有 70KB，分块交给 lwIP 更平稳，也不用额外 RAM。
// 块大小取 4KB —— 大于窗口没有收益，小了又多花系统调用。
// =====================================================================
#define ASSET_CHUNK_SZ  4096

static esp_err_t send_asset_gz(httpd_req_t *req, const char *type,
                               const uint8_t *data, size_t len)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // 资源随固件走，版本内不变，但**不能**像上一版那样无条件 max-age=86400 缓存：
    // 那会让烧了新固件的浏览器 24 小时内继续用旧页面。ETag 用资源长度，固件一改
    // 长度必变；配 no-cache 让浏览器每次带 If-None-Match 来问一句，没变就回 304，
    // 省掉整段传输，又不会拿到过期页面。
    char etag[24];
    snprintf(etag, sizeof(etag), "\"%x\"", (unsigned) len);
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char inm[32];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK
        && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    for (size_t sent = 0; sent < len; ) {
        size_t n = len - sent;
        if (n > ASSET_CHUNK_SZ) n = ASSET_CHUNK_SZ;
        if (httpd_resp_send_chunk(req, (const char *) data + sent, n) != ESP_OK)
            return ESP_FAIL;
        sent += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_asset_gz(req, "text/html; charset=utf-8",
                         PORTAL_INDEX_GZ, PORTAL_INDEX_GZ_LEN);
}

static esp_err_t chart_js_get(httpd_req_t *req)
{
    return send_asset_gz(req, "application/javascript; charset=utf-8",
                         PORTAL_CHART_GZ, PORTAL_CHART_GZ_LEN);
}

// Captive Portal：手机探测的任意未知路径都 302 回首页，系统就会自动弹出配置页
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void) err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// =====================================================================
// GET /api/status
// =====================================================================
static esp_err_t status_get(httpd_req_t *req)
{
    const ui_model_t *m = ui_model_get();
    char buf[1280];
    jw_t w = { buf, sizeof(buf), 0, false };

    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok", true);
    // 网络
    jw_kv_bool(&w, "wifi", m->wifi_connected);
    jw_kv_int (&w, "rssi", m->wifi_rssi);
    jw_kv_str (&w, "ip",   m->ip);
    jw_kv_str (&w, "ssid", m->ssid);
    jw_kv_bool(&w, "ap",   m->ap_active);
    // 室内传感器
    jw_kv_f   (&w, "temp", m->indoor_temp, 1);
    jw_kv_f   (&w, "humi", m->indoor_humi, 0);
    // 电池
    jw_kv_int (&w, "batt", m->battery_percent);
    jw_kv_bool(&w, "charging", m->battery_charging);
    // 天气
    jw_kv_str (&w, "city",  m->city);
    jw_kv_str (&w, "wtext", m->weather_text);
    jw_kv_str (&w, "wupd",  m->weather_update);
    jw_kv_f   (&w, "otemp", m->outdoor_temp, 1);
    jw_kv_f   (&w, "ohumi", m->outdoor_humi, 0);
    jw_kv_f   (&w, "feels", m->feels_like_temp, 1);
    jw_kv_int_na(&w, "tmin", m->temp_min, UI_TEMP_NA);
    jw_kv_int_na(&w, "tmax", m->temp_max, UI_TEMP_NA);
    {
        // 风向、风速各自可能缺失：风速为 NaN 时只写风向，别渲染成 "东风 nan km/h"
        char wind[32];
        wind[0] = 0;
        if (m->wind_dir[0] && !isnan(m->wind_speed_kmh))
            snprintf(wind, sizeof(wind), "%s %.0f km/h", m->wind_dir, (double) m->wind_speed_kmh);
        else if (m->wind_dir[0])
            snprintf(wind, sizeof(wind), "%s", m->wind_dir);
        jw_kv_str(&w, "wind", wind);
    }
    // SD
    jw_kv_bool(&w, "sd",       m->sd_mounted);
    jw_kv_int (&w, "sd_used",  m->sd_used_mb);
    jw_kv_int (&w, "sd_total", m->sd_total_mb);
    // 设备
    jw_kv_int (&w, "uptime",     m->uptime_sec);
    jw_kv_int (&w, "heap",       m->free_heap_kb);
    jw_kv_int (&w, "flash_free", m->flash_free_kb);
    jw_kv_str (&w, "chip", m->chip_model);
    jw_kv_str (&w, "app",  m->app_ver);
    jw_kv_str (&w, "idf",  m->idf_ver);
    jw_kv_str (&w, "mac",  m->mac);
    jw_putc(&w, '}');

    if (w.ovf) return send_err(req, "500 Internal Server Error", "状态数据过长");
    return send_json(req, "200 OK", buf);
}

// =====================================================================
// GET /api/limits —— 容量上限，前端据此禁用"添加"并提示
// =====================================================================
static esp_err_t limits_get(httpd_req_t *req)
{
    char buf[128];
    jw_t w = { buf, sizeof(buf), 0, false };
    jw_putc(&w, '{');
    jw_kv_int(&w, "marks",  UI_CAL_MAX_MARKS);
    jw_kv_int(&w, "events", UI_CAL_MAX_EVENTS);
    jw_kv_int(&w, "labels", UI_CAL_MAX_LABELS);
    jw_kv_int(&w, "text",   CAL_TEXT_LIMIT);
    jw_putc(&w, '}');
    return send_json(req, "200 OK", buf);
}

// =====================================================================
// GET /api/config —— 敏感字段（pass/key/host）只回布尔，绝不回明文
// =====================================================================
static esp_err_t config_get(httpd_req_t *req)
{
    char buf[384];
    jw_t w = { buf, sizeof(buf), 0, false };
    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok", true);
    jw_kv_str (&w, "ssid", s_cfg.ssid);
    jw_kv_str (&w, "city", s_cfg.city);
    // 密码 / API Key / API Host 全都是敏感字段，不回明文
    // 前端会根据布尔值显示 placeholder，改这些字段时必须完整重填
    jw_kv_bool(&w, "has_pass", s_cfg.pass[0] != 0);
    jw_kv_bool(&w, "has_key",  s_cfg.weather_apikey[0] != 0);
    jw_kv_bool(&w, "has_host", s_cfg.weather_host[0] != 0);
    jw_kv_bool(&w, "has_uapi", s_cfg.uapi_key[0] != 0);
    jw_putc(&w, '}');
    if (w.ovf) return send_err(req, "500 Internal Server Error", "配置数据过长");
    return send_json(req, "200 OK", buf);
}

// =====================================================================
// POST /api/config —— 局部更新。只有 ssid/pass 变化才需要重启；
// 城市/Host/Key 立即生效（weather_task 每轮重读 s_cfg + city dirty 标志）。
// 密码与 Key 留空 = 保持原值，这样前端不必回显明文也能改其他字段。
// =====================================================================
static esp_err_t config_post(httpd_req_t *req)
{
    esp_err_t e;
    char *body = read_body(req, &e);
    if (!body) return e;

    net_config_t c = s_cfg;      // 以当前配置为基线做局部更新
    char v[128];
    bool has;
    bool net_changed = false, wx_changed = false, city_changed = false;

    // --- SSID ---
    if (!opt_str(body, "ssid", v, sizeof(c.ssid), &has)) {
        free(body); return send_err(req, "400 Bad Request", "WiFi 名称过长（最多 32 字节）");
    }
    if (has) {
        if (!v[0]) { free(body); return send_err(req, "400 Bad Request", "WiFi 名称不能为空"); }
        if (strcmp(c.ssid, v) != 0) { strcpy(c.ssid, v); net_changed = true; }
    }
    // --- 密码：留空 = 不修改 ---
    if (!opt_str(body, "pass", v, sizeof(c.pass), &has)) {
        free(body); return send_err(req, "400 Bad Request", "WiFi 密码过长（最多 64 字节）");
    }
    if (has && v[0]) {
        if (strcmp(c.pass, v) != 0) { strcpy(c.pass, v); net_changed = true; }
    }
    // --- 城市：**允许清空** —— 空 = 自动定位（UAPI 取公网 IP 的 district） ---
    if (!opt_str(body, "city", v, sizeof(c.city), &has)) {
        free(body); return send_err(req, "400 Bad Request", "城市名称过长（最多 31 字节，约 10 个汉字）");
    }
    if (has && strcmp(c.city, v) != 0) {
        strcpy(c.city, v); wx_changed = true; city_changed = true;
    }
    // --- API Host：留空 = 不修改 ---
    // 前端 f_host 和 apikey 一样是掩码字段（只回 has_host 布尔，不回明文），只改城市时
    // 提交的就是空串。这里若不挡住空串，一次"只改城市"的保存会把 Host 清成空，
    // weather_task 开头的 `!s_cfg.weather_host[0]` 直接跳过 —— 天气就再也不更新了。
    if (!opt_str(body, "host", v, sizeof(c.weather_host), &has)) {
        free(body); return send_err(req, "400 Bad Request", "API Host 过长（最多 63 字节）");
    }
    if (has && v[0] && strcmp(c.weather_host, v) != 0) {
        strcpy(c.weather_host, v); wx_changed = true;
    }
    // --- API Key：留空 = 不修改 ---
    if (!opt_str(body, "apikey", v, sizeof(c.weather_apikey), &has)) {
        free(body); return send_err(req, "400 Bad Request", "API Key 过长（最多 63 字节）");
    }
    if (has && v[0] && strcmp(c.weather_apikey, v) != 0) {
        strcpy(c.weather_apikey, v); wx_changed = true;
    }
    // --- UAPI Key（自动定位用）：留空 = 不修改；文档要求形如 "uapi-xxx" ---
    if (!opt_str(body, "uapikey", v, sizeof(c.uapi_key), &has)) {
        free(body); return send_err(req, "400 Bad Request", "UAPI Key 过长（最多 63 字节）");
    }
    if (has && v[0]) {
        if (strncmp(v, "uapi-", 5) != 0) {
            free(body);
            return send_err(req, "400 Bad Request", "UAPI Key 格式不正确，应以 uapi- 开头");
        }
        if (strcmp(c.uapi_key, v) != 0) { strcpy(c.uapi_key, v); wx_changed = true; }
    }
    free(body);

    if (!net_changed && !wx_changed) return send_ok(req);

    if (!NetBsp_SaveConfig(&c)) return send_err(req, "500 Internal Server Error", "写入 NVS 失败");
    s_cfg = c;                                  // 内存里的配置同步，天气任务下轮即读到
    if (city_changed) s_weather_city_dirty = true;

    ESP_LOGI(NET_TAG, "config saved: ssid='%s' city='%s' host='%s' key=%s (net=%d wx=%d)",
             c.ssid, c.city, c.weather_host,
             c.weather_apikey[0] ? "set" : "EMPTY", (int) net_changed, (int) wx_changed);

    if (net_changed) {
        send_ok(req);
        ESP_LOGW(NET_TAG, "WiFi 配置已变更 → 重启");
        vTaskDelay(pdMS_TO_TICKS(1200));        // 让响应发完
        esp_restart();
        return ESP_OK;
    }
    // 天气配置改动无需重启 —— 顺手 kick 一次立即拉取
    NetBsp_TriggerWeatherFetch();
    return send_ok(req);
}

// 拷贝 [src, src+len) 到 dst，超出 cap 时**按 UTF-8 字符边界**回退。
// SD 上的旧文件可能有超长文字，若截在多字节字符中间会产生非法 UTF-8，
// 整个 /api/calendar 响应就 JSON.parse 失败 → 页面全白。
static void copy_utf8(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) {
        len = cap - 1;
        // 退到首字节（非 10xxxxxx）为止，丢掉不完整的尾字符
        while (len > 0 && ((unsigned char) src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

// =====================================================================
// GET /api/calendar —— 把 SD 上的三段字符串转成 JSON 数组
// =====================================================================
static esp_err_t calendar_get(httpd_req_t *req)
{
    static char cm[CAL_MARKS_BUF], ce[CAL_EVENTS_BUF], cl[CAL_LABELS_BUF];
    cal_read_file(cm, sizeof(cm), ce, sizeof(ce), cl, sizeof(cl));

    int cap = 3072;
    char *out = (char *) malloc(cap);
    if (!out) return send_err(req, "500 Internal Server Error", "设备内存不足");
    jw_t w = { out, cap, 0, false };

    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok", true);
    jw_kv_bool(&w, "sd", ui_model_get()->sd_mounted);

    // marks: "MM-DD,MM-DD,..."
    jw_key(&w, "marks");
    jw_putc(&w, '[');
    for (const char *p = cm; *p; ) {
        const char *sep = strchr(p, ',');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len) {
            char t[16];
            copy_utf8(t, sizeof(t), p, len);
            jw_comma(&w);
            jw_qstr(&w, t);
        }
        if (!sep) break;
        p = sep + 1;
    }
    jw_putc(&w, ']');

    // events: "MM-DD=内容;..."
    jw_key(&w, "events");
    jw_putc(&w, '[');
    for (const char *p = ce; *p; ) {
        const char *sep = strchr(p, ';');
        const char *stop = sep ? sep : (p + strlen(p));
        const char *eq = memchr(p, '=', (size_t)(stop - p));
        if (eq) {
            char d[16], t[UI_CAL_TEXT_MAX];
            copy_utf8(d, sizeof(d), p, (size_t)(eq - p));
            copy_utf8(t, sizeof(t), eq + 1, (size_t)(stop - eq - 1));
            jw_comma(&w);
            jw_putc(&w, '{');
            jw_kv_str(&w, "date", d);
            jw_kv_str(&w, "text", t);
            jw_putc(&w, '}');
        }
        if (!sep) break;
        p = sep + 1;
    }
    jw_putc(&w, ']');

    // labels: "文字;文字;..."
    jw_key(&w, "labels");
    jw_putc(&w, '[');
    for (const char *p = cl; *p; ) {
        const char *sep = strchr(p, ';');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len) {
            char t[UI_CAL_TEXT_MAX];
            copy_utf8(t, sizeof(t), p, len);
            jw_comma(&w);
            jw_qstr(&w, t);
        }
        if (!sep) break;
        p = sep + 1;
    }
    jw_putc(&w, ']');
    jw_putc(&w, '}');

    esp_err_t r = w.ovf ? send_err(req, "500 Internal Server Error", "日历数据过长")
                        : send_json(req, "200 OK", out);
    free(out);
    return r;
}

// "MM-DD" 校验
static bool valid_mmdd(const char *s)
{
    if (strlen(s) != 5 || s[2] != '-') return false;
    for (int i = 0; i < 5; i++) if (i != 2 && (s[i] < '0' || s[i] > '9')) return false;
    int m = (s[0] - '0') * 10 + (s[1] - '0');
    int d = (s[3] - '0') * 10 + (s[4] - '0');
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// 文字校验：分隔符会破坏 SD 的按行 / 分号 / 等号编码，必须拒绝而不是存坏
static const char *check_text(const char *t)
{
    if (!t[0]) return "内容不能为空";
    if (strpbrk(t, ";=,\r\n")) return "内容不能包含分号、等号、逗号或换行符";
    if (strlen(t) > CAL_TEXT_LIMIT) return "内容过长";
    return NULL;
}

// =====================================================================
// POST /api/calendar —— 校验后写 SD 并立即生效
// 超出 UI 容量 / 含非法字符 → 400 并说明是哪一条，不再"存了但设备端丢弃"
// =====================================================================
static esp_err_t calendar_post(httpd_req_t *req)
{
    if (!ui_model_get()->sd_mounted)
        return send_err(req, "409 Conflict", "未检测到 SD 卡，无法保存日历数据");

    esp_err_t e;
    char *body = read_body(req, &e);
    if (!body) return e;

    static char marks[CAL_MARKS_BUF], events[CAL_EVENTS_BUF], labels[CAL_LABELS_BUF];
    marks[0] = events[0] = labels[0] = 0;
    char msg[128];

#define FAIL(...) do { snprintf(msg, sizeof(msg), __VA_ARGS__); free(body); \
                       return send_err(req, "400 Bad Request", msg); } while (0)

    // --- marks ---
    const char *arr = js_member(body, "marks");
    if (arr) {
        const char *it = arr;
        int n = 0;
        for (const char *el; (el = js_arr_next(&it)) != NULL; ) {
            char d[16];
            if (js_str(el, d, sizeof(d)) < 0 || !valid_mmdd(d)) FAIL("第 %d 个标记日期格式不正确", n + 1);
            if (++n > UI_CAL_MAX_MARKS) FAIL("标记日期最多 %d 条，请先删除部分条目", UI_CAL_MAX_MARKS);
            if (marks[0]) strcat(marks, ",");
            strcat(marks, d);
        }
    }
    // --- events ---
    arr = js_member(body, "events");
    if (arr) {
        const char *it = arr;
        int n = 0;
        for (const char *el; (el = js_arr_next(&it)) != NULL; ) {
            char d[16], t[UI_CAL_TEXT_MAX];
            if (js_str(js_member(el, "date"), d, sizeof(d)) < 0 || !valid_mmdd(d))
                FAIL("第 %d 条事项的日期格式不正确", n + 1);
            if (js_str(js_member(el, "text"), t, sizeof(t)) < 0)
                FAIL("第 %d 条事项的内容过长（最多 %d 字节）", n + 1, CAL_TEXT_LIMIT);
            const char *bad = check_text(t);
            if (bad) FAIL("第 %d 条事项：%s", n + 1, bad);
            if (++n > UI_CAL_MAX_EVENTS) FAIL("事项内容最多 %d 条，请先删除部分条目", UI_CAL_MAX_EVENTS);
            if (events[0]) strcat(events, ";");
            strcat(events, d); strcat(events, "="); strcat(events, t);
        }
    }
    // --- labels ---
    arr = js_member(body, "labels");
    if (arr) {
        const char *it = arr;
        int n = 0;
        for (const char *el; (el = js_arr_next(&it)) != NULL; ) {
            char t[UI_CAL_TEXT_MAX];
            if (js_str(el, t, sizeof(t)) < 0)
                FAIL("第 %d 个标签过长（最多 %d 字节）", n + 1, CAL_TEXT_LIMIT);
            const char *bad = check_text(t);
            if (bad) FAIL("第 %d 个标签：%s", n + 1, bad);
            if (++n > UI_CAL_MAX_LABELS) FAIL("随机标签最多 %d 条，请先删除部分条目", UI_CAL_MAX_LABELS);
            if (labels[0]) strcat(labels, ";");
            strcat(labels, t);
        }
    }
#undef FAIL
    free(body);

    if (!cal_save_to_sd(marks, events, labels))
        return send_err(req, "500 Internal Server Error", "写入 SD 卡失败，请检查卡片剩余空间及写保护状态");

    // setter 只写静态数组、不碰 LVGL，无需持锁；锁只保护 apply。
    // 锁超时也不影响：数组已更新，1s tick 会用新数据重绘。
    ui_calendar_set_marks(marks);
    ui_calendar_set_events(events);
    ui_calendar_set_labels(labels);
    if (Lvgl_lock(200)) {
        ui_pages_apply_locked();
        Lvgl_unlock();
    }
    ESP_LOGI(NET_TAG, "cal saved marks='%s' events='%s' labels='%s'", marks, events, labels);
    return send_ok(req);
}

// =====================================================================
// GET /api/scan —— {ok:true,aps:[{ssid,rssi,auth}]}；失败带原因，前端能区分
// "附近没有网络" 和 "WiFi 忙 / 未初始化"
//
// 正在做 web 扫描时置 s_scanning —— wifi_evt 的 disconnect 处理里查询它决定
// 要不要抢占 esp_wifi_connect()。
// =====================================================================
static esp_err_t scan_get(httpd_req_t *req)
{
    if (!s_wifi_common_inited)
        return send_err(req, "503 Service Unavailable", "WiFi 尚未初始化，请稍后重试");

    // 一次一个客户端扫描
    if (xSemaphoreTake(s_scan_mux, pdMS_TO_TICKS(1000)) != pdTRUE)
        return send_err(req, "503 Service Unavailable", "已有扫描在进行中，请稍后重试");

    // 关键：STA 处在 CONNECTING（自动重连）状态时，esp_wifi_scan_start 会立刻返回
    // ESP_ERR_WIFI_STATE 拒绝扫描 —— 得先 disconnect 让状态机回到 IDLE。
    // 但如果 STA 已经 CONNECTED（用户正是通过 STA IP 访问这个配网页），disconnect
    // 会瞬间掐断 HTTP 会话，浏览器就报 "TypeError: Failed to fetch"。
    // 判据：esp_wifi_sta_get_ap_info() == ESP_OK 表示 STA 已关联 —— 此时不要断，
    // 直接扫（芯片支持 connected + scan，会短暂跳信道，TCP 自动恢复）。
    wifi_ap_record_t cur_ap;
    bool sta_online = (esp_wifi_sta_get_ap_info(&cur_ap) == ESP_OK);
    s_scanning = true;
    if (!sta_online) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));   // 等 STA 从 CONNECTING 掉到 IDLE
    }

    // ALL_CHANNEL_SCAN 主动扫可以看到隐藏之外的绝大多数 AP
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 120;
    sc.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&sc, true);   // blocking
    uint16_t n = 0;
    wifi_ap_record_t *recs = NULL;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&n);
        if (n > 24) n = 24;                            // 前 24 个够用
        recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        if (recs) esp_wifi_scan_get_ap_records(&n, recs);
        else      n = 0;
    }
    // 扫描结束 —— 若之前我们主动断开过 STA，这里恢复自动重连
    s_scanning = false;
    if (s_want_sta && !sta_online) esp_wifi_connect();
    xSemaphoreGive(s_scan_mux);

    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "scan_start fail: %s (0x%x)", esp_err_to_name(err), (unsigned) err);
        free(recs);
        return send_err(req, "503 Service Unavailable", "WiFi 忙，扫描未能启动，请稍后重试");
    }
    if (!recs) return send_err(req, "500 Internal Server Error", "设备内存不足");
    ESP_LOGI(NET_TAG, "scan done: %u APs", (unsigned) n);

    int cap = 4096;
    char *out = (char *) malloc(cap);
    if (!out) { free(recs); return send_err(req, "500 Internal Server Error", "设备内存不足"); }
    jw_t w = { out, cap, 0, false };
    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok", true);
    jw_key(&w, "aps");
    jw_putc(&w, '[');
    for (int i = 0; i < n && !w.ovf; i++) {
        if (recs[i].ssid[0] == 0) continue;                       // 隐藏 SSID
        bool dup = false;                                        // 部分路由多信道重复回应
        for (int j = 0; j < i; j++) {
            if (strcmp((const char *) recs[j].ssid, (const char *) recs[i].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        jw_comma(&w);
        jw_putc(&w, '{');
        jw_kv_str (&w, "ssid", (const char *) recs[i].ssid);
        jw_kv_int (&w, "rssi", recs[i].rssi);
        jw_kv_bool(&w, "auth", recs[i].authmode != WIFI_AUTH_OPEN);
        jw_putc(&w, '}');
    }
    jw_putc(&w, ']');
    jw_putc(&w, '}');
    free(recs);

    // 溢出就少报几个，也别发半截 JSON —— 但 24 条 × ~70B 远小于 4KB，实际到不了
    esp_err_t r = w.ovf ? send_err(req, "500 Internal Server Error", "扫描结果过长")
                        : send_json(req, "200 OK", out);
    free(out);
    return r;
}

// =====================================================================
// POST /api/weather_refresh · /api/reboot · /api/forget
// =====================================================================
static esp_err_t weather_refresh_post(httpd_req_t *req)
{
    NetBsp_TriggerWeatherFetch();
    ESP_LOGI(NET_TAG, "portal: 手动触发天气拉取");
    return send_ok(req);
}

// POST /api/city_refresh —— 手动重跑「自动城市」定位（UAPI /network/myip）。
// 只在城市留空时有意义：用户手填了城市就以手填为准，自动定位不参与，
// 这里直接回 409 让前端提示，而不是白白消耗一次 API 配额。
static esp_err_t city_refresh_post(httpd_req_t *req)
{
    if (s_cfg.city[0]) {
        return send_err(req, "409 Conflict", "请先清空城市再重新定位");
    }
    NetBsp_TriggerCityRefresh();
    ESP_LOGI(NET_TAG, "portal: 手动触发自动城市定位");
    return send_ok(req);
}

// GET /api/pubip —— 最近一次 UAPI 定位结果（公网 IP / 归属地 / 运营商）。
// 尚未成功拉过时回 ok:true + valid:false，前端显示 "—"（不是错误，只是还没拿到）。
static esp_err_t pubip_get(httpd_req_t *req)
{
    uapi_myip_t info;
    bool valid = NetBsp_GetPublicIp(&info);

    char buf[512];
    jw_t w = { buf, sizeof(buf), 0, false };
    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok",    true);
    jw_kv_bool(&w, "valid", valid);
    jw_kv_str (&w, "ip",       valid ? info.ip       : "");
    jw_kv_str (&w, "region",   valid ? info.region   : "");
    jw_kv_str (&w, "isp",      valid ? info.isp      : "");
    jw_kv_str (&w, "district", valid ? info.district : "");
    // auto=true 表示当前天气城市来自自动定位（用户没手填）
    jw_kv_bool(&w, "auto", s_cfg.city[0] == 0);
    jw_kv_str (&w, "city", valid ? info.city : "");
    jw_putc(&w, '}');

    if (w.ovf) return send_err(req, "500 Internal Server Error", "公网 IP 数据过长");
    return send_json(req, "200 OK", buf);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    send_ok(req);
    ESP_LOGW(NET_TAG, "portal: 用户请求重启");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t forget_post(httpd_req_t *req)
{
    NetBsp_ForgetWifi();
    send_ok(req);
    ESP_LOGW(NET_TAG, "portal: 用户清除 WiFi 凭据 → 重启进配网模式");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

// =====================================================================
// GET /api/data/csv —— 默认只回最新 500 行（给图表用）；
// GET /api/data/csv?full=1 —— 整份文件原样回传（给"下载全部数据"用）。
//
// 分两种模式的原因：图表只画得下几百个点，全量下来白花 TCP 时间；而下载是
// 要存档的，截断 500 行等于**悄悄丢数据** —— 10 分钟一行，500 行只有约 3.5 天，
// 之前"下载的数据不全"就是这么来的（前端下载按钮复用了图表那个截断接口）。
//
// 定位起点用**块倒读**：每次往前读 1KB 数一遍换行符。旧实现是每字节一次
// fseek+fgetc，500 行（约 35KB）要 3.5 万次 FATFS 调用，SD 上要好几秒。
// 现在同样的活儿只要 ~35 次读，快两个数量级。full=1 模式不用倒读，直接从头发。
//
// 缓冲全部放在**静态区**而不是任务栈：httpd 任务栈只有 8KB，原先在栈上开
// 8KB 数组必然溢栈。发送块取 1460B = 一个 TCP MSS，比 8KB 更贴合 5760B 窗口。
// 同一时刻只有一个 httpd 任务跑 handler，静态缓冲不存在竞争。
// =====================================================================
#define MAX_CSV_ROWS   500
#define CSV_SCAN_SZ    1024    // 倒着定位行首用的块
#define CSV_SEND_SZ    1460    // 一个以太网 MSS
#define CSV_LOG_PATH   "/sdcard/rlcd/weather_log.csv"   // 与 user_app.c 的写入路径一致

// URL query 里有 full=1 / full=true 就算全量
static bool csv_want_full(httpd_req_t *req)
{
    char q[64], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, "full", v, sizeof(v)) != ESP_OK) return false;
    return v[0] == '1' || v[0] == 't' || v[0] == 'y';
}

static esp_err_t csv_get(httpd_req_t *req)
{
    const ui_model_t *m = ui_model_get();
    if (!m->sd_mounted)
        return send_err(req, "409 Conflict", "SD 卡未挂载，无可用数据");

    const bool full = csv_want_full(req);

    FILE *f = fopen(CSV_LOG_PATH, "rb");
    if (!f) return send_err(req, "404 Not Found", "暂无数据");

    static char scan[CSV_SCAN_SZ];

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return send_err(req, "500 Internal Server Error", "读取失败"); }
    long size = ftell(f);
    if (size < 0) { fclose(f); return send_err(req, "500 Internal Server Error", "读取失败"); }

    // 全量模式：pos 停在 0，整份文件（含原始表头）原样发出，不做任何截断。
    // 截断模式：从尾部往前，一块一块数换行符，数到第 MAX_CSV_ROWS 个就是起点。
    long pos  = size;
    int  rows = 0;
    if (full) {
        pos = 0;
    } else {
        while (pos > 0 && rows < MAX_CSV_ROWS) {
            long chunk = (pos > CSV_SCAN_SZ) ? CSV_SCAN_SZ : pos;
            pos -= chunk;
            if (fseek(f, pos, SEEK_SET) != 0) break;
            size_t got = fread(scan, 1, (size_t) chunk, f);
            // 块内从后往前找，命中第 MAX_CSV_ROWS 个换行就把 pos 定在它之后
            for (long i = (long) got - 1; i >= 0; i--) {
                if (scan[i] != '\n') continue;
                if (++rows >= MAX_CSV_ROWS) { pos += i + 1; break; }
            }
        }
    }
    // pos==0 表示整个文件都要发（含 header 行）；否则 pos 落在某行行首
    if (fseek(f, pos, SEEK_SET) != 0) { fclose(f); return send_err(req, "500 Internal Server Error", "读取失败"); }

    // 不是从文件头发的话，前端 parseCsvStream 会 shift() 掉第一行当 header ——
    // 这里补一行真正的表头，否则最旧的一条数据会被前端丢掉。
    char disp[128];
    struct tm ti; time_t now = time(NULL);
    localtime_r(&now, &ti);
    snprintf(disp, sizeof(disp), "attachment; filename=\"rlcd_weather_%04d-%02d-%02d.csv\"",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t r = ESP_OK;
    if (pos > 0) {
        static const char hdr[] = "timestamp,indoor_temp,indoor_humi,outdoor_temp,"
                                  "outdoor_humi,weather,city,wifi_rssi\n";
        if (httpd_resp_send_chunk(req, hdr, sizeof(hdr) - 1) != ESP_OK) r = ESP_FAIL;
    }

    // 只发**进函数时**量到的那 (size - pos) 个字节，不是一路 fread 到 EOF。
    //
    // 全量导出可能要几秒到几十秒（一年数据约 3.6MB），期间 csv_log 任务完全
    // 可能追加新行（10 分钟一次）。读到 EOF 的写法会把这些新数据也带上，
    // 让响应长度对不上一开始的判断；更麻烦的是文件持续增长时理论上收不了尾。
    // 锁定初始长度，导出内容 = 一个明确的时间切片。
    long remain = size - pos;
    static char send[CSV_SEND_SZ];
    const int64_t t0 = esp_timer_get_time();
    while (r == ESP_OK && remain > 0) {
        size_t want = (remain > (long) sizeof(send)) ? sizeof(send) : (size_t) remain;
        size_t nr   = fread(send, 1, want, f);
        if (nr == 0) break;   // 文件被截断/读错，能发多少算多少
        if (httpd_resp_send_chunk(req, send, nr) != ESP_OK) {
            r = ESP_FAIL;
            break;
        }
        remain -= (long) nr;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    // 全量导出会独占 httpd 那唯一一条任务（select 循环是串行的），期间门户其它
    // 请求排队等着。把耗时打出来，真出现"点了导出网页就卡住"时能直接对上。
    ESP_LOGI(NET_TAG, "csv: %s (from offset %ld/%ld, rows=%d, %lld ms)",
             full ? "sent full file" : "sent latest rows", pos, size, rows,
             (long long) ((esp_timer_get_time() - t0) / 1000));
    return r;
}

// =====================================================================
// GET /api/apistat?p=day|week|month|year
// —— 外部接口调用次数（明细存 SD，见 net_apistat.c）。只回次数，不回时间序列：
//    前端就是一行一个接口显示数字，不画图。
// =====================================================================
static esp_err_t apistat_get(httpd_req_t *req)
{
    api_period_t p = API_PERIOD_MONTH;   // 默认本月，与前端 <select> 的 selected 一致
    char q[48], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "p", v, sizeof(v)) == ESP_OK) {
        if      (strcmp(v, "day")   == 0) p = API_PERIOD_DAY;
        else if (strcmp(v, "week")  == 0) p = API_PERIOD_WEEK;
        else if (strcmp(v, "year")  == 0) p = API_PERIOD_YEAR;
        else if (strcmp(v, "month") == 0) p = API_PERIOD_MONTH;
        else return send_err(req, "400 Bad Request", "时间范围参数不正确");
    }

    api_stat_t st;
    const bool ok = NetBsp_ApiStatQuery(p, &st);

    // 4 个接口 × 约 90B + 外壳，512 够；加接口时这里要跟着放大
    char buf[512];
    jw_t w = { buf, sizeof(buf), 0, false };
    jw_putc(&w, '{');
    jw_kv_bool(&w, "ok", true);
    // valid=false 表示统计不可用（无 SD 或系统时间未校准），前端给出对应提示
    jw_kv_bool(&w, "valid", ok);
    jw_kv_bool(&w, "sd", ui_model_get()->sd_mounted);
    if (ok) {
        char from[16];
        snprintf(from, sizeof(from), "%04d-%02d-%02d", st.from_year, st.from_month, st.from_day);
        jw_kv_str(&w, "from", from);
    }
    jw_key(&w, "items");
    jw_putc(&w, '[');
    unsigned total = 0;
    for (int i = 0; i < API_CALL_COUNT; i++) {
        jw_comma(&w);
        jw_putc(&w, '{');
        jw_kv_str(&w, "name", NetBsp_ApiCallName((api_call_id_t) i));
        jw_kv_int(&w, "n",    st.count[i]);
        jw_kv_int(&w, "fail", st.fail[i]);
        jw_putc(&w, '}');
        total += st.count[i];
    }
    jw_putc(&w, ']');
    jw_kv_int(&w, "total", total);
    jw_putc(&w, '}');

    if (w.ovf) return send_err(req, "500 Internal Server Error", "统计数据过长");
    return send_json(req, "200 OK", buf);
}

// =====================================================================
void config_httpd_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 20;   // 当前 17 条，留几个余量（超了会静默注册失败）
    cfg.stack_size        = 8 * 1024;   // JSON 生成 + snprintf 需要点栈
    // scan 阻塞 ~2-3s，把发送/接收 timeout 拉长避免浏览器提前断连
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) return;

    static const httpd_uri_t routes[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = root_get },
        { .uri = "/chart.umd.min.js",    .method = HTTP_GET,  .handler = chart_js_get },
        { .uri = "/api/status",          .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/limits",          .method = HTTP_GET,  .handler = limits_get },
        { .uri = "/api/config",          .method = HTTP_GET,  .handler = config_get },
        { .uri = "/api/config",          .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/calendar",        .method = HTTP_GET,  .handler = calendar_get },
        { .uri = "/api/calendar",        .method = HTTP_POST, .handler = calendar_post },
        { .uri = "/api/scan",            .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/weather_refresh", .method = HTTP_POST, .handler = weather_refresh_post },
        { .uri = "/api/city_refresh",    .method = HTTP_POST, .handler = city_refresh_post },
        { .uri = "/api/pubip",           .method = HTTP_GET,  .handler = pubip_get },
        { .uri = "/api/reboot",          .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/forget",          .method = HTTP_POST, .handler = forget_post },
        { .uri = "/api/data/csv",        .method = HTTP_GET,  .handler = csv_get },
        { .uri = "/api/apistat",         .method = HTTP_GET,  .handler = apistat_get },
        { .uri = "/api/ota",             .method = HTTP_POST, .handler = ota_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_httpd, &routes[i]);
    }
    // 手机的 Captive Portal 探测路径（/generate_204、/hotspot-detect.html 等）
    // 统一 302 回首页，系统就会自动弹出配置页
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, not_found);
}
