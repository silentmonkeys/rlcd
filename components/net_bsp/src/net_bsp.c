// net_bsp —— WiFi 连接、SoftAP 配置门户、扫描、SNTP 校时、天气 API 轮询
//
// 启动流程：
//   [boot]
//     ├── NVS 无 ssid   → AP-only，配置门户 http://192.168.4.1
//     └── NVS 有 ssid   → APSTA 同开
//                          - STA 后台反复尝试连（ALL_CHANNEL_SCAN + PMF-capable + WPA3-SAE）
//                          - AP 仍开，方便手机随时改配置
//                          - STA 拿到 IP：SNTP 启动 + 天气轮询 + 打印 STA IP 供门户访问
//
// HTTP endpoints：
//   GET /        —— 配置表单（附扫描按钮）
//   GET /scan    —— 触发一次 WiFi 扫描，返回 JSON: [{ssid,rssi,auth}]
//   POST /save   —— 保存到 NVS 并 esp_restart
//
// 关于 "STA 连上后 192.168.4.1 访问不到" 的说明：
//   ESP32-S3 只有一个 2.4 GHz 射频，APSTA 模式下 AP 会被强制切到 STA 所在信道。
//   手机原本连着 "RLCD-Setup"（ch1），一旦 AP 挪信道，手机会掉线。
//   解决方式：改用 **STA IP** 访问同一个配置页（我们把它打到日志 + LVGL），
//   或者手机重新连 "RLCD-Setup" 让它跟着新信道回来。

#include "net_bsp.h"
#include "ui_model.h"
#include "ui_home.h"
#include "ui_pages.h"
#include "ui_calendar.h"
#include "lvgl_bsp.h"
#include "user_config.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <esp_crt_bundle.h>

// gzip 解压 —— QWeather 强制返回 gzip；esp_http_client 不自动解压。
// 用 espressif/zlib 组件（IDF v6 不再自带 zlib，走 component manager 拉取）。
#include "zlib.h"

static const char *TAG = "net_bsp";

// ------------ 全局状态 --------------------------------------------
static net_config_t   s_cfg;
static bool           s_cfg_loaded = false;
static EventGroupHandle_t s_wifi_events;
#define BIT_WIFI_CONNECTED  BIT0
static int            s_retry_count = 0;
static httpd_handle_t s_httpd = NULL;
static esp_netif_t   *s_netif_sta = NULL;
static esp_netif_t   *s_netif_ap  = NULL;
static bool           s_wifi_common_inited = false;
static bool           s_want_sta   = false;
static SemaphoreHandle_t s_scan_mux = NULL;   // 保护 esp_wifi_scan_* 一次一个用户

// 正在做 web 扫描 —— disconnect 事件里不要再自动 reconnect，
// 让 STA 静下来给 esp_wifi_scan_start 让出信道。扫描完再放开。
// （scan_get 里的定义会重复吗？——用同一符号，把 scan_get 里的 static 删掉。）
static volatile bool s_scanning = false;

// 无网检测：STA disconnected 时间戳（单位：us）。0 = 当前已连接或从未启动 STA
static int64_t s_last_disconnected_us = 0;
// 已经因为超时把 SETUP 弹出过一次 —— 避免每次 tick 都重复 apply
static bool    s_offline_setup_shown  = false;
#define OFFLINE_SETUP_THRESHOLD_US   ((int64_t)60 * 1000 * 1000)   // 60s

// ------------ NVS ----------------------------------------------------
bool NetBsp_LoadConfig(net_config_t *out)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nh) != ESP_OK) return false;
    size_t sz;
    memset(out, 0, sizeof(*out));
#define LOAD_STR(field)                                                     \
    do { sz = sizeof(out->field); nvs_get_str(nh, #field, out->field, &sz); } while (0)
    LOAD_STR(ssid);
    LOAD_STR(pass);
    LOAD_STR(city);
    LOAD_STR(weather_provider);
    LOAD_STR(weather_apikey);
    LOAD_STR(weather_host);
#undef LOAD_STR
    nvs_close(nh);
    return out->ssid[0] != 0;
}

bool NetBsp_SaveConfig(const net_config_t *in)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) return false;
    nvs_set_str(nh, "ssid",             in->ssid);
    nvs_set_str(nh, "pass",             in->pass);
    nvs_set_str(nh, "city",             in->city);
    nvs_set_str(nh, "weather_provider", in->weather_provider);
    nvs_set_str(nh, "weather_apikey",   in->weather_apikey);
    nvs_set_str(nh, "weather_host",     in->weather_host);
    esp_err_t err = nvs_commit(nh);
    nvs_close(nh);
    return err == ESP_OK;
}

