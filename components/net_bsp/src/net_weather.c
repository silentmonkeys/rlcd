// net_weather.c —— QWeather API 拉取
//
// 设计原则：
//   1. 所有缓冲局部分配，函数返回前 free —— 无 static state，无跨调用累积。
//   2. gzip 用 espressif/zlib（inflateInit2 + inflate + inflateEnd），
//      每次调用完整生命周期；zlib 是 mbedtls/lwip 兄弟组件，久经考验。
//   3. HTTP 用 esp_http_client_open / _fetch_headers / _read 流式读，
//      不用 event handler —— 数据直接读到本地缓冲。
//   4. 任一步失败立即返回，上层 weather_task 决定重试节奏。
//   5. Now/Daily 两个端点串行拉，Now 是主数据（决定是否显示），
//      Daily 失败只警告不阻塞。

#include "net_internal.h"
#include "ui_model.h"
#include "ui_home.h"
#include "lvgl_bsp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>

// gzip 解压 —— QWeather 强制返回 gzip；esp_http_client 不自动解压。
#include "zlib.h"

#define WX_HTTP_TIMEOUT_MS   15000
#define WX_HTTP_MAX_BYTES    16384       // 单次响应上限（未压缩）
#define WX_GZIP_MAX_BYTES    32768       // 解压输出上限
#define WX_URL_MAX           320

// 一次 GET 拉到的 body：可能是压缩数据或明文，看 content-encoding。
typedef struct {
    char *data;       // malloc 的，caller free
    size_t len;
    bool   gzipped;
} wx_response_t;

static void wx_response_free(wx_response_t *r)
{
    if (r && r->data) { free(r->data); r->data = NULL; r->len = 0; }
}

// URL 百分比编码（保留 unreserved: A-Z a-z 0-9 - . _ ~）
static void wx_url_encode(char *out, size_t out_n, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s && n + 4 < out_n; s++) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
            (*s >= '0' && *s <= '9') || *s == '-' || *s == '.' || *s == '_' || *s == '~') {
            out[n++] = (char)*s;
        } else {
            out[n++] = '%';
            out[n++] = hex[*s >> 4];
            out[n++] = hex[*s & 0xF];
        }
    }
    out[n] = 0;
}

// 简易 JSON 字段抽取（找第一个 "key":value）：
//   - "key":"str" → str 写入 out（截断到 out_n-1）
//   - "key":num   → num 字符串写入 out
// 用于扁平 QWeather 响应，不支持嵌套 key。返回 true 表示写了非空。
static bool wx_json_str(const char *body, const char *key, char *out, size_t out_n)
{
    out[0] = 0;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e) return false;
        size_t l = (size_t)(e - p);
        if (l >= out_n) l = out_n - 1;
        memcpy(out, p, l);
        out[l] = 0;
    } else {
        size_t l = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' '
               && *p != '\n' && *p != '\r' && l + 1 < out_n) {
            out[l++] = *p++;
        }
        out[l] = 0;
    }
    return out[0] != 0;
}

