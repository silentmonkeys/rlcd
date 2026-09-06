// net_xiaozhi.c —— 接入 xiaozhi AI（协议提炼自 xiaozhi-esp32 仓库，MIT）
//
// 对话控制面 + 语音面（板上有 ES8311/ES7210 音频底座时）：
//   1. 激活：POST 系统信息到 OTA URL → 未激活时返回 activation{code}，
//      用户在 xiaozhi.me 控制台输入激活码；设备轮询 POST <ota>/activate
//      （202=等待，200=成功），成功后 check 响应携带 websocket{url,token}
//      存 NVS —— 与 xiaozhi-esp32 main/ota.cc 的流程一致。
//   2. 文本对话：按需开 WebSocket（Authorization: Bearer <token>、Device-Id、
//      Client-Id），发 hello 握手，然后发
//        {"type":"listen","state":"detect","text":"<用户问题>"}
//      收 tts(sentence_start=回答文本) / llm(emotion=情绪) / tts stop，
//      一轮结束即断开 —— 与 websocket_protocol.cc / application.cc 一致。
//   3. 语音对话（音频底座已注册时）：
//      - KEY 按住 → listen start(mode=manual)，麦克风 16k/16bit/mono 采
//        60ms 一帧 → opus 编码 → WS binary 帧上行；KEY 松开 → listen stop。
//      - 服务端 stt(text) / llm(emotion) / tts(sentence_start+binary opus
//        音频)；opus 音频 → 解码 → 重采样到 16k → 扬声器。TTS 常见 24k，
//        而 I2S 全双工收发共享时钟、麦克风侧固定 16k，故播放侧做线性重采样。
//      - tts stop 且音频播完 → 会话结束断开。
//   4. 状态/情绪写入 ui_model：bot_state 驱动机器人姿态，bot_emotion 在
//      播报时覆盖表情，bot_chat_user / bot_chat_reply 显示在 BOT 页底部。
#include "net_internal.h"
#include "ui_model.h"

#include <string.h>
#include <stdio.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_websocket_client.h>
#include <freertos/ringbuf.h>
#include <esp_timer.h>
#include <esp_app_desc.h>
#include <esp_system.h>

#include <esp_audio_dec.h>
#include <decoder/impl/esp_opus_dec.h>
#include <esp_audio_enc.h>
#include <encoder/impl/esp_opus_enc.h>

#define XZ_TAG "xiaozhi"

// api.tenclass.net 固定信任根：DigiCert Global Root G2（EMBED_TXTFILES 注入，
// 见 net_bsp/CMakeLists.txt）。服务端现用 GeoTrust G2 TLS CN RSA4096 SHA256
// 2022 CA1 中间证书，默认证书包按签发者名匹配不到 → -0x3000。
// 注意：设了 cert_pem 就不能再设 crt_bundle_attach（esp-tls 二选一）。
extern const unsigned char xz_root_pem_start[] asm("_binary_digicert_global_root_g2_pem_start");
extern const unsigned char xz_root_pem_end[]   asm("_binary_digicert_global_root_g2_pem_end");

// OTA / 激活入口（xiaozhi 官方默认；控制台 https://xiaozhi.me）
#define XZ_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"

// NVS（命名空间独立于 rlcd_cfg，键名对齐 xiaozhi-esp32 的 Settings("websocket")）
#define XZ_NVS_NS     "xiaozhi"
#define XZ_NVS_UUID   "uuid"
#define XZ_NVS_URL    "url"
#define XZ_NVS_TOKEN  "token"
#define XZ_NVS_BOUND  "bound"   // "1" = 配置来自真绑定（服务端对未绑定设备也回
                                // test-token 占位 websocket，不能拿它当配置）

#define XZ_SESSION_TIMEOUT_MS 30000   // 单轮文本对话整体超时
#define XZ_MIC_MAX_MS         15000   // 单次按住说话最长时间
#define XZ_PLAY_TIMEOUT_MS    90000   // TTS 播放阶段兜底超时（长回答）

// 语音规格（xiaozhi 协议标准）：上行麦克风 16k/mono/60ms；下行 TTS 的采样率
// 以服务端 hello 的 audio_params 为准（常见 24k），播放前重采样到 16k。
#define XZ_SAMPLE_RATE        16000
#define XZ_FRAME_SAMPLES      (XZ_SAMPLE_RATE * 60 / 1000)   // 960
#define XZ_ENC_BUF_SIZE       1024    // 60ms@24kbps ≈ 180B，留足余量
#define XZ_DEC_BUF_SIZE       4096    // 24k×60ms×2B = 2880B 最大
#define XZ_AUDIO_RB_SIZE      (32 * 1024)  // 下行 opus 环形缓冲（~4s 码流）

// ------------ 内部状态 ------------------------------------------------
static char s_uuid[37];                 // Client-Id（首次生成，NVS 持久化）
static char s_wss_url[160];             // 激活后下发
static char s_wss_token[160];
static int8_t s_xz_status = UI_BOT_XZ_UNSET;
static bool  s_chat_busy = false;       // 一轮对话进行中（门户再点 → 拒绝）

