# net_bsp — 网络 + 天气

ESP32 上的 WiFi / HTTP 配网 / 天气拉取后台。天气固定走 **QWeather**。

## 文件结构（组件内拆分）

对外 API 仍只在 `include/net_bsp.h`；内部按职责拆为多文件：

| 文件 | 职责 |
|---|---|
| `src/net_bsp.c` | 入口 `NetBsp_Start` + 组件内共享状态定义 + NVS 配置读写 + 无网看门狗 tick |
| `src/net_wifi.c` | WiFi STA/AP 事件、公共初始化、STA/AP 配置填充、SNTP 校时 |
| `src/net_portal.c` | SoftAP 配网门户 HTTP 服务器（表单/扫描/保存 handler） |
| `src/portal_page.h` | 配网门户 HTML/JS 模板（大字符串常量） |
| `src/net_weather.c` | QWeather API 拉取（HTTP + gzip 解压 + JSON 抽取）+ weather_task |
| `src/net_calendar.c` | 日历配置存 SD 卡（原子写：temp + fsync + rename） |
| `src/net_internal.h` | 组件内共享声明（extern 状态 + 内部函数原型） |

## 配置（NVS 命名空间 "rlcd_cfg"）

| 字段 | 用途 |
|---|---|
| ssid / pass | STA 连接凭据 |
| city | QWeather 城市名或 LocationID（如 "新郑" / "101180106"）|
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

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 配置表单（网络+天气 / 日历 双标签页，附 WiFi 扫描）|
| GET | `/scan` | 触发一次 WiFi 扫描，返回 JSON `[{ssid,rssi,auth}]` |
| POST | `/save` | 保存网络/天气配置到 NVS 并重启 |
| POST | `/save_cal` | 日历数据存 SD 卡，立即应用，不重启 |