// 拉一次 HTTP GET。返回 malloc 的 wx_response_t（body 是原始字节流），
// 失败返回 {NULL, 0}。请求头带 Accept-Encoding: gzip —— QWeather 无论如何
// 都会返回 gzip，我们诚实告诉服务器。
static wx_response_t wx_http_get(const char *url, int timeout_ms)
{
    wx_response_t out = { NULL, 0, false };

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return out;

    esp_http_client_set_header(c, "Accept-Encoding", "gzip");
    esp_http_client_set_header(c, "User-Agent", "rlcd/1.0");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return out;
    }

    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(NET_TAG, "http status %d for %.60s", status, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return out;
    }

    // Content-Encoding: gzip? 用 response header 版本 —— get_header() 读的是请求头！
    char *enc = NULL;
    esp_http_client_get_response_header(c, "Content-Encoding", &enc);
    out.gzipped = (enc && strcasecmp(enc, "gzip") == 0);
    // 后置嗅探：某些代理不设 Content-Encoding，但 body 就是 gzip 帧
    // （0x1f 0x8b 0x08 = ID1 ID2 CM）—— 兜底识别。
    // ↑ 见循环体后处理。
    ESP_LOGD(NET_TAG, "resp gzipped(header)=%d clen=%lld", (int)out.gzipped, (long long)clen);
    (void)clen;

    // 分配 body。已知长度：按 clen；未知（chunked）：先给 4 KiB，边读边扩。
    size_t cap = (clen > 0 && clen < WX_HTTP_MAX_BYTES) ? (size_t)clen + 1 : 4096;
    out.data = (char *)malloc(cap);
    if (!out.data) {
        ESP_LOGE(NET_TAG, "http body malloc %zu failed", cap);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return out;
    }

    for (;;) {
        if (out.len + 1024 > cap) {
            if (cap >= WX_HTTP_MAX_BYTES) {
                ESP_LOGW(NET_TAG, "http body exceeds %d bytes, truncating", WX_HTTP_MAX_BYTES);
                break;
            }
            size_t new_cap = cap * 2;
            if (new_cap > WX_HTTP_MAX_BYTES) new_cap = WX_HTTP_MAX_BYTES;
            char *p = (char *)realloc(out.data, new_cap);
            if (!p) { ESP_LOGE(NET_TAG, "http body realloc %zu failed", new_cap); break; }
            out.data = p; cap = new_cap;
        }
        int n = esp_http_client_read(c, out.data + out.len, cap - out.len - 1);
        if (n <= 0) break;
        out.len += (size_t)n;
    }
    out.data[out.len] = 0;

    // gzip 魔术数嗅探（兜底：某些代理不发 Content-Encoding，但 body 就是 gzip）
    if (!out.gzipped && out.len >= 3 &&
        (unsigned char)out.data[0] == 0x1f &&
        (unsigned char)out.data[1] == 0x8b &&
        (unsigned char)out.data[2] == 0x08) {
        out.gzipped = true;
        ESP_LOGD(NET_TAG, "resp gzip sniffed by magic");
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (out.len == 0) {
        free(out.data); out.data = NULL;
    }
    return out;
}

// gzip → plain。返回 malloc 的解压结果（NULL 表示失败）。
// 用 zlib 的 inflateInit2(15+32) —— magic +32 让它自动识别 gzip / zlib wrapper。
static char *wx_gunzip(const char *gz, size_t gz_len, size_t *out_len)
{
    if (!gz || gz_len == 0) return NULL;
    *out_len = 0;

    z_stream zs = { 0 };
    zs.next_in = (Bytef *)gz;
    zs.avail_in = (uInt)gz_len;
    // 15 = 最大 window bits；+32 = 自动检测 gzip/zlib header
    int rc = inflateInit2(&zs, 15 + 32);
    if (rc != Z_OK) {
        ESP_LOGW(NET_TAG, "inflateInit2: %d", rc);
        return NULL;
    }

    size_t cap = gz_len * 4;
    if (cap < 1024) cap = 1024;
    if (cap > WX_GZIP_MAX_BYTES) cap = WX_GZIP_MAX_BYTES;
    char *out = (char *)malloc(cap + 1);
    if (!out) { inflateEnd(&zs); return NULL; }

    for (;;) {
        zs.next_out = (Bytef *)(out + zs.total_out);
        zs.avail_out = (uInt)(cap - zs.total_out);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) {
            ESP_LOGW(NET_TAG, "inflate: %d (total_out=%lu)", rc, (unsigned long)zs.total_out);
            free(out); inflateEnd(&zs);
            return NULL;
        }
        if (zs.avail_out == 0) {
            // 输出满，扩容
            if (cap >= WX_GZIP_MAX_BYTES) {
                ESP_LOGW(NET_TAG, "gzip output exceeds %d bytes", WX_GZIP_MAX_BYTES);
                free(out); inflateEnd(&zs);
                return NULL;
            }
            size_t new_cap = cap * 2;
            if (new_cap > WX_GZIP_MAX_BYTES) new_cap = WX_GZIP_MAX_BYTES;
            char *p = (char *)realloc(out, new_cap + 1);
            if (!p) { free(out); inflateEnd(&zs); return NULL; }
            out = p; cap = new_cap;
        }
    }
    *out_len = zs.total_out;
    out[*out_len] = 0;
    inflateEnd(&zs);
    return out;
}