// 待发送的问题（门户 POST /api/xz_chat 写入，xiaozhi_task 消费）
#define XZ_TEXT_MAX 128
static char    s_pending_text[XZ_TEXT_MAX];
static volatile bool s_pending = false;
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;

static void model_state(int8_t st)
{
    ui_model_t *m = ui_model_get();
    m->bot_state = st;
}

static void model_xz_status(int8_t st)
{
    ui_model_t *m = ui_model_get();
    m->bot_xz_status = st;
    s_xz_status = st;
}

// ------------ 小工具 ---------------------------------------------------
// MAC 格式：xiaozhi 服务端校验冒号分隔（"AA:BB:CC:11:22:33"），
// 无分隔 12 位 hex 会被 400 Invalid MAC address 拒掉
static void mac_str(char *out, size_t n)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Client-Id：首次用 esp_random 生成 UUID v4，之后读 NVS
static void uuid_ensure(void)
{
    nvs_handle_t nh;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        size_t len = sizeof(s_uuid);
        if (nvs_get_str(nh, XZ_NVS_UUID, s_uuid, &len) == ESP_OK && s_uuid[0]) {
            nvs_close(nh);
            return;
        }
        uint8_t b[16];
        esp_fill_random(b, sizeof(b));
        b[6] = (b[6] & 0x0F) | 0x40;    // version 4
        b[8] = (b[8] & 0x3F) | 0x80;    // variant 10
        snprintf(s_uuid, sizeof(s_uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        nvs_set_str(nh, XZ_NVS_UUID, s_uuid);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

// 一次性 POST JSON → body 进 heap 缓冲（caller free）。非 2xx 也回 body。
// 走 NetBsp_HttpLock：与天气/定位共享内部堆，TLS 握手必须串行。
static char *xz_http_post_json_locked(const char *url, const char *body,
                                      const char *extra_hdr, const char *extra_val,
                                      int *out_status);

static char *xz_http_post_json(const char *url, const char *body,
                               const char *extra_hdr, const char *extra_val,
                               int *out_status)
{
    NetBsp_HttpLock();
    char *r = xz_http_post_json_locked(url, body, extra_hdr, extra_val, out_status);
    NetBsp_HttpUnlock();
    return r;
}

static char *xz_http_post_json_locked(const char *url, const char *body,
                                      const char *extra_hdr, const char *extra_val,
                                      int *out_status)
{
    char *buf = NULL;
    size_t len = 0, cap = 4096;
    buf = malloc(cap);
    if (!buf) return NULL;

    char mac[18];   // 冒号 MAC 17 字符 + NUL（原 12 位格式的 [16] 会截断成 "xx:xx:xx:xx:xx:"）
    mac_str(mac, sizeof(mac));
    char ua[64];
    snprintf(ua, sizeof(ua), "rlcd/1.0 esp32s3");

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.cert_pem = (const char *)xz_root_pem_start;   // tenclass 固定根，见文件头
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(buf); return NULL; }

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "User-Agent", ua);
    esp_http_client_set_header(c, "Device-Id", mac);
    esp_http_client_set_header(c, "Client-Id", s_uuid);
    // 未激活设备（无 eFuse 序列号）走 Activation-Version: 1
    esp_http_client_set_header(c, "Activation-Version", "1");
    if (extra_hdr) esp_http_client_set_header(c, extra_hdr, extra_val);

    esp_err_t oerr = esp_http_client_open(c, strlen(body));
    if (oerr != ESP_OK) {
        // open 里含 TCP+TLS 握手，失败原因在这里，必须打出来（此前被吞，
        // 表现成 status=0 空 body）
        ESP_LOGW(XZ_TAG, "POST %s open failed: %s", url, esp_err_to_name(oerr));
        free(buf);
        esp_http_client_cleanup(c);
        if (out_status) *out_status = 0;
        return NULL;
    }
    esp_http_client_write(c, body, strlen(body));
    esp_err_t ferr = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (ferr < 0) {
        ESP_LOGW(XZ_TAG, "POST %s fetch_headers 失败（连接中断）", url);
        free(buf);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (out_status) *out_status = 0;
        return NULL;
    }

    int n;
    while ((n = esp_http_client_read(c, buf + len, (int)(cap - len - 1))) > 0) {
        len += n;
        if (cap - len < 256) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
    }
    buf[len] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (out_status) *out_status = status;
    ESP_LOGI(XZ_TAG, "POST %s → %d (%u bytes)", url, status, (unsigned) len);
    return buf;
}

// check-version 响应解析：
//   - 未绑定设备：activation{code} 与一个 **test-token 占位 websocket** 同时返回
//     —— 必须优先走激活码流程，占位 token 不能当配置用（WS 鉴权会拒）。
//     返回 false，激活码已写入 ui_model 供门户/BOT 页展示。
//   - 已绑定设备：只有 websocket{url,token} → 返回 true。
static bool parse_check_response(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;

    bool got_ws = false;
    cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(act)) {
        ui_model_t *m = ui_model_get();
        cJSON *code = cJSON_GetObjectItem(act, "code");
        cJSON *msg = cJSON_GetObjectItem(act, "message");
        if (cJSON_IsString(code)) {
            snprintf(m->bot_xz_code, sizeof(m->bot_xz_code), "%s", code->valuestring);
            ESP_LOGI(XZ_TAG, "*** 激活码 %s —— 到 xiaozhi.me 控制台输入绑定 ***",
                     code->valuestring);
        }
        if (cJSON_IsString(msg))
            ESP_LOGI(XZ_TAG, "激活提示: %s", msg->valuestring);
        cJSON_Delete(root);
        return false;               // 未绑定：进入 activate 轮询
    }

    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        cJSON *u = cJSON_GetObjectItem(ws, "url");
        cJSON *t = cJSON_GetObjectItem(ws, "token");
        if (cJSON_IsString(u) && cJSON_IsString(t)) {
            snprintf(s_wss_url, sizeof(s_wss_url), "%s", u->valuestring);
            snprintf(s_wss_token, sizeof(s_wss_token), "%s", t->valuestring);
            got_ws = true;
        }
    }
    cJSON_Delete(root);
    return got_ws;
}

