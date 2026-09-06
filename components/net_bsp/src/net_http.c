// net_http.c —— 组件内共享的 HTTP GET / gzip 解压 / 轻量 JSON 取值
//
// 这些工具原先是 net_weather.c 的文件级 static，只有 QWeather 一个用户。
// 接入 UAPI（公网 IP → 自动城市）后出现第二个 HTTPS 调用方，与其把
// 200 行 HTTP + zlib 代码抄一遍，抽到本文件共用。
//
// 设计原则沿用 net_weather.c 的那套：
//   1. 所有缓冲局部分配，返回 malloc 的结果由 caller free —— 无 static state。
//   2. HTTP 用 esp_http_client_open / _fetch_headers / _read 流式读，
//      不用 event handler —— 数据直接读到本地缓冲。
//   3. gzip 用 espressif/zlib，每次调用完整 init/inflate/end 生命周期。
//   4. **非 2xx 也把 body 读回来**：UAPI 的错误码（code/message）在 body 里，
//      不读回来就只能报一个干巴巴的状态码。状态码放 io->status 交给调用方判断。

#include "net_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>

#include "zlib.h"

#define NET_HTTP_MAX_BYTES    16384       // 单次响应上限（未压缩）
#define NET_GZIP_MAX_BYTES    32768       // 解压输出上限

// ------------ HTTPS 串行闸 ----------------------------------------------
// 内部堆只有 ~26KB，一条 TLS 握手峰值就要 ~16KB（dynamic buffer + 上下文）。
// 天气(30s 轮询)和 xiaozhi(10s 轮询)的握手一撞车就是 mbedtls -0x008D。
// 所有 HTTPS（本文件 GET + net_xiaozhi POST + 未来的 OTA）都过这把锁，
// 把峰值从 N 条连接压回 1 条。请求本身不频繁，串行无感知。
static SemaphoreHandle_t s_https_mux = NULL;

void NetBsp_HttpLock(void)
{
    if (!s_https_mux) s_https_mux = xSemaphoreCreateMutex();
    xSemaphoreTake(s_https_mux, portMAX_DELAY);
}

void NetBsp_HttpUnlock(void)
{
    if (s_https_mux) xSemaphoreGive(s_https_mux);
}

// URL 百分比编码（保留 unreserved: A-Z a-z 0-9 - . _ ~）
void net_url_encode(char *out, size_t out_n, const char *in)
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
//   - "key":"str" → str 写入 out（超长按 UTF-8 字符边界截断）
//   - "key":num   → num 字符串写入 out
// 用于扁平响应，不支持嵌套 key。返回 true 表示写了非空。
//
// 截断必须按字符边界退：中文字段（windDir/text/district）都是 3 字节一个汉字，
// 若直接按字节截断会留下半个汉字，LVGL 渲染成方框 —— "东北风" 曾因此显示成 "东北□"。
bool net_json_str(const char *body, const char *key, char *out, size_t out_n)
{
    out[0] = 0;
    if (!body) return false;
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
        if (l >= out_n) {
            l = out_n - 1;
            // 退到首字节（非 10xxxxxx 的续字节）为止，丢掉不完整的尾字符
            while (l > 0 && ((unsigned char) p[l] & 0xC0) == 0x80) l--;
        }
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

// 把 src 拷进定长字段，超长时**按 UTF-8 字符边界**回退。
// 中文字段一律走这里，别用裸 strncpy —— 那会留下半个汉字，屏上是个方框。
void net_copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t l = strlen(src);
    if (l >= cap) {
        l = cap - 1;
        while (l > 0 && ((unsigned char) src[l] & 0xC0) == 0x80) l--;
    }
    memcpy(dst, src, l);
    dst[l] = 0;
}

// gzip → plain。返回 malloc 的解压结果（NULL 表示失败）。
// 用 zlib 的 inflateInit2(15+32) —— magic +32 让它自动识别 gzip / zlib wrapper。
char *net_gunzip(const char *gz, size_t gz_len, size_t *out_len)
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
    if (cap > NET_GZIP_MAX_BYTES) cap = NET_GZIP_MAX_BYTES;
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
            if (cap >= NET_GZIP_MAX_BYTES) {
                ESP_LOGW(NET_TAG, "gzip output exceeds %d bytes", NET_GZIP_MAX_BYTES);
                free(out); inflateEnd(&zs);
                return NULL;
            }
            size_t new_cap = cap * 2;
            if (new_cap > NET_GZIP_MAX_BYTES) new_cap = NET_GZIP_MAX_BYTES;
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