void NetBsp_ForgetWifi(void)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_erase_key(nh, "ssid");
    nvs_erase_key(nh, "pass");
    nvs_commit(nh);
    nvs_close(nh);
}

// ------------ 日历配置：存 SD 卡 /sdcard/rlcd/calendar.conf --------------
// 不写 NVS/flash，改存 SD，保存后不重启。文件按行存，每行一条：
//   M <MM-DD>          标注日期
//   E <MM-DD>=<内容>   预定内容
//   L <文字>           随机标签
// 无 SD 时禁止写入（web 侧也会拒绝）。
#define CAL_DIR        "/sdcard/rlcd"
#define CAL_FILE       "/sdcard/rlcd/calendar.conf"

// 读 SD 上的日历文件，拆成三段字符串（原有 setter 格式）。
//   marks:  "MM-DD,..."     events: "MM-DD=内容;..."     labels: "文字;..."
// 三个缓冲各自会被清空后填充。SD 未挂载 / 文件不存在 → 三段均为空，返回 false。
static bool cal_read_file(char *marks, size_t nm, char *events, size_t ne,
                          char *labels, size_t nl)
{
    marks[0] = events[0] = labels[0] = 0;
    if (!ui_model_get()->sd_mounted) return false;
    FILE *f = fopen(CAL_FILE, "r");
    if (!f) return false;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        char tag = line[0];
        const char *val = line + 1;
        while (*val == ' ') val++;
        if (tag == 'M') {
            if (marks[0]) strncat(marks, ",", nm - strlen(marks) - 1);
            strncat(marks, val, nm - strlen(marks) - 1);
        } else if (tag == 'E') {
            if (events[0]) strncat(events, ";", ne - strlen(events) - 1);
            strncat(events, val, ne - strlen(events) - 1);
        } else if (tag == 'L') {
            if (labels[0]) strncat(labels, ";", nl - strlen(labels) - 1);
            strncat(labels, val, nl - strlen(labels) - 1);
        }
    }
    fclose(f);
    return true;
}

// 从 SD 加载日历配置并推入 UI。SD 未挂载 / 文件不存在 → 静默（UI 保持空）。
static void cal_load_from_sd(void)
{
    static char marks[128], events[512], labels[512];
    if (!cal_read_file(marks, sizeof(marks), events, sizeof(events),
                       labels, sizeof(labels))) {
        ESP_LOGI(TAG, "cal: 无 SD 或文件不存在，日历配置为空");
        return;
    }
    if (Lvgl_lock(200)) {
        ui_calendar_set_marks(marks);
        ui_calendar_set_events(events);
        ui_calendar_set_labels(labels);
        ui_pages_apply_locked();
        Lvgl_unlock();
    }
    ESP_LOGI(TAG, "cal: 已加载 marks='%s' events='%s' labels='%s'", marks, events, labels);
}

// 把三段字符串写回 SD 文件。成功返回 true。SD 未挂载返回 false。
// marks: "MM-DD,..."；events: "MM-DD=内容;..."；labels: "文字;..."
static bool cal_save_to_sd(const char *marks, const char *events, const char *labels)
{
    if (!ui_model_get()->sd_mounted) return false;
    mkdir(CAL_DIR, 0777);   // 确保目录存在（已存在无害）
    FILE *f = fopen(CAL_FILE, "w");
    if (!f) {
        ESP_LOGW(TAG, "cal: 写 %s 失败（SD 只读/满？）", CAL_FILE);
        return false;
    }
    // 逐条拆分写行 —— 用局部拷贝做 strtok
    char buf[512];
    if (marks && marks[0]) {
        strncpy(buf, marks, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) fprintf(f, "M %s\n", t);
    }
    if (events && events[0]) {
        strncpy(buf, events, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *t = strtok(buf, ";"); t; t = strtok(NULL, ";")) fprintf(f, "E %s\n", t);
    }
    if (labels && labels[0]) {
        strncpy(buf, labels, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *t = strtok(buf, ";"); t; t = strtok(NULL, ";")) fprintf(f, "L %s\n", t);
    }
    fclose(f);
    return true;
}

// ------------ SNTP 校时 ----------------------------------------------
static bool s_sntp_started = false;

