仍

# CLAUDE.md

RLCD — ESP-IDF 固件 + 桌面模拟器，驱动 Waveshare ESP32-S3-RLCD-4.2（400×300 单色反射 LCD）。

渲染一套家庭信息面板：状态栏（WiFi/电池）+ 大字号 7 段时钟 + 三卡片数值（室内温湿度/天气）+ 天气详情页 + 日历页 + 设备信息页 + 配网提示页。

## 构建 & 运行

### 设备端

```sh
. ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3          # 首次
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 桌面模拟器（SDL2 + LVGL v9）

```sh
sudo apt install libsdl2-dev cmake build-essential
cd simulator && cmake -B build && cmake --build build -j
./build/rlcd_sim
# 抓帧：./build/rlcd_sim --capture /tmp/frame.ppm --capture-ms 1500
# 控制台：mark MM-DD 标注日历 / unmark 清除 / page N 切页
# 图形化调试：python3 tools/rlcd_debug_gui/rlcd_debug_gui.py
```

键（窗口模式）：`ESC` `S` 存帧 / `+ -` 缩放。

模拟器同时监听 TCP `127.0.0.1:9000`，协议是**字段表驱动**的：字段表在
`simulator/sim_console.c` 的 `FIELDS[]`（48 个），**加字段只改两处** ——
`FIELDS[]` 加一行 `FLD(...)` + `sim_console.h` 加一个 `OVR_*` 位
（`set`/`get`/`dump`/`help` 全部自动跟上）。每条命令的响应后会再发一个
**空行作为帧终止**，客户端据此判断读完。`sim_tick_data()` 500ms 一拍，
按 override 位决定是否重填，所以 `clear` 后要等 >500ms 才读到恢复值。
GUI 客户端见 [tools/rlcd_debug_gui/README.md](tools/rlcd_debug_gui/README.md)。

## 架构

```
              +-- main/main.cpp  (device entry) ---+
              |                                    |
   sensors    v   flush RGB565 -> mono            simulator/main.c
   RTC     +------+   < 0x7fff -> Black            +-------+
   ADC --->|      |                                | SDL2  |
   net_bsp | UI   |<---- ui_model_t (singleton) -->|       |
   weather |      | ui_home.c / lcd_clock.c        +-------+
           +------+ + ui_font.c
            ↕ flush via port_bsp