static void save_ws_config(void)
{
    nvs_handle_t nh;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_set_str(nh, XZ_NVS_URL, s_wss_url);
    nvs_set_str(nh, XZ_NVS_TOKEN, s_wss_token);
    nvs_set_u8(nh, XZ_NVS_BOUND, 1);   // 只有真绑定流程会走到这里
    nvs_commit(nh);
    nvs_close(nh);
}

// 系统信息 body —— 对齐 xiaozhi-esp32 board.cc GetSystemInfoJson 的关键字段
// （version:2 / mac_address / uuid / chip / application；分区表省略）。
// 键名照抄官方：此前用 "mac" 简化版虽也能拿到激活码，但别赌服务端分支。
static void build_check_body(char *out, size_t n)
{
    char mac[20];
    mac_str(mac, sizeof(mac));
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(out, n,
             "{\"version\":2,"
             "\"minimum_free_heap_size\":%d,"
             "\"mac_address\":\"%s\","
             "\"uuid\":\"%s\","
             "\"chip_model_name\":\"esp32s3\","
             "\"application\":{"
                 "\"name\":\"%s\","
                 "\"version\":\"%s\","
                 "\"compile_time\":\"%sT%sZ\","
                 "\"idf_version\":\"%s\"},"
             "\"board\":{\"type\":\"esp32s3-rlcd-42\"}}",
             (int)esp_get_minimum_free_heap_size(),
             mac, s_uuid,
             d->project_name, d->version, d->date, d->time, d->idf_ver);
}

