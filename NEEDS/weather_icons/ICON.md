# 和风天气图标开发参考

## API → 图标映射机制

和风天气 API（`weather-now`）在 `now.icon` 字段返回一个 **3 位数字字符串**（如 `"100"`、`"305"`），与本目录下的 SVG 文件名 **一一对应**：

| API 字段     | 类型   | 示例      | 对应 SVG                                                                                          |
| ------------ | ------ | --------- | ------------------------------------------------------------------------------------------------- |
| `now.icon` | string | `"100"` | `QWeather-Icons-1.8.0/icons/100.svg`（线稿）`QWeather-Icons-1.8.0/icons/100-fill.svg`（填充） |

**文件命名规则**（与 `icon` 字段完全一致，无需额外映射表）：

- `{code}.svg` — 线稿（outline），class 名 `qi-{code}`
- `{code}-fill.svg` — 填充（filled），class 名 `qi-{code}-fill`

SVG 默认 `width="16" height="16"`、`fill="currentColor"`，可任意缩放与改色，适合单色 RLCD 直接渲染。

## 当前项目的图标策略

> `components/ui/src/ui_home.c` 的 `classify_weather()` 目前按 `weather_text` 中文关键字（晴/多云/阴/雨/雪/雷/雾/霾）把天气归为 7 类（`weather_kind_t`），再用 LVGL 绘图原语画图标 — **并未直接使用 SVG**。
>
> 若未来要改用本目录的真实 SVG 图标，只需把 `now.icon` 字符串拼成文件名即可，命名与 API 完全对齐。

## 天气图标代码表

所有代码在本目录 `QWeather-Icons-1.8.0/icons/` 下均有线稿 + 填充两个版本（共 126 个文件）。

### 晴 / 多云 / 阴（100-153）

| 代码 | 中文     | English             | 日夜变体         |
| ---- | -------- | ------------------- | ---------------- |
| 100  | 晴       | Clear               | 150 = 夜晚       |
| 101  | 多云     | Cloudy              | 151 = 夜晚       |
| 102  | 少云     | Few Clouds          | 152 = 夜晚       |
| 103  | 晴间多云 | Partly Cloudy       | 153 = 夜晚       |
| 104  | 阴       | Overcast            | 通用（无日夜版） |
| 150  | 晴       | Clear（夜）         | —               |
| 151  | 多云     | Cloudy（夜）        | —               |
| 152  | 少云     | Few Clouds（夜）    | —               |
| 153  | 晴间多云 | Partly Cloudy（夜） | —               |

### 雨（300-399）

| 代码 | 中文             | English                     | 日夜变体   |
| ---- | ---------------- | --------------------------- | ---------- |
| 300  | 阵雨             | Shower                      | 350 = 夜晚 |
| 301  | 强阵雨           | Heavy Shower                | 351 = 夜晚 |
| 302  | 雷阵雨           | Thundershower               | 通用       |
| 303  | 强雷阵雨         | Heavy Thundershower         | 通用       |
| 304  | 雷阵雨伴有冰雹   | Hail                        | 通用       |
| 305  | 小雨             | Light Rain                  | 通用       |
| 306  | 中雨             | Moderate Rain               | 通用       |
| 307  | 大雨             | Heavy Rain                  | 通用       |
| 308  | 极端降雨         | Extreme Rainfall            | 通用       |
| 309  | 毛毛雨/细雨      | Drizzle                     | 通用       |
| 310  | 暴雨             | Storm                       | 通用       |
| 311  | 大暴雨           | Heavy Storm                 | 通用       |
| 312  | 特大暴雨         | Severe Storm                | 通用       |
| 313  | 冻雨             | Freezing Rain               | 通用       |
| 314  | 小到中雨         | Light to Moderate Rain      | 通用       |
| 315  | 中到大雨         | Moderate to Heavy Rain      | 通用       |
| 316  | 大到暴雨         | Heavy Rain to Storm         | 通用       |
| 317  | 暴雨到大暴雨     | Storm to Heavy Storm        | 通用       |
| 318  | 大暴雨到特大暴雨 | Heavy Storm to Severe Storm | 通用       |
| 350  | 阵雨（夜）       | Shower（夜）                | —         |
| 351  | 强阵雨（夜）     | Heavy Shower（夜）          | —         |
| 399  | 雨               | Rain                        | 通用       |

### 雪（400-499）