```

**UI 代码（`components/ui/`）永远不直接碰 ESP-IDF / SDL 头文件** — 只读写 `ui_model_t`（`ui_model.h`）。模拟器链接同一份 ui_home.c 来产像素相同输出。

## 页面顺序

| 索引 | 页面     | 文件          | 可见性            |
| ---- | -------- | ------------- | ----------------- |
| 0    | HOME     | ui_home.c     | 始终              |
| 1    | WEATHER  | ui_weather.c  | 始终              |
| 2    | CALENDAR | ui_calendar.c | 始终              |
| 3    | DEVICE   | ui_device.c   | 始终              |
| 4    | SETUP    | ui_setup.c    | 仅 ap_active=true |

切换：BOOT 短按下一页 / KEY 短按上一页 / BOOT 长按重建 UI。

## 组件

- **port_bsp** — DisplayPort C++ 类（SPI3 驱动 RLCD）、I2C 主机 + SHTC3 驱动、按键 BSP（multi_button + 5ms tick）、电池 ADC（`adc_bsp` — ADC1_CH3/GPIO4 + 曲线校准 ×3 分压）、SD 卡 BSP（SDMMC 1-line + 5s 热插拔探活）。引脚见 `main/user_config.h`。
- **app_bsp** — LVGL v9 端口：tick 定时器、任务 handler、互斥（Lvgl_lock/unlock）。
- **ui** — 共享 UI。`ui_home_create()` 建树；其他任务写 `ui_model_get()` 后调 `ui_home_request_refresh()` 或 `ui_home_apply_locked()`。
- **net_bsp** — WiFi STA/SoftAP、HTTP 配网门户、NVS 持久化、天气拉取（**QWeather** 单一 provider）。按职责拆为多文件：`net_bsp.c`（入口 + 共享状态 + NVS + 看门狗）/ `net_wifi.c`（WiFi 事件 + SNTP）/ `net_portal.c`（管理门户：静态资源 + JSON API）/ `net_http.c`（**共享** HTTPS GET + gzip 解压 + 轻量 JSON 取值，QWeather 与 UAPI 共用）/ `net_weather.c`（QWeather API）/ `net_uapi.c`（**UAPI uapis.cn** `/network/myip`：公网 IP + 自动城市）/ `net_calendar.c`（日历存 SD，原子写）/ `net_ota.c`（`POST /api/ota` 流式接收固件写备用 app 槽）/ `net_internal.h`（组件内共享声明）。对外 API 仍只在 `net_bsp.h`。门户前端**权威源码在 `components/net_bsp/portal/`**（index.html / style.css / app.js），改完必须跑 `python3 tools/gen_portal.py` 重新生成 `src/portal_assets.h`（gzip 字节数组，勿手改）+ `simulator/portal_preview.html`（带 mock，浏览器直接打开可预览）。
- **user_app** — 传感器 / 电池 ADC 初始化 + 1Hz tick 任务把读数写入 ui_model（温湿度每秒；电量、充电趋势、SD 探活、无网看门狗共用 5s 慢节拍）；独立 CSV 日志任务（fsync 落盘）。RTC 待硬件到货再接。

## 后台任务与节拍

常驻任务 4 个 + esp_timer 周期回调 2 个。传感器/设备采样统一收敛在 user_tick 一条主线，避免多个独立倒计时。

| 任务 / 回调              | 节拍                  | 职责                                                                                     |
| ------------------------ | --------------------- | ---------------------------------------------------------------------------------------- |
| user_tick                | 1s；**5s 慢节拍**     | 每秒读时间 + SHTC3；5s 慢节拍做 SD 热插拔探活 + 电池采样 +`NetBsp_OfflineWatchdogTick()` + `NetBsp_OtaSelfTestTick()` |
| csv_log                  | 10min                 | 追加一行 CSV 到 SD（首帧延迟 15s，fsync 落盘）                                           |
| weather                  | 成功 10min / 失败 30s | 拉 QWeather；**顺带每日一次 UAPI 定位**（自动城市 + 公网 IP）；事件位可提前唤醒 |
| LVGL                     | 自适应 1~500ms        | `lv_timer_handler()` 渲染                                                                |
| button tick（esp_timer） | 5ms                   | multi_button 按键去抖                                                                    |
| lvgl tick（esp_timer）   | 5ms                   | 给 LVGL 喂 tick                                                                          |

> 无网看门狗不自带任务：做成一次性 `NetBsp_OfflineWatchdogTick()`，由 user_tick 的 5s 慢节拍调用（状态未就绪时函数自身 early-return，早启无害）。
>
> **OTA 自检确认**同样是一次性的 `NetBsp_OtaSelfTestTick()`，挂在同一条 5s 慢节拍上：新固件首次启动处于 `PENDING_VERIFY`，稳定跑满 60s 才调 `esp_ota_mark_app_valid_cancel_rollback()`；若这之前崩溃重启，bootloader 自动回滚到旧槽。非 OTA 启动时函数第一次调用就 early-return 并自锁，零开销。依赖 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。

## 自动城市 & 公网 IP（UAPI uapis.cn）

管理页城市**留空 = 自动定位**：调 `https://uapis.cn/api/v1/network/myip?source=commercial`，
用返回的 `district`（行政区，如"青秀区"）当查询词喂 QWeather 城市解析；`district` 缺失时退到
`region`（"国家 省份 城市"）末段。

- **优先级**：用户手填城市**永远优先**。`s_cfg.city` 非空时完全不调 UAPI，也不消耗配额。
  自动结果只存运行期缓存（`s_uapi_info` / weather_task 局部 `auto_city`），**绝不写回
  `s_cfg.city`** —— 否则自动值会伪装成"用户手填过"。
- **频率**：一天一次，与 daily 天气共用「跨天」判据，挂在 weather_task 里（无独立任务）。
  失败不记当天、下轮重试，但每天最多试 5 次（`UAPI_MAX_TRIES_PER_DAY`），避免限流时空转。
  定位**不受**天气凭据缺失影响 —— 没配 QWeather 也能看到自己的公网 IP。
- **鉴权**：`Authorization: Bearer <uapi-…>`，key 存 NVS（`uapi_key`，门户天气页可填），
  **不硬编码、不进 URL**。留空则按访客配额调用（文档：1500 credits/月/IP、4 QPS）。
- **门户**：天气页「刷新城市」按钮（城市非空时后端回 409）；网络页「公网 IP」卡片
  用同一套 `.grid/.kv` 布局展示 ip/region/district/isp。接口 `POST /api/city_refresh`、
  `GET /api/pubip`。

## 状态栏显示规则