// 一次 HTTPS GET，返回 malloc 的**明文** body（需要时已 gunzip），caller free。
// 失败（连不上 / 无 body）返回 NULL。非 2xx 也会返回 body —— 状态码见 io->status，
// 调用方自己判断，这样才能读到 UAPI 错误响应里的 code/message。
//
// io->hdr_name/hdr_val 可选（UAPI 用来带 Authorization: Bearer）；
// io->retry_after_s 在服务端给了 Retry-After 时填秒数（限流退避用）。
// 实现：caller 必须已持 NetBsp_HttpLock()
static char *get_text_locked(const char *url, net_http_req_t *io, size_t *out_len)
{
    if (out_len) *out_len = 0;
    io->status = 0;
    io->retry_after_s = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = io->timeout_ms > 0 ? io->timeout_ms : 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;

    esp_http_client_set_header(c, "Accept-Encoding", "gzip");
    esp_http_client_set_header(c, "User-Agent", "rlcd/1.0");
    esp_http_client_set_header(c, "Accept", "application/json");
    if (io->hdr_name && io->hdr_val && io->hdr_val[0])
        esp_http_client_set_header(c, io->hdr_name, io->hdr_val);

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return NULL;
    }

    int64_t clen = esp_http_client_fetch_headers(c);
    io->status = esp_http_client_get_status_code(c);

    // Content-Encoding: gzip? 用 response header 版本 —— get_header() 读的是请求头！
    char *enc = NULL;
    esp_http_client_get_response_header(c, "Content-Encoding", &enc);
    bool gzipped = (enc && strcasecmp(enc, "gzip") == 0);

    // 限流退避用：429 会带 Retry-After（秒）
    char *ra = NULL;
    esp_http_client_get_response_header(c, "Retry-After", &ra);
    if (ra && ra[0]) {
        int v = atoi(ra);
        if (v > 0) io->retry_after_s = v;
    }

    // 分配 body。已知长度：按 clen；未知（chunked）：先给 4 KiB，边读边扩。
    size_t cap = (clen > 0 && clen < NET_HTTP_MAX_BYTES) ? (size_t)clen + 1 : 4096;
    char *raw = (char *)malloc(cap);
    if (!raw) {
        ESP_LOGE(NET_TAG, "http body malloc %zu failed", cap);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }

    size_t len = 0;
    for (;;) {
        if (len + 1024 > cap) {
            if (cap >= NET_HTTP_MAX_BYTES) {
                ESP_LOGW(NET_TAG, "http body exceeds %d bytes, truncating", NET_HTTP_MAX_BYTES);
                break;
            }
            size_t new_cap = cap * 2;
            if (new_cap > NET_HTTP_MAX_BYTES) new_cap = NET_HTTP_MAX_BYTES;
            char *p = (char *)realloc(raw, new_cap);
            if (!p) { ESP_LOGE(NET_TAG, "http body realloc %zu failed", new_cap); break; }
            raw = p; cap = new_cap;
        }
        int n = esp_http_client_read(c, raw + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    raw[len] = 0;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (len == 0) { free(raw); return NULL; }

    // gzip 魔术数嗅探（兜底：某些代理不发 Content-Encoding，但 body 就是 gzip）
    if (!gzipped && len >= 3 &&
        (unsigned char)raw[0] == 0x1f &&
        (unsigned char)raw[1] == 0x8b &&
        (unsigned char)raw[2] == 0x08) {
        gzipped = true;
        ESP_LOGD(NET_TAG, "resp gzip sniffed by magic");
    }

    if (!gzipped) {
        if (out_len) *out_len = len;
        return raw;
    }

    size_t plain_len = 0;
    char *plain = net_gunzip(raw, len, &plain_len);
    free(raw);
    if (!plain) return NULL;
    if (out_len) *out_len = plain_len;
    return plain;
}

char *net_http_get_text(const char *url, net_http_req_t *io, size_t *out_len)
{
    NetBsp_HttpLock();
    char *r = get_text_locked(url, io, out_len);
    NetBsp_HttpUnlock();
    return r;
}