// 激活循环：check → 未激活则展示 code 并轮询 activate → 拿到 websocket 配置
static bool activate_flow(void)
{
    ui_model_t *m = ui_model_get();
    model_xz_status(UI_BOT_XZ_ACTIVATING);
    model_state(UI_BOT_ST_ACTIVATING);
    m->bot_xz_code[0] = 0;

    for (int round = 0; round < 360; round++) {   // 最多 1 小时
        char body[384];
        build_check_body(body, sizeof(body));
        int status = 0;
        char *resp = xz_http_post_json(XZ_OTA_URL, body, NULL, NULL, &status);
        if (!resp) {
            ESP_LOGW(XZ_TAG, "check 连接失败（网络/TLS 内存），10s 后重试");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (parse_check_response(resp)) {
            free(resp);
            save_ws_config();
            ESP_LOGI(XZ_TAG, "激活完成，websocket 配置已存 NVS");
            return true;
        }
        // activation 已在 parse 里展示；走到这还可能是「无 activation 也无
        // websocket」的意外响应 —— 打出头部片段便于诊断
        if (!m->bot_xz_code[0])
            ESP_LOGW(XZ_TAG, "check(%d) 响应无 activation/websocket: %.160s",
                     status, resp);
        free(resp);

        // 未激活 → 轮询 activate（空载荷，Device-Id 头识别设备；202=等待输码）
        int astatus = 0;
        char *abody = xz_http_post_json(XZ_OTA_URL "activate", "{}", NULL, NULL, &astatus);
        if (abody) {
            if (astatus != 200 && astatus != 202)
                ESP_LOGW(XZ_TAG, "activate(%d) 响应: %.160s", astatus, abody);
            free(abody);
        }
        ESP_LOGI(XZ_TAG, "activate → %d（202=等待用户在控制台输入激活码）", astatus);
        if (astatus == 200) {
            // 已确认激活，下一轮 check 就会带 websocket 配置
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    return false;
}

// ------------ 音频底座（port_bsp 反向注册）+ PTT 状态 -----------------
static xiaozhi_audio_ops_t s_audio;         // ready=false 时全走纯文本
static volatile bool s_voice_pending = false;   // KEY 按下待处理
static volatile bool s_ptt_down      = false;   // 语音会话期间保持按住

void NetBsp_XiaozhiSetAudioOps(const xiaozhi_audio_ops_t *ops)
{
    if (ops) s_audio = *ops;
    else     memset(&s_audio, 0, sizeof(s_audio));
}

bool NetBsp_XiaozhiPttDown(void)
{
    if (!s_audio.ready || s_chat_busy || s_voice_pending) return false;
    s_voice_pending = true;
    return true;
}

void NetBsp_XiaozhiPttUp(void)
{
    s_ptt_down = false;
}

// ------------ 对话会话 -------------------------------------------------
typedef struct {
    SemaphoreHandle_t done;         // 会话结束（含 hello 失败/超时）
    char session_id[80];
    bool hello_ok;
    volatile bool stop;
    // ---- 语音路径 ----
    bool  audio;                    // true=下行 opus 音频走扬声器播放
    volatile bool tts_done;         // 服务端 tts stop 已到（音频可能还在播）
    RingbufHandle_t rb;             // 下行 opus 码流（ws 事件线程 → 播放循环）
    int   spk_rate;                 // 服务端 TTS 采样率（默认 24k）
    // binary 帧分片重组（xiaozhi 的 opus 帧都很小，一般一帧到齐，防御性留）
    uint8_t frag[XZ_ENC_BUF_SIZE * 2];
    int     frag_len, frag_total;
} xz_session_t;

// xiaozhi llm.emotion 字符串（font_awesome 表）→ UI 枚举；未识别返回 NEUTRAL
static int8_t emotion_to_enum(const char *e)
{
    static const struct { const char *name; int8_t emo; } MAP[] = {
        {"neutral",    UI_BOT_EMO_NEUTRAL},
        {"happy",      UI_BOT_EMO_HAPPY},   {"laughing", UI_BOT_EMO_HAPPY},
        {"funny",      UI_BOT_EMO_HAPPY},   {"delicious", UI_BOT_EMO_HAPPY},
        {"winking",    UI_BOT_EMO_HAPPY},   {"confident", UI_BOT_EMO_HAPPY},
        {"sad",        UI_BOT_EMO_SAD},     {"crying", UI_BOT_EMO_SAD},
        {"embarrassed", UI_BOT_EMO_SAD},
        {"angry",      UI_BOT_EMO_ANGRY},
        {"surprised",  UI_BOT_EMO_SURPRISED}, {"shocked", UI_BOT_EMO_SURPRISED},
        {"silly",      UI_BOT_EMO_SURPRISED},
        {"sleepy",     UI_BOT_EMO_SLEEPY},  {"relaxed", UI_BOT_EMO_SLEEPY},
        {"thinking",   UI_BOT_EMO_THINKING},
        {"loving",     UI_BOT_EMO_LOVING},
        {"confused",   UI_BOT_EMO_CONFUSED},
        {"cool",       UI_BOT_EMO_COOL},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (strcmp(MAP[i].name, e) == 0) return MAP[i].emo;
    return UI_BOT_EMO_NEUTRAL;
}

static void handle_text(const char *data, int len, xz_session_t *sess)
{
    static char buf[2048];
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    cJSON *type = cJSON_GetObjectItem(root, "type");
    ui_model_t *m = ui_model_get();
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "hello") == 0) {
            cJSON *sid = cJSON_GetObjectItem(root, "session_id");
            if (cJSON_IsString(sid))
                snprintf(sess->session_id, sizeof(sess->session_id), "%s", sid->valuestring);
            // 服务端下行音频参数（TTS 常见 24k；不报就按 24k 兜底）
            sess->spk_rate = 24000;
            cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
            cJSON *sr = cJSON_IsObject(ap) ? cJSON_GetObjectItem(ap, "sample_rate") : NULL;
            if (cJSON_IsNumber(sr) && sr->valueint >= 8000 && sr->valueint <= 48000)
                sess->spk_rate = sr->valueint;
            sess->hello_ok = true;
            ESP_LOGI(XZ_TAG, "server hello, session=%s spk_rate=%d",
                     sess->session_id, sess->spk_rate);
        } else if (strcmp(type->valuestring, "tts") == 0) {
            cJSON *st = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(st)) {
                if (strcmp(st->valuestring, "start") == 0) {
                    model_state(UI_BOT_ST_SPEAKING);
                } else if (strcmp(st->valuestring, "sentence_start") == 0) {
                    cJSON *text = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(text)) {
                        net_copy_utf8(m->bot_chat_reply, sizeof(m->bot_chat_reply), text->valuestring);
                        ESP_LOGI(XZ_TAG, "<< %s", m->bot_chat_reply);
                    }
                } else if (strcmp(st->valuestring, "stop") == 0) {
                    // 语音路径：等音频播完再收尾；纯文本路径直接结束
                    if (sess->audio) sess->tts_done = true;
                    else             sess->stop = true;
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            cJSON *text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                net_copy_utf8(m->bot_chat_user, sizeof(m->bot_chat_user), text->valuestring);
                m->bot_chat_reply[0] = 0;    // 新一轮：清上一回答
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            cJSON *emo = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emo)) {
                m->bot_emotion = emotion_to_enum(emo->valuestring);
                ESP_LOGI(XZ_TAG, "emotion: %s", emo->valuestring);
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            cJSON *msg = cJSON_GetObjectItem(root, "message");
            if (cJSON_IsString(msg)) {
                net_copy_utf8(m->bot_chat_reply, sizeof(m->bot_chat_reply), msg->valuestring);
            }
        }
    }
    cJSON_Delete(root);
}

