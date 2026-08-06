# net_bsp — 网络 + 天气

ESP32 上的 WiFi / HTTP 配网 / 天气拉取后台。天气固定走 **QWeather**。

## 文件结构（组件内拆分）

对外 API 仍只在 `include/net_bsp.h`；内部按职责拆为多文件：

| 文件 | 职责 |
|---|---|
| `src/net_bsp.c` | 入口 `NetBsp_Start` + 组件内共享状态定义 + NVS 配置读写 + 无网看门狗 tick |
| `src/net_wifi.c` | WiFi STA/AP 事件、公共初始化、STA/AP 配置填充、SNTP 校时 |
| `src/net_portal.c` | 管理门户 HTTP 服务器（静态资源 + JSON API） |
| `src/portal_assets.h` | 门户前端资源，**自动生成**（gzip 字节数组，勿手改）|
| `portal/index.html` `portal/style.css` `portal/app.js` | 门户前端**权威源码**（可编辑 / 可 lint）|
| `portal/vendor/chart.umd.min.js` | Chart.js v4.4.8（按需加载，不进首屏）|
| `src/net_weather.c` | QWeather API 拉取（HTTP + gzip 解压 + JSON 抽取）+ weather_task |
| `src/net_calendar.c` | 日历配置存 SD 卡（原子写：temp + fsync + rename） |
| `src/net_internal.h` | 组件内共享声明（extern 状态 + 内部函数原型） |

## 配置（NVS 命名空间 "rlcd_cfg"）

| 字段 | 用途 |
|---|---|
| ssid / pass | STA 连接凭据 |
| city | QWeather 城市名或 LocationID（如 "北京" / "101180106"）|
| weather_apikey | QWeather API Key |
| weather_host | QWeather API Host（每个开发者独立域名，如 xxx.re.qweatherapi.com）；GeoAPI 城市解析也走此主机 |

## 天气流程（QWeather v7）

1. GeoAPI 解析城市：`https://{host}/geo/v2/city/lookup?location=<city>&key=<key>` → LocationID
2. 实况：`https://{host}/v7/weather/now?location=<id>&key=<key>`（主数据，含云量）
3. 每日预报：`https://{host}/v7/weather/7d?location=<id>&key=<key>`（最低/最高温、日出日落、UV）

响应强制 gzip，用 espressif/zlib 解压；JSON 用简易 `wx_json_str()` 扁平抽取，需
`esp_crt_bundle_attach`。成功 10min 一轮，失败 30s 重试；
`NetBsp_TriggerWeatherFetch()` 可提前唤醒。

## 公开 API

```c
void NetBsp_Start(void);
void NetBsp_TriggerWeatherFetch(void);
void NetBsp_OfflineWatchdogTick(void);   // 无网看门狗一次性检查，由 user_app 5s 调用
bool NetBsp_LoadConfig(net_config_t *out);
bool NetBsp_SaveConfig(const net_config_t *in);
void NetBsp_ForgetWifi(void);
```

## HTTP endpoints

前端完全静态（gzip 预压缩的字节数组存 flash rodata，运行时 0 RAM、0 CPU）；
所有数据走 JSON API，页面渲染路径无任何 `snprintf` 拼接。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 管理门户完整页面（HTML+CSS+JS 已内联，gzip 9KB，单请求）|
| GET | `/chart.umd.min.js` | Chart.js（gzip 70KB，仅打开"数据"页时按需加载）|
| GET | `/api/status` | 设备实时状态看板数据（WiFi/传感器/天气/电池/SD/运行时长/版本） |
| GET | `/api/limits` | 容量上限（日历各列表最大条数、单条文字最大字节数） |
| GET | `/api/config` | 当前配置回显 —— **密码 / API Key / API Host 仅回布尔值，不回明文** |
| POST | `/api/config` | 局部更新配置；仅改 SSID/pass 才重启；改天气配置立即生效 |
| GET | `/api/calendar` | 读 SD 上的日历配置（marks/events/labels 三段转 JSON 数组） |
| POST | `/api/calendar` | 校验后写 SD 并立即应用，不重启；超限/非法字符回 400 带原因 |
| GET | `/api/scan` | 触发一次 WiFi 扫描，返回 `{ok:true,aps:[{ssid,rssi,auth}]}` |
| POST | `/api/weather_refresh` | 强制立即拉取天气 |
| GET | `/api/data/csv` | 下载温湿度历史记录 CSV（尾部最新 500 行，1460B 分块流式）|
| POST | `/api/forget` | 清空 WiFi 凭据并重启 |
| POST | `/api/reboot` | 重启设备 |
| * | 404 → 302 `/` | Captive Portal：手机连接 SoftAP 后自动弹出配置页 |

静态资源带 `ETag`（值取资源长度）+ `Cache-Control: no-cache`：复访回 304 不重传，
但烧了新固件后长度变化会让浏览器立刻拿到新页面 —— 不会像纯 `max-age` 那样卡 24 小时。

## 前端开发流程

**权威源码是 `portal/` 下的普通 `.html` / `.css` / `.js`**，不要直接改
`src/portal_assets.h`（自动生成，头部有警告）。改完跑：

```sh
python3 tools/gen_portal.py     # 内联 + gzip → portal_assets.h + 预览页
idf.py build
```

脚本同时产出两个东西：

1. `src/portal_assets.h` —— CSS/JS 内联进 HTML 后整体 gzip。ESP32-S3 的 TCP 窗口
   只有 5760B，把 3 个请求 25KB 压成 1 个请求 9KB 是首屏最大的一笔优化。
2. `simulator/portal_preview.html` —— 同一份 HTML/CSS/JS 注入 mock `fetch`，
   浏览器直接打开就能验证全部交互（折线图、CSV 下载、日历编辑、标签页切换），
   无需烧录。

> 脚本会在内联后断言 `app.js` 确实进了 HTML、且没有残留外链 `<script src=>`。
> 这道检查正是为了防止历史上出现过的事故：手抄的 HTML 丢了 `<script>` 标签，
> 于是 `app.js` 照常被 serve 却从未被浏览器加载 —— 数据不刷新、标签页点不动、
> 右上角永远停在"连接中…"。现在两份产物同源生成，不可能再漂移。
