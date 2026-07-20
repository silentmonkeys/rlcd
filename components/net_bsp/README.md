# net_bsp — 网络 + 天气

ESP32 上的 WiFi / HTTP 配网 / 天气拉取后台。

## 配置（NVS 命名空间 "rlcd_cfg"）

| 字段 | 用途 |
|---|---|
| ssid / pass | STA 连接凭据 |
| city | 天气查询城市（URL-safe；QWeather 推荐 LocationID 如 101010100）|
| weather_provider | "wttr" / "qweather" / "openweather" |
| weather_apikey | wttr 留空；qweather / openweather 用 |
| weather_host | 仅 qweather（每个开发者独立 API 域名，如 xxx.qweatherapi.com）|

## 天气 provider

- wttr：`http://wttr.in/<city>?format=%l:%t:%C&M`，纯文本解析
- qweather：`https://<host>/v7/weather/now?location=<city>&key=<apikey>`，JSON 简易抽取（`json_get()`），需 esp_crt_bundle_attach
- openweather：TODO skeleton

## 公开 API

```c
void NetBsp_Start(void);
void NetBsp_TriggerWeatherFetch(void);
bool NetBsp_LoadConfig(net_config_t *out);
bool NetBsp_SaveConfig(const char *in);
void NetBsp_ForgetWifi(void);
```