// 下行 opus 帧入环形缓冲（处理 WS 分片：payload_offset/payload_len 拼装）
static void feed_audio(xz_session_t *sess, esp_websocket_event_data_t *ev)
{
    if (!sess->rb) return;
    if (ev->payload_offset == 0) { sess->frag_len = 0; sess->frag_total = ev->payload_len; }
    if (ev->payload_offset + ev->data_len > sizeof(sess->frag)) return;   // 异常大帧，丢
    memcpy(sess->frag + ev->payload_offset, ev->data_ptr, ev->data_len);
    sess->frag_len = ev->payload_offset + ev->data_len;
    if (sess->frag_len >= sess->frag_total && sess->frag_total > 0) {
        if (xRingbufferSend(sess->rb, sess->frag, sess->frag_total, 0) != pdTRUE)
            ESP_LOGW(XZ_TAG, "audio rb 满，丢 %dB", sess->frag_total);
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    xz_session_t *sess = (xz_session_t *)arg;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(XZ_TAG, "ws connected");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (ev->op_code == 0x1 && ev->data_ptr && ev->data_len > 0) {
                // TEXT 帧（忽略分片重组：xiaozhi 的控制消息都很小，一帧到齐）
                handle_text((const char *)ev->data_ptr, ev->data_len, sess);
            } else if ((ev->op_code == 0x2 ||          // BINARY
                        (ev->op_code == 0 && sess->frag_len > 0)) &&  // 分片后续
                       ev->data_ptr && ev->data_len > 0) {
                feed_audio(sess, ev);     // opus 音频（含分片重组）
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            xSemaphoreGive(sess->done);
            break;
        default:
            break;
    }
}

// 打开 WS 并等 CONNECTED。成功返回句柄，失败返回 NULL（sess 内部资源由
// caller 统一清理）。
static esp_websocket_client_handle_t ws_open(xz_session_t *sess)
{
    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", s_wss_token);
    char mac[18];   // 冒号 MAC 17 字符 + NUL（原 12 位格式的 [16] 会截断成 "xx:xx:xx:xx:xx:"）
    mac_str(mac, sizeof(mac));

    esp_websocket_client_config_t cfg = {};
    cfg.uri = s_wss_url;
    // 控制帧/opus 帧都很小（<1KB）；4KB 缓冲 + TLS 在 35KB 内部堆上握手
    // 会挤爆（alloc 4770 失败实测）
    cfg.buffer_size = 2048;
    cfg.cert_pem = (const char *)xz_root_pem_start;   // tenclass 固定根，见文件头
    cfg.keep_alive_enable = true;
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) return NULL;
    esp_websocket_client_append_header(ws, "Authorization", auth);
    esp_websocket_client_append_header(ws, "Protocol-Version", "1");
    esp_websocket_client_append_header(ws, "Device-Id", mac);
    esp_websocket_client_append_header(ws, "Client-Id", s_uuid);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, sess);

    // 握手期与天气/定位的 TLS 互斥（NetBsp_HttpLock）——否则并发握手在
    // 26~35KB 内部堆上必撞（alloc 4770 失败实测）
    NetBsp_HttpLock();
    if (esp_websocket_client_start(ws) != ESP_OK) {
        NetBsp_HttpUnlock();
        esp_websocket_client_destroy(ws);
        return NULL;
    }
    int wait_ms = 0;
    while (!esp_websocket_client_is_connected(ws) && wait_ms < 10000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }
    if (!esp_websocket_client_is_connected(ws)) {
        NetBsp_HttpUnlock();
        ESP_LOGE(XZ_TAG, "ws 连接失败");
        esp_websocket_client_destroy(ws);
        return NULL;
    }
    // 连接保持期间不持锁：hello 之后是长会话（收发音频），锁只保护握手
    return ws;
}

// 设备 hello（照抄 websocket_protocol.cc 的 GetHelloMessage，音频参数固定
// 上报 16k/mono/60ms —— 麦克风侧规格）
static bool send_hello(esp_websocket_client_handle_t ws)
{
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "hello");
    cJSON_AddNumberToObject(h, "version", 1);
    cJSON *feat = cJSON_CreateObject();
    cJSON_AddBoolToObject(feat, "mcp", true);
    cJSON_AddItemToObject(h, "features", feat);
    cJSON_AddStringToObject(h, "transport", "websocket");
    cJSON *ap = cJSON_CreateObject();
    cJSON_AddStringToObject(ap, "format", "opus");
    cJSON_AddNumberToObject(ap, "sample_rate", XZ_SAMPLE_RATE);
    cJSON_AddNumberToObject(ap, "channels", 1);
    cJSON_AddNumberToObject(ap, "frame_duration", 60);
    cJSON_AddItemToObject(h, "audio_params", ap);
    char *s = cJSON_PrintUnformatted(h);
    bool ok = esp_websocket_client_send_text(ws, s, strlen(s), portMAX_DELAY) > 0;
    cJSON_free(s);
    cJSON_Delete(h);
    return ok;
}