// 拉一次 QWeather 端点，返回 malloc 的明文 body（caller free）。
// HTTP GET → 按需 gzip 解压 → 校验 v7 响应外壳 `code=="200"`。失败返回 NULL。
static char *wx_fetch(const char *url)
{
    wx_response_t resp = wx_http_get(url, WX_HTTP_TIMEOUT_MS);
    if (!resp.data) return NULL;

    char *body = NULL;
    size_t body_len = 0;
    if (resp.gzipped) {
        body = wx_gunzip(resp.data, resp.len, &body_len);
        wx_response_free(&resp);
        if (!body) return NULL;
    } else {
        body = resp.data;
        body_len = resp.len;
        resp.data = NULL;   // 所有权转移，避免 free 两次
    }

    char code[8] = {0};
    if (!wx_json_str(body, "code", code, sizeof(code)) || strcmp(code, "200") != 0) {
        ESP_LOGW(NET_TAG, "qweather code=%s body=%.80s", code[0] ? code : "?", body);
        free(body);
        return NULL;
    }
    (void)body_len;
    return body;
}

// 解析后的城市信息 —— 只在 weather_task 内使用，不再是文件级 static。
typedef struct {
    char id[16];       // LocationID "101180106"
    char name[24];     // 标准中文名 "北京"
} wx_city_t;

// GeoAPI 城市解析：把 city 名/LocationID → {id, name, lat, lon}。
// URL: https://{host}/geo/v2/city/lookup?location=<name>&key=<key>
// 响应 location 数组第一项 —— 字段顺序 name/id/lat/lon/... 从数组第一个 { 开始扫。
static bool wx_geo_lookup(const char *host, const char *apikey,
                          const char *query, wx_city_t *out)
{
    memset(out, 0, sizeof(*out));

    char enc[96];
    wx_url_encode(enc, sizeof(enc), query);
    char url[WX_URL_MAX];
    snprintf(url, sizeof(url), "https://%s/geo/v2/city/lookup?location=%s&key=%s",
             host, enc, apikey);

    char *body = wx_fetch(url);
    if (!body) return false;

    // 定位到 "location":[{  开头，然后从这个对象里抓字段。
    const char *loc = strstr(body, "\"location\"");
    if (!loc) { free(body); return false; }
    const char *obj = strchr(loc, '{');
    if (!obj) { free(body); return false; }

    wx_json_str(obj, "id",   out->id,   sizeof(out->id));
    wx_json_str(obj, "name", out->name, sizeof(out->name));

    ESP_LOGI(NET_TAG, "geo '%s' → id=%s name=%s",
             query, out->id, out->name[0] ? out->name : "?");
    free(body);
    return out->id[0] != 0;
}

// 实况天气：填入 outdoor_temp / feels_like / humidity / pressure / vis /
// wind_speed / wind_dir / weather_text / cloud_pct。UV 在 /v7/weather/7d 里，不在这里。
// URL: https://{host}/v7/weather/now?location=<id>&key=<key>
static bool wx_fetch_now(const char *host, const char *apikey,
                         const wx_city_t *city, ui_model_t *m)
{
    char url[WX_URL_MAX];
    snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&key=%s",
             host, city->id, apikey);
    char *body = wx_fetch(url);
    if (!body) return false;

    char v[32];
    if (wx_json_str(body, "temp",      v, sizeof(v))) m->outdoor_temp    = (float)atoi(v);
    if (wx_json_str(body, "feelsLike", v, sizeof(v))) m->feels_like_temp = (float)atoi(v);
    if (wx_json_str(body, "humidity",  v, sizeof(v))) m->outdoor_humi    = (float)atoi(v);
    if (wx_json_str(body, "pressure",  v, sizeof(v))) m->pressure_hpa    = atoi(v);
    if (wx_json_str(body, "vis",       v, sizeof(v))) m->visibility_km   = atoi(v);
    if (wx_json_str(body, "windSpeed", v, sizeof(v))) m->wind_speed_kmh  = (float)atoi(v);
    if (wx_json_str(body, "cloud",     v, sizeof(v))) m->cloud_pct       = atoi(v);
    m->wind_dir[0] = 0;
    if (wx_json_str(body, "windDir",   v, sizeof(v))) {
        strncpy(m->wind_dir, v, sizeof(m->wind_dir) - 1);
        m->wind_dir[sizeof(m->wind_dir) - 1] = 0;
    }
    if (wx_json_str(body, "text",      v, sizeof(v))) {
        strncpy(m->weather_text, v, sizeof(m->weather_text) - 1);
        m->weather_text[sizeof(m->weather_text) - 1] = 0;
    }
    // icon → weather_code：3 位数字字符串，直接 atoi；主页按它加载位图
    if (wx_json_str(body, "icon", v, sizeof(v))) {
        int code = atoi(v);
        if (code > 0) m->weather_code = code;
    }

    free(body);
    return m->weather_text[0] != 0;
}

