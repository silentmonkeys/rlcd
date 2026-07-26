# ui — 共享 UI 层

整个 UI（5 个页面 + 状态栏 + 时钟 + 字体入口 + 天气图标）。这些文件**不直接碰
ESP-IDF / SDL**，只读写 `ui_model_t`（`include/ui_model.h`）—— 这是模拟器能链接
同一份代码的根本。

## 文件清单

| 文件 | 作用 |
|---|---|
| `include/ui_model.h` | 全局数据模型（时间/传感器/天气/状态字段），simulator / device 共享单例 |
| `ui_pages.h/c` | 页面切换（switch_to/next/prev/rebuild）+ 可见性过滤（SETUP 仅 ap_active 时显示）|
| `ui_home.c` | HOME：状态栏 + lcd_clock + 三卡片（温度/湿度/天气）|
| `ui_weather.c` | 2×2 天气详情卡片 + 顶部大卡片（城市/天气/温度/体感）|
| `ui_calendar.c` | 6×7 月历 + 节假日高亮 + `ui_calendar_mark_date()` 标注入口 |
| `ui_device.c` | 2×2 设备信息（网络/系统/环境/固件）；IP/SSID 用 LV_LABEL_LONG_DOT 截断 |
| `ui_setup.c` | 配网提示（大 WiFi 图标 + AP 名 + 地址）|
| `ui_common.c/h` | 共享绘制原语（pixel_rect/rounded_frame/make_label）+ `ui_draw_wifi_icon` / `ui_draw_battery` / `ui_draw_page_dots` |
| `lcd_clock.c/h` | 大字号 7 段时钟 widget |
| `ui_font.c/h` | 字体入口：从 `A:/spiffs/*.bin` 用 `lv_binfont_create()` 加载 4 份字库（真机 SPIFFS / 模拟器本地目录）|
| `ui_weather_icon.c/h` | 天气图标加载：从 `ui_font_weather_40.bin` 按 `weather_code` 查索引 → 拼 `lv_image_dsc_t`（1-bit）|

## 字体

字库存在独立 `fonts` SPIFFS 分区（2 MB），运行时用 `lv_binfont_create()` 从
`/spiffs` 加载，不再编译进固件。生成工具见 `tools/README.md`。

- 4 份字库：`ui_font_cjk_16` / `ui_font_digit_big` / `ui_font_digit_mid` / `ui_font_mood_16`
- 加字：改 `tools/gen_font.sh` → 重跑 → `idf.py flash-fonts`（只烧字库不动 app）。**不用改 C 代码**
- 需要 `CONFIG_LV_USE_FS_POSIX=y` + `CONFIG_LV_FS_POSIX_LETTER=65`（盘符 'A'）

## 温度显示

温度字段（`indoor_temp` / `outdoor_temp` / `feels_like_temp`）用 `float` 存储，
`temp_min` / `temp_max` 用 `int`。显示时用 `roundf()` 取整（四舍五入，对正负数均正确）：

```c
snprintf(buf, sizeof(buf), "%d℃", (int)roundf(m->indoor_temp));
```

`temp_min` / `temp_max` 直接 `%d` 输出。无数据时（NaN / ≤0）显示 `"--"`。

## 心情表情（主页）

根据**室内温度 + 湿度**查表，在主页（时钟下方、星期与日期之间）显示颜文字 +
中文描述。渲染在 `ui_home.c` 的 `render_mood()` / `MOOD_MAP`。

### 档位划分

| 维度 | 档 0 | 档 1 | 档 2 | 档 3 | 档 4 |
|---|---|---|---|---|---|
| 温度 ℃ | <10 | 10..17 | 18..25 | 26..31 | ≥32 |
| 湿度 % | <40 | 40..65 | >65 | — | — |

### 查找表 `MOOD_MAP[温度档][湿度档]`

湿度低→"干/凉"，适中→"舒适"，高→"潮/闷"：

| 温度＼湿度 | <40% (干) | 40..65% (适) | >65% (潮) |
|---|---|---|---|
| **<10℃** | `(+_+)` 干冷 | `(o_o)` 寒冷 | `(~_~)` 湿冷 |
| **10..17℃** | `(-_-)` 凉·干 | `(^_^)` 微凉 | `(o~o)` 凉·潮 |
| **18..25℃** | `(^○^)` 舒适·干 | `(^▽^)` 舒适 | `(~▽~)` 舒适·潮 |
| **26..31℃** | `(=_=)` 干热 | `(*_*)` 微热 | `(○~○)` 闷热 |
| **≥32℃** | `(T_T)` 暴晒 | `(O_O)` 炎热 | `(#_#)` 蒸笼 |

**边界**：温度/湿度任一为 NaN → 显示空白（不画表情）。

### 改表情/调阈值

直接改 `ui_home.c` 的 `temp_level()` / `humi_level()` 阈值或 `MOOD_MAP` 表即可。
颜文字由 `ui_font_mood_16.bin` 提供 glyph（CJK 字库里没有 ▽ 等符号，必须由 mood 字体
提供，否则 fallback 后显示空白）。

## 状态栏规则

- WiFi：connected=false → 满信号 + "\" 划掉；rssi 分 -55/-65/-75 dBm 三档画弧
- 电池：percent 分 4 档画段（25/50/75）；≤10% 画警示下划线；charging 画闪电
- 页码点：`ui_draw_page_dots(scr, my_index, y, r, spacing)`，总数按 ap_active 取 5/4

## 加新页面

1. `ui_pages.h` 枚举插入新 id + 更新后续 index
2. 新增 `ui_xxx.h/c`（create + apply_locked）
3. `ui_pages.c` 三处：`ui_pages_create` / `screen_of` / `ui_pages_apply_locked`
4. 两处 CMakeLists（`components/ui` 和 `simulator`）加源文件