static void sntp_sync_cb(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm lt;
    localtime_r(&now, &lt);
    ESP_LOGI(TAG, "SNTP sync: %04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static void sntp_start_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    // 中国大陆时区（无夏令时）；改地区就改这里
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = sntp_sync_cb;
    cfg.start   = true;
    esp_netif_sntp_init(&cfg);
    // 备用服务器（次要优先级）
    esp_sntp_setservername(1, "ntp1.aliyun.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    ESP_LOGI(TAG, "SNTP started (TZ=CST-8)");
}

// ------------ WiFi 事件 ----------------------------------------------
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ui_model_t *m = ui_model_get();
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA_START → connect()");
                if (s_last_disconnected_us == 0) s_last_disconnected_us = esp_timer_get_time();
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                m->wifi_connected = false;
                m->wifi_rssi = 0;
                m->ip[0] = 0;       // 清 IP 显示
                m->ssid[0] = 0;     // 清 SSID —— 设备信息页会回落到本机 AP 名
                s_retry_count++;
                if (s_last_disconnected_us == 0) s_last_disconnected_us = esp_timer_get_time();
                wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
                ESP_LOGW(TAG, "STA disconnected (reason=%d), retry #%d",
                         ev ? ev->reason : -1, s_retry_count);
                // 扫描进行中不要抢占—— scan_get 结束后会自己调 esp_wifi_connect()
                if (!s_scanning) esp_wifi_connect();
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "AP: station joined");
                break;
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_last_disconnected_us = 0;
        s_offline_setup_shown  = false;
        m->wifi_connected = true;
        m->ap_active = false;    // STA 连上了，配网页自动隐藏
        m->setup_dismissed = false;  // 下次断网 60s 后可以再弹
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            m->wifi_rssi = ap.rssi;
            // 回填 SSID —— 设备信息页 NETWORK 字段
            strncpy(m->ssid, (const char *)ap.ssid, sizeof(m->ssid) - 1);
            m->ssid[sizeof(m->ssid) - 1] = 0;
        }
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        // 回填 IP —— 设备信息页 IP ADDRESS 字段
        snprintf(m->ip, sizeof(m->ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP " IPSTR " rssi=%d  → 设置页也可从 http://" IPSTR "/ 访问",
                 IP2STR(&ev->ip_info.ip), (int)m->wifi_rssi, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, BIT_WIFI_CONNECTED);
        sntp_start_once();
    }
}

// ------------ WiFi 公共初始化（只跑一次） ------------------------------
static void wifi_common_init(void)
{
    if (s_wifi_common_inited) return;
    s_wifi_common_inited = true;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,
                                                        wifi_evt, NULL, NULL));
}

// ------------ 配置门户 HTML/JS --------------------------------------
// 页面结构：顶部通用（WiFi 扫描）+ 双标签页（网络 / 日历）；每条配置逐条增删。
// JS 提交前把列表项编码成后端的 CSV / 分号格式，后端 NVS 代码完全不变。
static const char PAGE_TEMPLATE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>RLCD Setup</title>"
"<style>"
"body{font-family:sans-serif;max-width:440px;margin:20px auto;padding:0 12px}"
"input,select,button{width:100%%;padding:8px;margin:4px 0;box-sizing:border-box;font-size:15px}"
"label{font-weight:bold;display:block;margin:12px 0 4px}"
".btn{padding:10px;font-size:16px}"
".btn.sm{padding:6px 10px;font-size:14px;width:auto}"
".scan{border:1px solid #ccc;border-radius:6px;max-height:180px;overflow:auto;margin-bottom:12px}"
".scan .row{padding:8px 12px;border-bottom:1px solid #eee;cursor:pointer;display:flex;justify-content:space-between}"
".scan .row:hover{background:#f2f7ff}"
".scan .row:last-child{border-bottom:none}"
".lock{color:#888;font-size:12px}"
".hint{color:#888;font-size:12px}"
/* 标签页 */
".tabs{display:flex;border-bottom:2px solid #eee;margin-bottom:16px}"
".tab{padding:8px 16px;cursor:pointer;opacity:.5}"
".tab.active{opacity:1;border-bottom:2px solid #000}"
".page{display:none}"
".page.active{display:block}"
/* 已添加条目：一行 flex，内容占满，删除按钮固定窄 */
".item{display:flex;align-items:center;gap:8px;margin:6px 0;padding:6px 10px;"
      "background:#f6f6f6;border-radius:6px}"