// 等 server hello（顺带等 audio_params 填 spk_rate）
static bool wait_hello(xz_session_t *sess)
{
    int wait_ms = 0;
    while (!sess->hello_ok && wait_ms < 10000) {
        if (xSemaphoreTake(sess->done, 0) == pdTRUE) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
    }
    if (!sess->hello_ok) ESP_LOGE(XZ_TAG, "server hello 超时");
    return sess->hello_ok;
}

// ------------ 下行播放：opus 解码 → 重采样 → 扬声器 --------------------
// 24k（或服务端给的其它率）→ 16k 线性插值。相位跨帧累积，会话开始时清零。
typedef struct {
    float    pos;                   // 相对源的浮点读位置
    int16_t  prev, next;            // 上一帧末尾两个样本（跨帧插值用）
    bool     has_prev;
} resamp_t;

// 把 src（rate_in）重采样到 16k 写进 dst，返回写入样本数
static int resamp_16k(resamp_t *r, const int16_t *src, int n,
                      int rate_in, int16_t *dst, int dst_max)
{
    if (rate_in == XZ_SAMPLE_RATE) {         // 免重采样
        int c = (n < dst_max) ? n : dst_max;
        memcpy(dst, src, c * 2);
        return c;
    }
    double step = (double)rate_in / XZ_SAMPLE_RATE;
    int out = 0;
    float p = r->pos;
    int16_t s0 = r->has_prev ? r->prev : src[0];
    for (int i = 0; i < n && out < dst_max; i++) {
        while (p < 1.0f && out < dst_max) {  // 在 src[i-1] 与 src[i] 之间插值
            dst[out++] = (int16_t)((1.0f - p) * s0 + p * src[i]);
            p += (float)step;
        }
        p -= 1.0f;
        s0 = src[i];
    }
    r->pos = p;
    r->prev = s0;
    r->has_prev = true;
    return out;
}

// 播放循环：环形缓冲取 opus 帧 → 解码 → 重采样 → 扬声器。
// 结束条件：tts_done 且缓冲播空 / 连接断开 / 超时。
static void playback_loop(xz_session_t *sess, esp_websocket_client_handle_t ws)
{
    void *dec = NULL;
    int16_t *pcm24 = malloc(XZ_DEC_BUF_SIZE);
    int16_t *pcm16 = malloc(XZ_FRAME_SAMPLES * 2 * 2);  // 2 帧余量
    uint8_t *pkt = malloc(XZ_ENC_BUF_SIZE * 2);
    if (!pcm24 || !pcm16 || !pkt) goto out;

    esp_opus_dec_cfg_t dc = {
        .sample_rate   = sess->spk_rate,
        .channel       = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&dc, sizeof(dc), &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(XZ_TAG, "opus dec open fail");
        goto out;
    }

    // 本循环只在 xiaozhi 任务里跑 → 重采样相位做文件级 static 跨帧保持
    static resamp_t rs;
    memset(&rs, 0, sizeof(rs));

    int64_t last_data = esp_timer_get_time();
    for (;;) {
        if (xSemaphoreTake(sess->done, 0) == pdTRUE) break;
        if (esp_timer_get_time() - last_data > (int64_t)XZ_PLAY_TIMEOUT_MS * 1000) {
            ESP_LOGW(XZ_TAG, "播放超时兜底");
            break;
        }
        size_t sz = 0;
        uint8_t *p = (uint8_t *)xRingbufferReceiveUpTo(
            sess->rb, &sz, pdMS_TO_TICKS(100), XZ_ENC_BUF_SIZE * 2);
        if (!p) {
            if (sess->tts_done) break;   // 播完且没有新数据
            continue;
        }
        last_data = esp_timer_get_time();
        memcpy(pkt, p, sz);
        vRingbufferReturnItem(sess->rb, p);

        esp_audio_dec_in_raw_t raw = { .buffer = pkt, .len = sz };
        esp_audio_dec_out_frame_t frame = { .buffer = (uint8_t *)pcm24,
                                            .len = XZ_DEC_BUF_SIZE };
        esp_audio_dec_info_t info;
        if (esp_opus_dec_decode(dec, &raw, &frame, &info) != ESP_AUDIO_ERR_OK)
            continue;
        int written = resamp_16k(&rs, pcm24,
                                 frame.decoded_size / 2, sess->spk_rate,
                                 pcm16, XZ_FRAME_SAMPLES * 2);
        if (written > 0) s_audio.spk_write(pcm16, written);
    }
out:
    if (dec) esp_opus_dec_close(dec);
    free(pcm24); free(pcm16); free(pkt);
}

