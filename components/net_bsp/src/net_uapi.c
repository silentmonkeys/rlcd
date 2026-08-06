// net_uapi.c —— UAPI（uapis.cn）「查询我的 IP」接入
//
// 文档：https://uapis.cn/docs/api-reference/get-network-myip
//   GET https://uapis.cn/api/v1/network/myip?source=commercial
//   200 → {ip, region, isp, llc, asn, latitude, longitude, beginip, endip,
//          district, time_zone}
//        其中 district（行政区，如 "青秀区"）与 time_zone **仅 source=commercial
//        时可能返回**；region 是 "国家 省份 城市" 三段空格分隔。
//   400 → {code:"INVALID_STATE", message:"Could not determine client IP address."}
//   500 → {code:"INTERNAL_SERVER_ERROR", message:"..."}
//
// 鉴权：文档（FAQ）说明用请求头 `Authorization: Bearer <key>`，key 以 uapi- 开头。
// 本项目把 key 存 NVS（门户「天气」页可填），**不硬编码、不拼进 URL**。
// key 留空时按访客配额直接调用（FAQ：访客可用，1500 credits/月/IP、4 QPS）。
//
// 用途：管理页城市留空时，用 district 兜底喂给 QWeather 的城市解析。
// 频率：一天一次，挂在 weather_task 的每日节拍里（见 net_weather.c）。
// 约束：**绝不写回 s_cfg.city** —— 自动城市只是运行期兜底，用户手填的城市
//       永远优先；一旦用户填了城市，本接口就不再被调用。

#include "net_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_log.h>

#define UAPI_URL_MYIP_COMMERCIAL  "https://uapis.cn/api/v1/network/myip?source=commercial"
#define UAPI_TIMEOUT_MS           12000

// region "中国 广西 南宁市" → 取末段 "南宁市"（QWeather 城市解析吃得下市名）。
// 只有 district 缺失时才用它兜底。
static void uapi_region_tail(const char *region, char *out, size_t cap)
{
    out[0] = 0;
    if (!region || !region[0]) return;
    const char *tail = strrchr(region, ' ');
    net_copy_utf8(out, cap, tail ? tail + 1 : region);
}

bool NetBsp_FetchPublicIp(uapi_myip_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    net_http_req_t io = {
        .timeout_ms = UAPI_TIMEOUT_MS,
        .hdr_name   = NULL,
        .hdr_val    = NULL,
    };
    // 鉴权头：key 存在才带；形式固定 "Bearer <key>"，绝不进 URL。
    char auth[96] = {0};
    if (s_cfg.uapi_key[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", s_cfg.uapi_key);
        io.hdr_name = "Authorization";
        io.hdr_val  = auth;
    }

    size_t len = 0;
    char *body = net_http_get_text(UAPI_URL_MYIP_COMMERCIAL, &io, &len);

    // 连不上 / 无 body：网络异常或超时，交给调用方按节拍重试
    if (!body) {
        ESP_LOGW(NET_TAG, "uapi myip: 请求失败（网络异常/超时），status=%d", io.status);
        return false;
    }

    // 非 2xx：把文档里的 code/message 读出来再报，便于定位
    if (io.status < 200 || io.status >= 300) {
        char code[48] = {0}, msg[128] = {0};
        net_json_str(body, "code",    code, sizeof(code));
        net_json_str(body, "message", msg,  sizeof(msg));
        if (io.status == 429) {
            // 文档：RATE_LIMIT_EXCEEDED / SERVICE_BUSY / VISITOR_MONTHLY_QUOTA_EXHAUSTED
            // 响应带 Retry-After；本接口一天只调一次，等下一轮即可，不做内部快速重试。
            out->retry_after_s = io.retry_after_s;
            ESP_LOGW(NET_TAG, "uapi myip: 被限流(429) code=%s retry_after=%ds",
                     code[0] ? code : "?", io.retry_after_s);
        } else if (io.status == 401 || io.status == 403) {
            ESP_LOGE(NET_TAG, "uapi myip: 鉴权失败(%d) code=%s —— 检查门户里填的 UAPI Key",
                     io.status, code[0] ? code : "?");
        } else {
            ESP_LOGW(NET_TAG, "uapi myip: HTTP %d code=%s msg=%.80s",
                     io.status, code[0] ? code : "?", msg[0] ? msg : "");
        }
        free(body);
        return false;
    }

    // 200：抽字段。ip 是核心，拿不到就算失败（文档 400 分支也是"取不到客户端 IP"）
    net_json_str(body, "ip",       out->ip,       sizeof(out->ip));
    net_json_str(body, "region",   out->region,   sizeof(out->region));
    net_json_str(body, "isp",      out->isp,      sizeof(out->isp));
    net_json_str(body, "district", out->district, sizeof(out->district));
    free(body);

    if (!out->ip[0]) {
        ESP_LOGW(NET_TAG, "uapi myip: 响应缺少 ip 字段");
        return false;
    }

    // 自动城市：优先 district（commercial 才有），否则退到 region 末段
    if (out->district[0]) {
        net_copy_utf8(out->city, sizeof(out->city), out->district);
    } else {
        uapi_region_tail(out->region, out->city, sizeof(out->city));
        if (out->city[0])
            ESP_LOGI(NET_TAG, "uapi myip: 无 district，用 region 末段 '%s' 兜底", out->city);
    }

    ESP_LOGI(NET_TAG, "uapi myip: ip=%s region='%s' district='%s' → city='%s'",
             out->ip, out->region, out->district, out->city);
    return true;
}