".item .txt{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".item .x{flex:none;width:32px;padding:4px 0;font-size:16px;line-height:1;color:#c00;"
         "background:none;border:1px solid #ddd;border-radius:6px;cursor:pointer}"
/* 添加行：输入占满，按钮固定窄 */
".add{display:flex;gap:8px;align-items:center;margin:6px 0}"
".add input{flex:1;min-width:0;margin:0}"
".add .plus{flex:none;width:64px;margin:0;padding:8px 0}"
".toggle{display:flex;align-items:center;gap:8px;margin:14px 0}"
".toggle input{width:auto;margin:0}"
".disabled{opacity:.4;pointer-events:none}"
"</style></head><body><h2>RLCD Setup</h2>"

/* --- 标签页切换栏（放最上方） --- */
"<div class=tabs>"
"<div class=tab active onclick=\"showTab(0)\">网络与天气</div>"
"<div class=tab onclick=\"showTab(1)\">日历页</div>"
"</div>"

/* ===== 第 0 页：网络与天气（独立 form，保存后重启） ===== */
"<div id=p0 class=page active>"
"<button type=button onclick=\"doScan()\">扫描附近 WiFi</button>"
"<div id=scan class=scan style=\"display:none\"></div>"
"<form method=POST action=/save>"
"<label>WiFi SSID</label><input id=ssid name=ssid value='%s' maxlength=32 required>"
"<label>WiFi Password</label><input name=pass type=password value='%s' maxlength=64>"
"<label>城市 (天气查询用)</label><input name=city value='%s' maxlength=31 placeholder=新郑>"
"<label>天气 Provider</label>"
"<select name=provider><option value=qweather selected>QWeather</option></select>"
"<label>QWeather API Key</label><input name=apikey value='%s' maxlength=63>"
"<label>QWeather API Host</label>"
"<input name=host value='%s' maxlength=63 placeholder=xxx.re.qweatherapi.com>"
"<button type=submit class=btn style=margin-top:20px>保存网络并重启</button>"
"</form>"
"<p class=hint>保存后设备会重启并加入所选 WiFi。ESP32-S3 只支持 2.4GHz。</p>"
"</div>"

/* ===== 第 1 页：日历页（存 SD 卡，保存后不重启） ===== */
"<div id=p1 class=page>"
"<div id=nosd class=hint style=\"color:#c00;display:none\">未检测到 SD 卡，无法保存日历数据。请插卡后刷新页面。</div>"
"<div id=calbody>"
/* 标记日期 */
"<label>标记日期（日历上黑方块高亮）</label>"
"<div id=marks></div>"
"<div class=add>"
"<input type=date id=new_mark>"
"<button type=button class=\"btn plus\" onclick=\"addMark()\">添加</button>"
"</div>"
/* 预定内容 */
"<hr><label>预定内容（当天底部固定显示的文字）</label>"
"<div id=events></div>"
"<div class=add>"
"<input type=date id=evt_date style=flex:none;width:150px>"
"<input type=text id=evt_text placeholder=内容文字 maxlength=28>"
"<button type=button class=\"btn plus\" onclick=\"addEvent()\">添加</button>"
"</div>"
/* 随机标签 */
"<hr>"
"<div class=toggle>"
"<input type=checkbox id=labels_on onchange=\"toggleLabels()\">"
"<span>启用随机标签（无预定的日子，底部按日期轮选一条）</span>"
"</div>"
"<div id=labels_box style=display:none>"
"<div id=labels></div>"
"<div class=add>"
"<input type=text id=new_label placeholder=标签文字 maxlength=28>"
"<button type=button class=\"btn plus\" onclick=\"addLabel()\">添加</button>"
"</div>"
"</div>"
"<button type=button class=btn style=margin-top:20px onclick=\"saveCal()\">保存日历（写入 SD，不重启）</button>"
"<div id=calmsg class=hint style=margin-top:8px></div>"
"</div>"  /* calbody */
"</div>"  /* p1 */

"<script>"
"var SD_OK=%d;"
"var INIT_MARKS='%s', INIT_EVENTS='%s', INIT_LABELS='%s';"
"var marks=[], events=[], labels=[];"
"function $(id){return document.getElementById(id);}"
"function showTab(n){"
"  document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',i==n));"
"  document.querySelectorAll('.page').forEach((p,i)=>p.classList.toggle('active',i==n));"
"}"
"function esc(s){return (''+s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/'/g,'&#39;');}"
/* 通用渲染：list + 取文本函数 → 每行 [内容][删除] */
"function renderBox(id,items,txt){"
"  $(id).innerHTML=items.map((it,i)=>"
"    '<div class=item><span class=txt>'+esc(txt(it))+'</span>'+"
"    '<button type=button class=x onclick=\"del(\\''+id+'\\','+i+')\">×</button></div>').join('');"
"}"
"function del(id,i){({marks:marks,events:events,labels:labels})[id].splice(i,1);renderAll();}"
"function renderAll(){"
"  renderBox('marks',marks,d=>d);"
"  renderBox('events',events,e=>e.date+'  \\u2192  '+e.text);"
"  renderBox('labels',labels,t=>t);"
"}"
"function fmtMMDD(d){return d?d.slice(5):'';}"
"function addMark(){"
"  var v=fmtMMDD($('new_mark').value);"
"  if(!v||marks.indexOf(v)>=0)return;"
"  marks.push(v);marks.sort();$('new_mark').value='';renderAll();"
"}"
"function addEvent(){"
"  var d=fmtMMDD($('evt_date').value),t=$('evt_text').value.trim();"
"  if(!d||!t)return;"
"  for(var i=0;i<events.length;i++)if(events[i].date==d){events[i].text=t;renderAll();return;}"
"  events.push({date:d,text:t});events.sort((a,b)=>a.date<b.date?-1:1);"
"  $('evt_date').value='';$('evt_text').value='';renderAll();"
"}"
"function toggleLabels(){$('labels_box').style.display=$('labels_on').checked?'block':'none';}"
"function addLabel(){"
"  var v=$('new_label').value.trim();"
"  if(!v||labels.indexOf(v)>=0)return;"
"  labels.push(v);$('new_label').value='';renderAll();"
"}"
/* 日历 AJAX 保存到 /save_cal（写 SD，不重启） */
"function saveCal(){"
"  var fd=new URLSearchParams();"
"  fd.append('cal_marks',marks.join(','));"
"  fd.append('cal_events',events.map(e=>e.date+'='+e.text).join(';'));"
"  fd.append('cal_labels',$('labels_on').checked?labels.join(';'):'');"
"  var msg=$('calmsg');msg.style.color='#888';msg.textContent='保存中…';"
"  fetch('/save_cal',{method:'POST',body:fd}).then(r=>r.text().then(t=>({ok:r.ok,t:t}))).then(o=>{"
"    msg.style.color=o.ok?'#080':'#c00';msg.textContent=o.ok?'已保存到 SD 卡':('保存失败：'+o.t);"
"  }).catch(e=>{msg.style.color='#c00';msg.textContent='请求失败：'+e;});"
"}"
/* 初始化：解析 INIT 字符串渲染 */
"function parseMarks(s){return s?s.split(',').filter(x=>x.length>=5).sort():[];}"
"function parseEvents(s){return s?s.split(';').filter(x=>x.indexOf('=')>0).map(p=>{"
"  var i=p.indexOf('=');return {date:p.slice(0,i),text:p.slice(i+1)};"
"}).sort((a,b)=>a.date<b.date?-1:1):[];}"
"function parseLabels(s){return s?s.split(';').filter(x=>x.length>0):[];}"
"marks=parseMarks(INIT_MARKS);events=parseEvents(INIT_EVENTS);labels=parseLabels(INIT_LABELS);"
"$('labels_on').checked=INIT_LABELS.length>0;toggleLabels();renderAll();"
/* 无 SD → 禁用日历编辑 */
"if(!SD_OK){$('nosd').style.display='block';$('calbody').classList.add('disabled');}"
/* WiFi 扫描 */
"function doScan(){"
"  var box=$('scan');box.style.display='block';"
"  box.innerHTML='<div class=row>扫描中…</div>';"
"  fetch('/scan').then(r=>r.json()).then(list=>{"
"    if(!list.length){box.innerHTML='<div class=row>未发现网络</div>';return;}"
"    box.innerHTML=list.map(a=>{"
"      var lock=a.auth?'🔒':'  ';"
"      var bars=a.rssi>-55?'▂▄▆█':a.rssi>-70?'▂▄▆ ':a.rssi>-80?'▂▄  ':'▂   ';"
"      var s=esc(a.ssid);"
"      return \"<div class=row onclick=\\\"pick('\"+s+\"')\\\">\"+"
"        \"<span>\"+lock+' '+s+\"</span><span class=lock>\"+bars+' '+a.rssi+\"dBm</span></div>\";"
"    }).join('');"
"  }).catch(e=>{box.innerHTML='<div class=row>扫描失败：'+e+'</div>';});"
"}"
"function pick(s){$('ssid').value=s;}"
"</script>"

"</body></html>";

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

// 全局标记：正在做 web 扫描 —— 在文件级声明，wifi_evt 的 disconnect 处理里查
// 询它决定要不要抢占 esp_wifi_connect()。
// GET /scan  → JSON [{ssid,rssi,auth}]
static esp_err_t scan_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");

    if (!s_wifi_common_inited) {
        ESP_LOGW(TAG, "scan: wifi not inited");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    // 一次一个客户端扫描
    if (xSemaphoreTake(s_scan_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "scan: mux busy");
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
        ESP_LOGW(TAG, "scan_start fail: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        s_scanning = false;
        // 让 STA 恢复重连（若有凭据）
        if (s_want_sta) esp_wifi_connect();
        xSemaphoreGive(s_scan_mux);
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "scan done: %u APs", (unsigned)n);
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
    parse_form_field(body, "provider", c.weather_provider, sizeof(c.weather_provider));
    parse_form_field(body, "apikey",   c.weather_apikey,   sizeof(c.weather_apikey));
    parse_form_field(body, "host",     c.weather_host,     sizeof(c.weather_host));
    if (c.weather_provider[0] == 0) strcpy(c.weather_provider, "qweather");

    if (c.ssid[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }
    bool save_ok = NetBsp_SaveConfig(&c);
    // 立刻读回验证是否真正落盘
    char vp[16] = {0}; size_t vsz = sizeof(vp);
    nvs_handle_t vh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &vh) == ESP_OK) {
        nvs_get_str(vh, "weather_provider", vp, &vsz);
        nvs_close(vh);
    }
    ESP_LOGI(TAG, "config saved ok=%d ssid='%s' provider='%s' host='%s' city='%s' key=%s | verify_read='%s', rebooting…",
             save_ok, c.ssid, c.weather_provider, c.weather_host, c.city,
             c.weather_apikey[0] ? "set" : "EMPTY", vp[0] ? vp : "EMPTY");
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
    if (ok && Lvgl_lock(200)) {
        // 立即应用到 UI（不重启）
        ui_calendar_set_marks(marks);
        ui_calendar_set_events(events);
        ui_calendar_set_labels(labels);
        ui_pages_apply_locked();
        Lvgl_unlock();
    }
    ESP_LOGI(TAG, "cal saved ok=%d marks='%s' events='%s' labels='%s'",
             ok, marks, events, labels);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, ok ? "OK" : "写 SD 失败");
    return ESP_OK;
}

