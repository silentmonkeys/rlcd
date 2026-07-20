# ui — 共享 UI 层

整个 UI（5 个页面 + 状态栏 + 时钟 + 字体入口）。这些文件**不直接碰 ESP-IDF / SDL**，
只读写 `ui_model_t`（`include/ui_model.h`）—— 这是模拟器能链接同一份代码的根本。

## 文件清单

| 文件 | 作用 |
|---|---|
| ui_model.h | 全局数据模型（时间/传感器/天气/状态字段），simulator / device 共享单例 |
| ui_pages.h/c | 页面切换（switch_to/next/prev/rebuild）+ 可见性过滤（SETUP 仅 ap_active 时显示）|
| ui_home.c | HOME：状态栏 + lcd_clock + 三卡片（温度/湿度/天气），本地 draw_wifi/draw_battery |
| ui_weather.c | 2×2 天气详情卡片 + 顶部大卡片（城市/天气/温度/体感）|
| ui_calendar.c | 6×7 月历 + 节假日高亮 + `ui_calendar_mark_date()` 标注入口 |
| ui_device.c | 2×2 设备信息（网络/系统/环境/固件）；IP/SSID 用 LV_LABEL_LONG_DOT 截断 |
| ui_setup.c | 配网提示（大 WiFi 图标 + AP 名 + 地址）|
| ui_common.c/h | 共享绘制原语（pixel_rect/rounded_frame/make_label）+ `ui_draw_wifi_icon` / `ui_draw_battery` / `ui_draw_page_dots` |
| lcd_clock.c/h | 大字号 7 段时钟 widget |
| ui_font.c/h | 字体入口（LV_FONT_DECLARE + 生成字模的访问器）|
| ui_font_*_gen.c | tools/gen_font.sh 生成的 16px CJK / 96px / 28px 数字字模 |

## 状态栏规则

- WiFi：connected=false → 满信号 + "\" 划掉；rssi 分 -55/-65/-75 dBm 三档画弧
- 电池：percent 分 4 档画段（25/50/75）；≤10% 画警示下划线；charging 画闪电
- 页码点：`ui_draw_page_dots(scr, my_index, y, r, spacing)`，总数按 ap_active 取 5/4

## 加新页面

1. `ui_pages.h` 枚举插入新 id + 更新后续 index
2. 新增 `ui_xxx.h/c`（create + apply_locked）
3. `ui_pages.c` 三处：`ui_pages_create` / `screen_of` / `ui_pages_apply_locked`
4. 两处 CMakeLists（`components/ui` 和 `simulator`）加源文件
