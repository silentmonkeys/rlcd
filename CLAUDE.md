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
```

键（窗口模式）：`ESC` `S` 存帧 / `+ -` 缩放。

## 架构

```
              +-- main/main.cpp  (device entry) ---+
              |                                    |
   sensors    v   flush RGB565 -> mono            simulator/main.c
   RTC     +------+   < 0x7fff -> Black            +-------+
   ADC --->|      |                                | SDL2  |
   net_bsp | UI   |<---- ui_model_t (singleton) -->|       |
   weather |      | ui_home.c / lcd_clock.c        +-------+
           +------+ + ui_font_*_gen.c
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
- **net_bsp** — WiFi STA/SoftAP、HTTP 配网门户、NVS 持久化、天气拉取（**QWeather** 单一 provider）。按职责拆为多文件：`net_bsp.c`（入口 + 共享状态 + NVS + 看门狗）/ `net_wifi.c`（WiFi 事件 + SNTP）/ `net_portal.c`（配网门户，HTML 模板见 `portal_page.h`）/ `net_weather.c`（QWeather API + gzip）/ `net_calendar.c`（日历存 SD，原子写）/ `net_internal.h`（组件内共享声明）。对外 API 仍只在 `net_bsp.h`。
- **user_app** — 传感器 / 电池 ADC 初始化 + 1Hz tick 任务把读数写入 ui_model（温湿度每秒；电量、充电趋势、SD 探活、无网看门狗共用 5s 慢节拍）；独立 CSV 日志任务（fsync 落盘）。RTC 待硬件到货再接。

## 后台任务与节拍

常驻任务 4 个 + esp_timer 周期回调 2 个。传感器/设备采样统一收敛在 user_tick 一条主线，避免多个独立倒计时。

| 任务 / 回调              | 节拍                    | 职责                                                                                       |
| ------------------------ | ----------------------- | ------------------------------------------------------------------------------------------ |
| user_tick                | 1s；**5s 慢节拍** | 每秒读时间 + SHTC3；5s 慢节拍做 SD 热插拔探活 + 电池采样 +`NetBsp_OfflineWatchdogTick()` |
| csv_log                  | 10min                   | 追加一行 CSV 到 SD（首帧延迟 15s，fsync 落盘）                                             |
| weather                  | 成功 10min / 失败 30s   | 拉 QWeather；事件位可提前唤醒                                                              |
| LVGL                     | 自适应 1~500ms          | `lv_timer_handler()` 渲染                                                                |
| button tick（esp_timer） | 5ms                     | multi_button 按键去抖                                                                      |
| lvgl tick（esp_timer）   | 5ms                     | 给 LVGL 喂 tick                                                                            |

> 无网看门狗不自带任务：做成一次性 `NetBsp_OfflineWatchdogTick()`，由 user_tick 的 5s 慢节拍调用（状态未就绪时函数自身 early-return，早启无害）。

## 状态栏显示规则

- **WiFi**：connected=false → 满信号 + "\" 划掉；rssi ≥ -55 → 3 弧；-65 → 2 弧；-75 → 1 弧；<-75 → 仅圆点。
- **电池**：percent 分 4 档（25/50/75）段数；≤10% 加警示下划线；charging=true 画闪电。数据来自 `adc_bsp` 实测电压（3.0V→0% / 4.12V→100%），charging 由 user_app 的电压趋势启发式推断（无充电检测引脚）。

## 字体

字库**存在独立 `fonts` SPIFFS 分区（2MB）**，运行时用 `lv_binfont_create()` 从 `/spiffs` 加载，不再编译进固件。

- `tools/gen_font.sh` 用 lv_font_conv 生成 **3 个 `.bin`** 到 `partitions/fonts/`：`ui_font_cjk_16.bin`（GB2312 一级 3755 字 + 标点 + ASCII）、`ui_font_digit_big.bin`（96px 时钟）、`ui_font_digit_mid.bin`（28px 卡片数值）。需要 `/home/chen/.npm-global/bin` 在 PATH。
- 加字：改 `gen_font.sh` 的字符集 → `bash tools/gen_font.sh` → `idf.py build && idf.py flash`（或只烧字库不动 app：先 `idf.py partition-table` 再 flash）。**不用改 C 代码**。
- CMake：`main/CMakeLists.txt` 里 `spiffs_create_partition_image(fonts partitions/fonts FLASH_IN_PROJECT)` 把目录打包成镜像随 flash 一起烧。
- `main.cpp` 在 `Lvgl_PortInit` 之后、`ui_pages_create` 之前调 `UiFont_MountFs()` + `UiFont_Load()`。
- 需要 `CONFIG_LV_USE_FS_POSIX=y` + `CONFIG_LV_FS_POSIX_LETTER=65`（盘符 'A'）。
- 模拟器：`UiFont_LoadFromDir(RLCD_FONTS_DIR)` 直接从 `partitions/fonts/` 读同一批 `.bin`。

## 约定

- **语言**：main.cpp / port_bsp / app_bsp 是 C++；ui / net_bsp / user_app 是 C。头文件用 `extern "C"` 守卫。
- **注释与提交文本用中文**。
- **sdkconfig** 已提交；改 menuconfig 后同步 sdkconfig.defaults。
- **NEEDS/** 是参考 PNG，布局调整时对照。
- **build/** 已提交相邻；managed_components 由 component manager 拉取。
- **编译运行** 使用espidf的mcp服务来编译，如果遇到nijia确实或冲突的情况，应当直接删除原先的build再次编译