static void config_httpd_start(void)
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

// ------------ WiFi 启动 -----------------------------------------------
static void fill_ap_config(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    strcpy((char *) wc->ap.ssid, "RLCD-Setup");
    wc->ap.ssid_len       = strlen("RLCD-Setup");
    wc->ap.channel        = 1;              // 会在 APSTA 下自动跟随 STA 信道
    wc->ap.max_connection = 3;
    wc->ap.authmode       = WIFI_AUTH_OPEN;
}

static void fill_sta_config(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    strncpy((char *) wc->sta.ssid,     s_cfg.ssid, sizeof(wc->sta.ssid) - 1);
    strncpy((char *) wc->sta.password, s_cfg.pass, sizeof(wc->sta.password) - 1);
    // ALL_CHANNEL_SCAN + 按信号排序：找不同信道的 AP 更稳
    wc->sta.scan_method         = WIFI_ALL_CHANNEL_SCAN;
    wc->sta.sort_method         = WIFI_CONNECT_AP_BY_SIGNAL;
    wc->sta.threshold.authmode  = WIFI_AUTH_OPEN;
    wc->sta.threshold.rssi      = -127;
    // WPA2-PMF & WPA3-transition & WPA3-only
    wc->sta.pmf_cfg.capable     = true;
    wc->sta.pmf_cfg.required    = false;
    wc->sta.sae_pwe_h2e         = WPA3_SAE_PWE_BOTH;
    wc->sta.failure_retry_cnt   = 5;
}