// 会话收尾：关 ws、删 rb、模型回 IDLE
static void session_close(xz_session_t *sess, esp_websocket_client_handle_t ws)
{
    if (ws) {
        esp_websocket_client_close(ws, 1000);
        esp_websocket_client_destroy(ws);
    }
    if (sess->rb) vRingbufferDelete(sess->rb);
    vSemaphoreDelete(sess->done);
    model_state(UI_BOT_ST_IDLE);
    ESP_LOGI(XZ_TAG, "会话结束（task 栈余量 %u B）",
             (unsigned)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
}

// 一轮文本对话：连 → hello → detect(text) → 播 tts（有音频底座时）→ 收尾
static void chat_session(const char *text)
{
    ui_model_t *m = ui_model_get();
    m->bot_chat_user[0] = 0;
    m->bot_chat_reply[0] = 0;
    net_copy_utf8(m->bot_chat_user, sizeof(m->bot_chat_user), text);
    model_state(UI_BOT_ST_CONNECTING);
    m->bot_emotion = UI_BOT_EMO_NEUTRAL;

    xz_session_t sess = {0};
    sess.done = xSemaphoreCreateBinary();
    if (!sess.done) return;
    sess.spk_rate = 24000;
    sess.audio = s_audio.ready;      // 有底座 → TTS 用扬声器播
    if (sess.audio)
        sess.rb = xRingbufferCreate(XZ_AUDIO_RB_SIZE, RINGBUF_TYPE_BYTEBUF);

    esp_websocket_client_handle_t ws = ws_open(&sess);
    if (!ws) {
        vSemaphoreDelete(sess.done);
        if (sess.rb) vRingbufferDelete(sess.rb);
        model_state(UI_BOT_ST_ERROR);
        model_xz_status(UI_BOT_XZ_ERROR);
        return;
    }
    if (!send_hello(ws) || !wait_hello(&sess)) {
        session_close(&sess, ws);
        return;
    }
    if (sess.audio && s_audio.talk_start() != ESP_OK) sess.audio = false;

    // 发问题：listen detect（xiaozhi 文本触发 LLM 的路径）
    {
        model_state(UI_BOT_ST_THINKING);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "session_id", sess.session_id);
        cJSON_AddStringToObject(d, "type", "listen");
        cJSON_AddStringToObject(d, "state", "detect");
        cJSON_AddStringToObject(d, "text", text);
        char *s = cJSON_PrintUnformatted(d);
        esp_websocket_client_send_text(ws, s, strlen(s), portMAX_DELAY);
        cJSON_free(s);
        cJSON_Delete(d);
        ESP_LOGI(XZ_TAG, ">> %s", text);
    }

    if (sess.audio) {
        playback_loop(&sess, ws);    // tts stop 且播空才返回
        s_audio.talk_stop();
    } else {
        int wait_ms = 0;             // 纯文本：等到 tts stop / 断开 / 超时
        while (!sess.stop && wait_ms < XZ_SESSION_TIMEOUT_MS) {
            if (xSemaphoreTake(sess.done, 0) == pdTRUE) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }
    }
    session_close(&sess, ws);
}

// 一轮语音对话：连 → hello → listen start(manual) → 麦克风推流直到 PTT
// 松开 → listen stop → 播 TTS → 收尾。
static void voice_session(void)
{
    ui_model_t *m = ui_model_get();
    m->bot_chat_user[0] = 0;
    m->bot_chat_reply[0] = 0;
    model_state(UI_BOT_ST_CONNECTING);
    m->bot_emotion = UI_BOT_EMO_NEUTRAL;
    s_ptt_down = true;               // 从此刻起收音（按下瞬间可能略早于任务响应）

    xz_session_t sess = {0};
    sess.done = xSemaphoreCreateBinary();
    if (!sess.done) { s_ptt_down = false; return; }
    sess.spk_rate = 24000;
    sess.audio = true;
    sess.rb = xRingbufferCreate(XZ_AUDIO_RB_SIZE, RINGBUF_TYPE_BYTEBUF);

    esp_websocket_client_handle_t ws = ws_open(&sess);
    if (!ws) {
        vSemaphoreDelete(sess.done);
        vRingbufferDelete(sess.rb);
        model_state(UI_BOT_ST_ERROR);
        model_xz_status(UI_BOT_XZ_ERROR);
        s_ptt_down = false;
        return;
    }
    if (!send_hello(ws) || !wait_hello(&sess) ||
        s_audio.talk_start() != ESP_OK) {
        ESP_LOGE(XZ_TAG, "语音会话建立失败");
        s_audio.talk_stop();
        session_close(&sess, ws);
        s_ptt_down = false;
        return;
    }

    // listen start（manual 模式：设备端按键控制起点）
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "session_id", sess.session_id);
        cJSON_AddStringToObject(d, "type", "listen");
        cJSON_AddStringToObject(d, "state", "start");
        cJSON_AddStringToObject(d, "mode", "manual");
        char *s = cJSON_PrintUnformatted(d);
        esp_websocket_client_send_text(ws, s, strlen(s), portMAX_DELAY);
        cJSON_free(s);
        cJSON_Delete(d);
    }
    model_state(UI_BOT_ST_LISTENING);

    // 麦克风推流：60ms 一帧，编码即发；PTT 松开 / 超时 / 断开退出
    void *enc = NULL;
    esp_opus_enc_config_t ec = {
        .sample_rate      = XZ_SAMPLE_RATE,
        .channel          = 1,
        .bits_per_sample  = 16,
        .bitrate          = 24000,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity       = 0,
        .enable_fec       = false,
        .enable_dtx       = false,
        .enable_vbr       = false,
    };
    int64_t t0 = esp_timer_get_time();
    if (esp_opus_enc_open(&ec, sizeof(ec), &enc) == ESP_AUDIO_ERR_OK) {
        static int16_t mic[XZ_FRAME_SAMPLES];
        static uint8_t obuf[XZ_ENC_BUF_SIZE];
        while (s_ptt_down &&
               esp_timer_get_time() - t0 < (int64_t)XZ_MIC_MAX_MS * 1000) {
            if (xSemaphoreTake(sess.done, 0) == pdTRUE) break;
            int n = s_audio.mic_read(mic, XZ_FRAME_SAMPLES);
            if (n <= 0) break;
            esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)mic,
                                            .len = n * 2 };
            esp_audio_enc_out_frame_t out = { .buffer = obuf,
                                              .len = sizeof(obuf) };
            if (esp_opus_enc_process(enc, &in, &out) != ESP_AUDIO_ERR_OK)
                continue;
            esp_websocket_client_send_bin(ws, (char *)obuf, out.encoded_bytes,
                                          portMAX_DELAY);
        }
        esp_opus_enc_close(enc);
    } else {
        ESP_LOGE(XZ_TAG, "opus enc open fail");
    }
    s_ptt_down = false;

    // listen stop → 服务端做 STT → LLM → TTS
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "session_id", sess.session_id);
        cJSON_AddStringToObject(d, "type", "listen");
        cJSON_AddStringToObject(d, "state", "stop");
        char *s = cJSON_PrintUnformatted(d);
        esp_websocket_client_send_text(ws, s, strlen(s), portMAX_DELAY);
        cJSON_free(s);
        cJSON_Delete(d);
        ESP_LOGI(XZ_TAG, ">> [语音] 收音结束，等回答");
    }

    playback_loop(&sess, ws);        // tts stop 且播空才返回
    s_audio.talk_stop();
    session_close(&sess, ws);
}


