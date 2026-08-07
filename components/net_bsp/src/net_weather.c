// net_weather.c —— QWeather API 拉取
//
// 设计原则：
//   1. 所有缓冲局部分配，函数返回前 free —— 无 static state，无跨调用累积。
//   2. HTTP GET / gzip 解压 / JSON 取值抽到 net_http.c 与 net_uapi.c 共用。
//   3. 任一步失败立即返回，上层 weather_task 决定重试节奏。
//   4. Now/Daily 两个端点串行拉，Now 是主数据（决定是否显示），
//      Daily 失败只警告不阻塞。
//   5. 城市来源两条路：用户在门户手填（优先），或留空时由 UAPI 自动定位
//      （net_uapi.c，一天一次，与 Daily 共用同一个"跨天"节拍）。

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
#include <esp_log.h>

#define WX_HTTP_TIMEOUT_MS   15000
#define WX_URL_MAX           320

// HTTP GET / gzip 解压 / JSON 取值 / UTF-8 安全拷贝都在 net_http.c，
// 与 net_uapi.c 共用（原先这些是本文件的 static 副本）。
#define wx_json_str   net_json_str
#define wx_copy_utf8  net_copy_utf8

// 拉一次 QWeather 端点，返回 malloc 的明文 body（caller free）。
// HTTP GET → 按需 gzip 解压 → 校验 v7 响应外壳 `code=="200"`。失败返回 NULL。
static char *wx_fetch(const char *url)
{
    net_http_req_t io = { .timeout_ms = WX_HTTP_TIMEOUT_MS };
    size_t body_len = 0;
    char *body = net_http_get_text(url, &io, &body_len);
    if (!body) return NULL;
    if (io.status != 200) {
        ESP_LOGW(NET_TAG, "qweather HTTP %d for %.60s", io.status, url);
        free(body);
        return NULL;
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
    net_url_encode(enc, sizeof(enc), query);
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
    if (wx_json_str(body, "windDir",   v, sizeof(v)))
        wx_copy_utf8(m->wind_dir, sizeof(m->wind_dir), v);
    if (wx_json_str(body, "text",      v, sizeof(v)))
        wx_copy_utf8(m->weather_text, sizeof(m->weather_text), v);
    // icon → weather_code：3 位数字字符串，直接 atoi；主页按它加载位图
    if (wx_json_str(body, "icon", v, sizeof(v))) {
        int code = atoi(v);
        if (code > 0) m->weather_code = code;
    }

    free(body);
    return m->weather_text[0] != 0;
}

// 每日预报：daily[0] = 今天，取 tempMin/tempMax + sunrise/sunset + uvIndex。
// URL: https://{host}/v7/weather/3d?location=<id>&key=<key>
// 用 3d 而非 7d —— 我们只关心今天，减少传输体积。
static bool wx_fetch_daily(const char *host, const char *apikey,
                           const wx_city_t *city, ui_model_t *m)
{
    char url[WX_URL_MAX];
    snprintf(url, sizeof(url), "https://%s/v7/weather/3d?location=%s&key=%s",
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

// 「自动城市」一天一次：城市留空时调 UAPI /network/myip?source=commercial，
// 用 district 当查询词。结果只存运行期缓存（s_uapi_info + 返回值），
// **不写回 s_cfg.city** —— 手填城市永远优先，自动值不该变成"手填过"的样子。
//
// 与 Daily 共用同一个"跨天"判据：day 变了就该重新定位（设备可能已经挪地方了）。
// 返回是否成功拿到自动城市；auto_city 为出参（失败时保持原值不动）。
//
// 失败时**不**记 last_ip_day，让下一轮（10min/30s）重试；但用 tries 计数封顶，
// 避免限流/欠配额时一整天几百次空转 —— 每天最多试 UAPI_MAX_TRIES_PER_DAY 次。
#define UAPI_MAX_TRIES_PER_DAY  5

static bool wx_update_auto_city(int *last_ip_day, int *tries, int today,
                                char *auto_city, size_t cap)
{
    uapi_myip_t info;
    if (!NetBsp_FetchPublicIp(&info)) {
        if (++(*tries) >= UAPI_MAX_TRIES_PER_DAY) {
            *last_ip_day = today;   // 今天不再试，等跨天或手动刷新
            ESP_LOGW(NET_TAG, "auto city: 今日已失败 %d 次，暂停到明天（或手动刷新）",
                     *tries);
        }
        // 失败不清掉上次的自动城市 —— 宁可用旧的也别退回没有城市
        return auto_city[0] != 0;
    }

    s_uapi_info  = info;
    s_uapi_valid = true;
    *last_ip_day = today;
    *tries = 0;

    if (!info.city[0]) {
        ESP_LOGW(NET_TAG, "auto city: UAPI 未返回 district/region，无法定位城市");
        return auto_city[0] != 0;
    }
    if (strcmp(auto_city, info.city) != 0) {
        net_copy_utf8(auto_city, cap, info.city);
        ESP_LOGI(NET_TAG, "auto city: → '%s'（公网 IP %s）", auto_city, info.ip);
        return true;
    }
    return true;
}

void weather_task(void *arg)
{
    ui_model_t *m = ui_model_get();
    wx_city_t city = {0};

    // 等 WiFi 联网 —— BIT_WIFI_CONNECTED 由 wifi_evt 在拿到 IP 时置位
    xEventGroupWaitBits(s_wifi_events, BIT_WIFI_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);
    // 首次多等 1s 让 SNTP / TLS 状态稳定
    vTaskDelay(pdMS_TO_TICKS(1000));

    // daily 数据一天不变，只在首次（数值为空）或跨天时拉取，避免浪费请求。
    // -1 表示尚未拉过，下次循环必然触发。
    int last_daily_day = -1;
    // 自动定位同样一天一次，独立记日子：它可能失败（限流/无网）而 daily 成功。
    int  last_ip_day = -1;
    int  ip_tries    = 0;          // 当天已失败次数，封顶见 UAPI_MAX_TRIES_PER_DAY
    char auto_city[32] = {0};      // UAPI 定位出的城市，仅当 s_cfg.city 为空时使用

    for (;;) {
        bool ok = false;

        // ---- UAPI 每日定位：一天一次，天气凭据缺失时照样跑 ----
        // 两个用途：① 城市留空时提供自动城市；② 公网 IP / 归属地供门户「网络」页展示。
        // 放在天气凭据判断**之外** —— 没配 QWeather 也该能看到自己的公网 IP。
        // 门户「刷新城市」按钮通过 s_uapi_city_kick 无视每日节拍强制重跑。
        if (s_uapi_city_kick) {
            s_uapi_city_kick = false;
            last_ip_day = -1;
            ip_tries    = 0;        // 手动刷新重置失败计数
            ESP_LOGI(NET_TAG, "weather: 手动触发自动城市刷新");
        }
        if (last_ip_day == -1 || m->day != last_ip_day) {
            if (m->day != last_ip_day) ip_tries = 0;   // 跨天重新给满次数
            char prev[sizeof(auto_city)];
            strcpy(prev, auto_city);
            wx_update_auto_city(&last_ip_day, &ip_tries, m->day,
                                auto_city, sizeof(auto_city));
            // 自动城市变了且当前正用它（手填城市为空）→ 重解析 LocationID
            if (strcmp(prev, auto_city) != 0 && !s_cfg.city[0]) {
                city.id[0] = 0;
                last_daily_day = -1;
            }
        }

        if (!s_cfg.weather_host[0] || !s_cfg.weather_apikey[0]) {
            ESP_LOGW(NET_TAG, "weather: host/apikey empty, skip");
        } else {
            // 配网页改过城市 → 丢弃缓存的 LocationID，下面重新解析（无需重启设备）
            if (s_weather_city_dirty) {
                s_weather_city_dirty = false;
                city.id[0] = 0;
                last_daily_day = -1;    // 换城市了，daily 数据也得重拉
                ESP_LOGI(NET_TAG, "weather: city changed → re-resolve '%s'",
                         s_cfg.city[0] ? s_cfg.city : "(空→自动定位)");
            }

            // 查询词：手填城市优先，其次自动定位结果
            const char *q = s_cfg.city[0] ? s_cfg.city : auto_city;

            // 城市解析：city 变化或从未解析过时重跑一次
            if (city.id[0] == 0 && q[0]) {
                wx_geo_lookup(s_cfg.weather_host, s_cfg.weather_apikey, q, &city);
            }
            if (city.id[0] == 0) {
                if (!q[0])
                    ESP_LOGW(NET_TAG, "weather: 城市未配置且自动定位未成功，跳过本轮");
                else
                    ESP_LOGW(NET_TAG, "weather: city resolve failed ('%s')", q);
            } else if (wx_fetch_now(s_cfg.weather_host, s_cfg.weather_apikey, &city, m)) {
                // Daily（temp_min/max, uv, sunrise/sunset）一天只需拉一次：
                // 数值仍是哨兵（首次未拉到）或跨天时请求，其余轮次复用缓存。
                bool daily_needed = (last_daily_day == -1) ||
                                    (m->temp_min == UI_TEMP_NA) ||
                                    (m->day != last_daily_day);
                if (daily_needed) {
                    if (wx_fetch_daily(s_cfg.weather_host, s_cfg.weather_apikey, &city, m)) {
                        last_daily_day = m->day;
                    }
                } else {
                    ESP_LOGD(NET_TAG, "weather: daily cached (day=%d), skip", m->day);
                }
                if (Lvgl_lock(200)) {
                    if (city.name[0]) {
                        wx_copy_utf8(m->city, sizeof(m->city), city.name);
                    } else if (!m->city[0]) {
                        wx_copy_utf8(m->city, sizeof(m->city), q);
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