// ============================================================================
// Weather —— QWeather API 拉取
// ============================================================================
// 设计原则（跟旧版反着来）：
//   1. 所有缓冲局部分配，函数返回前 free —— 无 static state，无跨调用累积。
//   2. gzip 用 espressif/zlib（inflateInit2 + inflate + inflateEnd），
//      每次调用完整生命周期；zlib 是 mbedtls/lwip 兄弟组件，久经考验。
//   3. HTTP 用 esp_http_client_open / _fetch_headers / _read_response 流式读，
//      不用 event handler —— 数据直接读到本地缓冲。
//   4. 任一步失败立即返回，上层 weather_task 决定重试节奏。
//   5. Now/Daily/Air 三个端点串行拉，Now 是主数据（决定是否显示），
//      Daily/Air 失败只警告不阻塞。
// ----------------------------------------------------------------------------

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
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return out;
    }

    int64_t clen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "http status %d for %.60s", status, url);
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
    ESP_LOGD(TAG, "resp gzipped(header)=%d clen=%lld", (int)out.gzipped, (long long)clen);
    (void)clen;

    // 分配 body。已知长度：按 clen；未知（chunked）：先给 4 KiB，边读边扩。
    size_t cap = (clen > 0 && clen < WX_HTTP_MAX_BYTES) ? (size_t)clen + 1 : 4096;
    out.data = (char *)malloc(cap);
    if (!out.data) {
        ESP_LOGE(TAG, "http body malloc %zu failed", cap);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return out;
    }

    for (;;) {
        if (out.len + 1024 > cap) {
            if (cap >= WX_HTTP_MAX_BYTES) {
                ESP_LOGW(TAG, "http body exceeds %d bytes, truncating", WX_HTTP_MAX_BYTES);
                break;
            }
            size_t new_cap = cap * 2;
            if (new_cap > WX_HTTP_MAX_BYTES) new_cap = WX_HTTP_MAX_BYTES;
            char *p = (char *)realloc(out.data, new_cap);
            if (!p) { ESP_LOGE(TAG, "http body realloc %zu failed", new_cap); break; }
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
        ESP_LOGD(TAG, "resp gzip sniffed by magic");
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
        ESP_LOGW(TAG, "inflateInit2: %d", rc);
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
            ESP_LOGW(TAG, "inflate: %d (total_out=%lu)", rc, (unsigned long)zs.total_out);
            free(out); inflateEnd(&zs);
            return NULL;
        }
        if (zs.avail_out == 0) {
            // 输出满，扩容
            if (cap >= WX_GZIP_MAX_BYTES) {
                ESP_LOGW(TAG, "gzip output exceeds %d bytes", WX_GZIP_MAX_BYTES);
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
        ESP_LOGW(TAG, "qweather code=%s body=%.80s", code[0] ? code : "?", body);
        free(body);
        return NULL;
    }
    (void)body_len;
    return body;
}

// 解析后的城市信息 —— 只在 weather_task 内使用，不再是文件级 static。
typedef struct {
    char id[16];       // LocationID "101180106"
    char name[24];     // 标准中文名 "新郑"
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

    ESP_LOGI(TAG, "geo '%s' → id=%s name=%s",
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


// 手动触发：设置 BIT_WEATHER_KICK，让 weather_task 立即拉一次。
#define BIT_WEATHER_KICK  BIT1
static EventGroupHandle_t s_weather_events = NULL;

// 注：CSV 日志已迁移到 user_app.c —— 用独立的 10 分钟节奏，无网也能记录
// 本地温湿度（哪怕 weather 拉不到，"室外/天气" 字段留空即可）。

static void weather_task(void *arg)
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
            ESP_LOGW(TAG, "weather: host/apikey empty, skip");
        } else {
            // 城市解析：city 变化或从未解析过时重跑一次
            if (city.id[0] == 0) {
                const char *q = s_cfg.city[0] ? s_cfg.city : "Beijing";
                wx_geo_lookup(s_cfg.weather_host, s_cfg.weather_apikey, q, &city);
            }
            if (city.id[0] == 0) {
                ESP_LOGW(TAG, "weather: city resolve failed");
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
        ESP_LOGI(TAG, "weather cycle ok=%d stack_free=%u B", (int)ok,
                 (unsigned)hwm * sizeof(StackType_t));

        // 下一轮：成功→10 min，失败→30 s；期间被 kick 会提前唤醒
        TickType_t wait = ok ? pdMS_TO_TICKS(10 * 60 * 1000) : pdMS_TO_TICKS(30 * 1000);
        xEventGroupWaitBits(s_weather_events, BIT_WEATHER_KICK,
                            pdTRUE, pdFALSE, wait);
    }
}


// 无网看门狗：STA 断开 60s 仍未拿到 IP → 弹出 SETUP 页（前提用户未 dismiss）。
// 用户如果按键 dismiss，本次开机不再自动弹（setup_dismissed=true 由 UI 层置）。
static void offline_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5 * 1000));
        ui_model_t *m = ui_model_get();
        if (m->wifi_connected)           { s_last_disconnected_us = 0; s_offline_setup_shown = false; continue; }
        if (s_last_disconnected_us == 0) continue;
        if (m->setup_dismissed)          continue;   // 用户主动隐藏了本次开机不再弹
        if (s_offline_setup_shown)       continue;

        int64_t now = esp_timer_get_time();
        if (now - s_last_disconnected_us < OFFLINE_SETUP_THRESHOLD_US) continue;

        // 60s 已过 —— 弹 SETUP
        s_offline_setup_shown = true;
        if (Lvgl_lock(200)) {
            m->ap_active = true;
            ui_pages_switch_to_locked(UI_PAGE_SETUP);
            Lvgl_unlock();
        }
        ESP_LOGW(TAG, "offline > 60s → switch to SETUP page");
    }
}