// 每日预报：daily[0] = 今天，取 tempMin/tempMax + sunrise/sunset + uvIndex。
// URL: https://{host}/v7/weather/7d?location=<id>&key=<key>
static bool wx_fetch_daily(const char *host, const char *apikey,
                           const wx_city_t *city, ui_model_t *m)
{
    char url[WX_URL_MAX];
    snprintf(url, sizeof(url), "https://%s/v7/weather/7d?location=%s&key=%s",
             host, city->id, apikey);
    char *body = wx_fetch(url);
    if (!body) return false;

    // json_get 用 strstr 找第一个 "<key>" —— daily[0] 在数组首位，抓到的就是今天。
    char v[16];
    if (wx_json_str(body, "tempMin", v, sizeof(v))) m->temp_min  = atoi(v);
    if (wx_json_str(body, "tempMax", v, sizeof(v))) m->temp_max  = atoi(v);
    if (wx_json_str(body, "uvIndex", v, sizeof(v))) m->uv_index  = atoi(v);
    if (wx_json_str(body, "sunrise", v, sizeof(v)) && strlen(v) >= 5) {
        memcpy(m->sunrise, v, 5); m->sunrise[5] = 0;
    }
    if (wx_json_str(body, "sunset", v, sizeof(v)) && strlen(v) >= 5) {
        memcpy(m->sunset, v, 5); m->sunset[5] = 0;
    }
    free(body);
    return true;
}

// 注：CSV 日志已迁移到 user_app.c —— 用独立的 10 分钟节奏，无网也能记录
// 本地温湿度（哪怕 weather 拉不到，"室外/天气" 字段留空即可）。

void weather_task(void *arg)
{
    ui_model_t *m = ui_model_get();
    wx_city_t city = {0};

    // 等 WiFi 联网 —— BIT_WIFI_CONNECTED 由 wifi_evt 在拿到 IP 时置位
    xEventGroupWaitBits(s_wifi_events, BIT_WIFI_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);
    // 首次多等 1s 让 SNTP / TLS 状态稳定
    vTaskDelay(pdMS_TO_TICKS(1000));

    for (;;) {
        bool ok = false;

        if (!s_cfg.weather_host[0] || !s_cfg.weather_apikey[0]) {
            ESP_LOGW(NET_TAG, "weather: host/apikey empty, skip");
        } else {
            // 城市解析：city 变化或从未解析过时重跑一次
            if (city.id[0] == 0) {
                const char *q = s_cfg.city[0] ? s_cfg.city : "Beijing";
                wx_geo_lookup(s_cfg.weather_host, s_cfg.weather_apikey, q, &city);
            }
            if (city.id[0] == 0) {
                ESP_LOGW(NET_TAG, "weather: city resolve failed");
            } else if (wx_fetch_now(s_cfg.weather_host, s_cfg.weather_apikey, &city, m)) {
                // Now 是主数据（含 cloud 云量），成功后写显示；Daily 失败只警告
                wx_fetch_daily(s_cfg.weather_host, s_cfg.weather_apikey, &city, m);
                if (Lvgl_lock(200)) {
                    if (city.name[0]) {
                        strncpy(m->city, city.name, sizeof(m->city) - 1);
                        m->city[sizeof(m->city) - 1] = 0;
                    } else if (!m->city[0]) {
                        strncpy(m->city, s_cfg.city, sizeof(m->city) - 1);
                        m->city[sizeof(m->city) - 1] = 0;
                    }
                    snprintf(m->weather_update, sizeof(m->weather_update),
                             "%02d:%02d", m->hour, m->minute);
                    ui_home_apply_locked();
                    Lvgl_unlock();
                }
                // 天气拉取成功 —— UI 已更新；CSV 日志由 user_app 独立任务负责
                ok = true;
            }
        }

        // 报告栈使用（保留一次，便于确认改到 zlib 后余量足够）
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(NET_TAG, "weather cycle ok=%d stack_free=%u B", (int)ok,
                 (unsigned)hwm * sizeof(StackType_t));

        // 下一轮：成功→10 min，失败→30 s；期间被 kick 会提前唤醒
        TickType_t wait = ok ? pdMS_TO_TICKS(10 * 60 * 1000) : pdMS_TO_TICKS(30 * 1000);
        xEventGroupWaitBits(s_weather_events, BIT_WEATHER_KICK,
                            pdTRUE, pdFALSE, wait);
    }
}