// ------------ 任务与对外入口 ------------------------------------------
void xiaozhi_task(void *arg)
{
    (void)arg;
    uuid_ensure();
    ESP_LOGI(XZ_TAG, "Client-Id=%s", s_uuid);

    // 等 WiFi
    xEventGroupWaitBits(s_wifi_events, BIT_WIFI_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    // 读 NVS websocket 配置；没有 **绑定标记** → 激活流程。
    // 旧版固件的 bug：未绑定时把服务端的 test-token 占位配置也存了 NVS，
    // 导致每次启动都跳过激活、激活码永不见天日 —— 缺 bound 标记一律重激活。
    nvs_handle_t nh;
    bool have_cfg = false;
    if (nvs_open(XZ_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        size_t l1 = sizeof(s_wss_url), l2 = sizeof(s_wss_token);
        uint8_t bound = 0;
        bool have_str = nvs_get_str(nh, XZ_NVS_URL, s_wss_url, &l1) == ESP_OK &&
                        nvs_get_str(nh, XZ_NVS_TOKEN, s_wss_token, &l2) == ESP_OK;
        nvs_get_u8(nh, XZ_NVS_BOUND, &bound);
        have_cfg = have_str && bound == 1;
        if (have_str && !have_cfg)
            ESP_LOGW(XZ_TAG, "NVS 配置无绑定标记（旧固件存的占位值）→ 重新激活");
        nvs_close(nh);
    }
    if (!have_cfg) {
        if (!activate_flow()) {
            model_xz_status(UI_BOT_XZ_ERROR);
            model_state(UI_BOT_ST_ERROR);
            vTaskDelete(NULL);
            return;
        }
    }
    model_xz_status(UI_BOT_XZ_READY);
    model_state(UI_BOT_ST_IDLE);

    // opus 编解码器注册（无音频底座时跳过，省堆）
    if (s_audio.ready) {
        esp_opus_enc_register();
        esp_opus_dec_register();
    }

    for (;;) {
        if (s_voice_pending) {           // KEY 按住说话优先
            s_voice_pending = false;
            s_chat_busy = true;
            voice_session();
            s_chat_busy = false;
        } else if (s_pending) {
            static char text[XZ_TEXT_MAX];
            taskENTER_CRITICAL(&s_pending_mux);
            strlcpy(text, s_pending_text, sizeof(text));
            s_pending = false;
            taskEXIT_CRITICAL(&s_pending_mux);
            s_chat_busy = true;
            chat_session(text);
            s_chat_busy = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

bool NetBsp_XiaozhiChat(const char *text)
{
    if (!text || !text[0] || s_chat_busy) return false;
    taskENTER_CRITICAL(&s_pending_mux);
    if (s_pending) { taskEXIT_CRITICAL(&s_pending_mux); return false; }
    strlcpy(s_pending_text, text, sizeof(s_pending_text));
    s_pending = true;
    taskEXIT_CRITICAL(&s_pending_mux);
    return true;
}

int8_t NetBsp_XiaozhiStatus(void)
{
    return s_xz_status;
}