- **WiFi**：connected=false → 满信号 + "\" 划掉；rssi ≥ -55 → 3 弧；-65 → 2 弧；-75 → 1 弧；<-75 → 仅圆点。
- **电池**：percent 分 4 档（25/50/75）段数；≤10% 加警示下划线；charging=true 画闪电。数据来自 `adc_bsp` 实测电压（3.0V→0% / 4.12V→100%），charging 由 user_app 的电压趋势启发式推断（无充电检测引脚）。

## 字体

字库**存在独立 `fonts` SPIFFS 分区（1MB，当前实占约 257KB）**，运行时用 `lv_binfont_create()` 从 `/spiffs` 加载，不再编译进固件。

> **OTA 只写 app 槽，不动 `fonts` 分区。** 所以改过字库/天气图标的版本，发布时必须写明"需用 `full.bin` 整片重烧"，否则新 app 配旧字库会出现空白/豆腐块。见 `.github/release_notes.md`。

- `tools/gen_font.sh` 用 lv_font_conv 生成 **3 个 `.bin`** 到 `partitions/fonts/`：`ui_font_cjk_16.bin`（GB2312 一二级共 6763 字 + 标点 + ASCII）、`ui_font_digit_big.bin`（96px 时钟）、`ui_font_digit_mid.bin`（28px 卡片数值）。需要 `/home/chen/.npm-global/bin` 在 PATH。
- 加字：改 `gen_font.sh` 的字符集 → `bash tools/gen_font.sh` → `idf.py build && idf.py flash`（或只烧字库不动 app：先 `idf.py partition-table` 再 flash）。**不用改 C 代码**。
- CMake：`main/CMakeLists.txt` 里 `spiffs_create_partition_image(fonts partitions/fonts FLASH_IN_PROJECT)` 把目录打包成镜像随 flash 一起烧。
- `main.cpp` 在 `Lvgl_PortInit` 之后、`ui_pages_create` 之前调 `UiFont_MountFs()` + `UiFont_Load()`。
- 需要 `CONFIG_LV_USE_FS_POSIX=y` + `CONFIG_LV_FS_POSIX_LETTER=65`（盘符 'A'）。
- 模拟器：`UiFont_LoadFromDir(RLCD_FONTS_DIR)` 直接从 `partitions/fonts/` 读同一批 `.bin`。

## 天气图标

和风天气图标存在**同一 `fonts` 分区**的 `weather/<code>.bin`，与字库一起打包进 `fonts.bin`。

- 数据源：`NEEDS/weather_icons/QWeather-Icons-1.8.0/icons/{code}.svg`（线稿，**不用** `-fill` 填充版）。代码 100-153 / 300-399 / 400-499 / 500-515 / 900-999，与 `now.icon` 字段一一对应，无需映射表。
- 生成：`tools/gen_weather_icons.py`（cairosvg + PIL）4× 超采样栅格化 → LANCZOS 降到 40×40 → 128 阈值二值化。只打包实况 API 会返回的 62 个代码。输出**单文件** `partitions/fonts/ui_font_weather_40.bin`（~13KB），与字库文件同级。
- 文件格式：`header('WETH'+ver+count) | index[count×8B: code+offset+size] | data[各图标 w,h,bits 拼接]`。
- 加载：`ui_weather_icon_load(img, code)`（`components/ui/src/ui_weather_icon.c`）open 一次缓存句柄，二分查索引 → seek 读数据，拼 `lv_image_dsc_t`（`LV_COLOR_FORMAT_I1`，调色板 0=白 1=黑），找不到代码时回落 999（未知）。路径 `A:<base>/ui_font_weather_40.bin`。
- 主页 `ui_home.c` 用 `lv_img` + `weather_code` 显示位图（旧像素绘制已删除，仅保留 999 位图兜底）。`net_weather.c` 解析 `now.icon` → `weather_code`。
- 改图标/加字：重跑 `python3 tools/gen_weather_icons.py` → `idf.py build`（分区镜像自动包含 `weather/` 子目录）。

## 约定

- **语言**：main.cpp / port_bsp / app_bsp 是 C++；ui / net_bsp / user_app 是 C。头文件用 `extern "C"` 守卫。
- **注释与提交文本用中文**。
- **sdkconfig** 已提交；改 menuconfig 后同步 sdkconfig.defaults。
- **NEEDS/** 是参考 PNG，布局调整时对照。
- **build/** 已提交相邻；managed_components 由 component manager 拉取。
- **编译运行** 使用espidf的mcp服务来编译，如果遇到nijia确实或冲突的情况，应当直接删除原先的build再次编译，如果使用工具仍冲突，则应当删除build文件后再编译