| 代码 | 中文           | English                    | 日夜变体   |
| ---- | -------------- | -------------------------- | ---------- |
| 400  | 小雪           | Light Snow                 | 通用       |
| 401  | 中雪           | Moderate Snow              | 通用       |
| 402  | 大雪           | Heavy Snow                 | 通用       |
| 403  | 暴雪           | Blizzard                   | 通用       |
| 404  | 雨夹雪         | Sleet                      | 通用       |
| 405  | 雨雪天气       | Rain and Snow              | 通用       |
| 406  | 阵雨夹雪       | Rain and Snow Shower       | 456 = 夜晚 |
| 407  | 阵雪           | Snow Flurry                | 457 = 夜晚 |
| 408  | 小到中雪       | Light to Moderate Snow     | 通用       |
| 409  | 中到大雪       | Moderate to Heavy Snow     | 通用       |
| 410  | 大到暴雪       | Heavy Snow to Blizzard     | 通用       |
| 456  | 阵雨夹雪（夜） | Rain and Snow Shower（夜） | —         |
| 457  | 阵雪（夜）     | Snow Flurry（夜）          | —         |
| 499  | 雪             | Snow                       | 通用       |

### 雾 / 霾 / 沙尘（500-515）

| 代码 | 中文     | English          | 日夜 |
| ---- | -------- | ---------------- | ---- |
| 500  | 薄雾     | Mist             | 通用 |
| 501  | 雾       | Fog              | 通用 |
| 502  | 霾       | Haze             | 通用 |
| 503  | 扬沙     | Sand             | 通用 |
| 504  | 浮尘     | Dust             | 通用 |
| 507  | 沙尘暴   | Sandstorm        | 通用 |
| 508  | 强沙尘暴 | Heavy Sandstorm  | 通用 |
| 509  | 浓雾     | Dense Fog        | 通用 |
| 510  | 强浓雾   | Strong Dense Fog | 通用 |
| 511  | 中度霾   | Moderate Haze    | 通用 |
| 512  | 重度霾   | Heavy Haze       | 通用 |
| 513  | 严重霾   | Severe Haze      | 通用 |
| 514  | 大雾     | Heavy Fog        | 通用 |
| 515  | 特强浓雾 | Extra Heavy Fog  | 通用 |

### 其他 / 未知

| 代码 | 中文 | English | 日夜 |
| ---- | ---- | ------- | ---- |
| 900  | 热   | Hot     | 通用 |
| 901  | 冷   | Cold    | 通用 |
| 999  | 未知 | Unknown | 通用 |

## 月相图标（800-807，API 不返回，备用且不写入字库）

| 代码 | 北半球 | 南半球 |
| ---- | ------ | ------ |
| 800  | 新月   | 新月   |
| 801  | 蛾眉月 | 残月   |
| 802  | 上弦月 | 下弦月 |
| 803  | 盈凸月 | 亏凸月 |
| 804  | 满月   | 满月   |
| 805  | 亏凸月 | 盈凸月 |
| 806  | 下弦月 | 上弦月 |
| 807  | 残月   | 蛾眉月 |

## 实况 API 完整字段（`now` 对象）

除 `icon` 外，`weather-now` 还返回以下字段，供 UI 详情页使用：

| 字段          | 类型   | 含义                | 单位     |
| ------------- | ------ | ------------------- | -------- |
| `obsTime`   | string | 观测时间            | ISO 8601 |
| `temp`      | string | 温度                | ℃       |
| `feelsLike` | string | 体感温度            | ℃       |
| `icon`      | string | 天气图标代码 → SVG | —       |
| `text`      | string | 天气文字描述        | —       |
| `wind360`   | string | 风向 360°          | °       |
| `windDir`   | string | 风向文字            | —       |
| `windScale` | string | 风力等级            | —       |
| `windSpeed` | string | 风速                | km/h     |
| `humidity`  | string | 相对湿度            | %        |
| `precip`    | string | 降水量              | mm       |
| `pressure`  | string | 气压                | hPa      |
| `vis`       | string | 能见度              | km       |
| `cloud`     | string | 云量                | %        |
| `dew`       | string | 露点温度            | ℃       |

## 目录结构

```
weather_icons/
├── ICON.md                        ← 本文件
└── QWeather-Icons-1.8.0/
    ├── icons/                     ← 507 个 SVG（含线稿/填充/月相/品牌）
    │   ├── {code}.svg
    │   ├── {code}-fill.svg
    │   └── ...
    ├── font/                      ← Web Font（qweather-icons）
    ├── LICENSE                    ← MIT（代码）/ CC BY 4.0（图标）
    └── README.md
```

## 许可

- 图标：[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)（需署名 QWeather）
- 代码：[MIT](https://github.com/qwd/Icons/blob/main/LICENSE)

## 数据来源

- 图标代码：[https://dev.qweather.com/docs/resource/icons/](https://dev.qweather.com/docs/resource/icons/)
- 实况 API：[https://dev.qweather.com/docs/api/weather/weather-now/](https://dev.qweather.com/docs/api/weather/weather-now/)
- SVG 图标包：[https://github.com/qwd/Icons](https://github.com/qwd/Icons)（v1.8.0）