void NetBsp_Start(void)
{
    if (!s_cfg_loaded) {
        strcpy(s_cfg.city, "Beijing");
        strcpy(s_cfg.weather_provider, "qweather");
        s_cfg_loaded = NetBsp_LoadConfig(&s_cfg);
        if (s_cfg.weather_provider[0] == 0) strcpy(s_cfg.weather_provider, "qweather");
        if (s_cfg.city[0] == 0)              strcpy(s_cfg.city, "Beijing");
    }
    ESP_LOGI(TAG, "cfg loaded: host='%s' city='%s' apikey=%s",
             s_cfg.weather_host, s_cfg.city,
             s_cfg.weather_apikey[0] ? "set" : "EMPTY");
    // 日历配置从 SD 卡加载并推入 UI（不再走 NVS）
    cal_load_from_sd();
    s_wifi_events = xEventGroupCreate();
    if (!s_weather_events) s_weather_events = xEventGroupCreate();
    if (!s_scan_mux) s_scan_mux = xSemaphoreCreateMutex();

    wifi_common_init();

    // AP netif（配置门户永远在）
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg;
    fill_ap_config(&ap_cfg);

    s_want_sta = (s_cfg.ssid[0] != 0);

    if (!s_want_sta) {
        ESP_LOGW(TAG, "no ssid in NVS → AP-only setup mode (RLCD-Setup / 192.168.4.1)");
        // 即便无 STA 也开 APSTA —— 这样 /scan 依然可用（scan 需要 STA netif）
        if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        // 一个空 STA 配置，不真的去连（ssid=""）
        wifi_config_t sta_cfg = {};
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    } else {
        ESP_LOGI(TAG, "APSTA — STA target='%s' (all-channel scan, WPA3-SAE ready)", s_cfg.ssid);
        if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

        wifi_config_t sta_cfg;
        fill_sta_config(&sta_cfg);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (!s_want_sta) {
        // AP-only 模式下，STA 处于 idle，不发起 connect —— 但 scan 仍可以由 http 触发
    }

    // 无论何种模式，SoftAP 都在广播 —— 但 SETUP 页默认隐藏：
    //   * 无 NVS 凭据（s_want_sta=false）→ 立即弹 SETUP（用户必须配网）
    //   * 有凭据但暂未连上 → 等 60s 无网看门狗触发才弹
    // STA 连上时 wifi_evt 会把 ap_active 关掉
    {
        ui_model_t *m = ui_model_get();
        strncpy(m->ap_ssid, "RLCD-Setup", sizeof(m->ap_ssid) - 1);
        strncpy(m->ap_ip,   "192.168.4.1", sizeof(m->ap_ip)   - 1);
        m->ap_active = !s_want_sta;   // 无凭据 → 直接进配网态
        m->setup_dismissed = false;
        s_last_disconnected_us = esp_timer_get_time();
        s_offline_setup_shown  = !s_want_sta;   // 无凭据时"已弹"，避免看门狗再次切页
    }

    config_httpd_start();

    if (s_want_sta) {
        // 栈：mbedtls TLS 握手 + esp_crt_bundle 峰值 ~12 KiB，zlib inflate 走
        // heap 分配（内部工作缓冲不占栈），加上局部 url[320] 等，16 KiB 足够。
        xTaskCreatePinnedToCore(weather_task, "weather", 16 * 1024, NULL, 3, NULL, 0);
    }

    // 无网看门狗（无论是否有 SSID 都跑；无凭据时立即已经切到 SETUP）
    xTaskCreatePinnedToCore(offline_watchdog_task, "net_watchdog",
                            3 * 1024, NULL, 2, NULL, 0);

    // 无凭据 → 立即把当前页面切到 SETUP（用户必须走配网流程）
    if (!s_want_sta) {
        if (Lvgl_lock(500)) {
            ui_pages_switch_to_locked(UI_PAGE_SETUP);
            Lvgl_unlock();
        }
    }
}

void NetBsp_TriggerWeatherFetch(void)
{
    if (s_weather_events) {
        xEventGroupSetBits(s_weather_events, BIT_WEATHER_KICK);
    }
}
