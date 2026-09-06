仍

# CLAUDE.md

RLCD — ESP-IDF 固件 + 桌面模拟器，驱动 Waveshare ESP32-S3-RLCD-4.2（400×300 单色反射 LCD）。

渲染一套家庭信息面板：状态栏（WiFi/电池）+ 大字号 7 段时钟 + 三卡片数值（室内温湿度/天气）+ 天气详情页 + 日历页 + 设备信息页 + 机器人动画页 + 配网提示页。

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
sudo apt install libsdl2-dev cmake build-essential python3-tk

# 一条命令：编译 + 起模拟器 + 起调试台（自动连接，两窗口并排不重叠），关窗口时收掉模拟器
bash tools/dev_sim.sh            # --page N 指定起始页 / --no-build 跳过编译
                                 # --sim-only 只要模拟器 / --gui-only 连已在跑的实例
                                 # --stack 上下叠放 / --no-layout 不管窗口位置

# 或手动分两个终端
cd simulator && cmake -B build && cmake --build build -j
./build/rlcd_sim
# 抓帧：./build/rlcd_sim --capture /tmp/frame.ppm --capture-ms 1500
# 窗口位置：./build/rlcd_sim --pos 40,40（默认居中）
# 控制台：mark MM-DD 标注日历 / unmark 清除 / page N 切页
# 图形化调试：python3 tools/rlcd_debug_gui/rlcd_debug_gui.py [host:port] [--connect] [--geometry WxH+X+Y]
```

键（窗口模式）：`ESC` `S` 存帧 / `+ -` 缩放。

模拟器同时监听 TCP `127.0.0.1:9000`，协议是**字段表驱动**的：字段表在
`simulator/sim_console.c` 的 `FIELDS[]`（47 个），**加字段只改两处** ——
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
| 1    | BOT      | ui_bot.c      | 始终              |
| 2    | WEATHER  | ui_weather.c  | 始终              |
| 3    | CALENDAR | ui_calendar.c | 始终              |
| 4    | DEVICE   | ui_device.c   | 始终              |
| 5    | SETUP    | ui_setup.c    | 仅 ap_active=true |

切换：BOOT 短按下一页 / KEY 短按上一页 / BOOT 长按重建 UI / KEY 按住 ≥350ms 说话（见「语音对话」）。

## 组件

- **port_bsp** — DisplayPort C++ 类（SPI3 驱动 RLCD）、I2C 主机 + SHTC3 驱动、按键 BSP（multi_button + 5ms tick，KEY 按住 350ms 判定为 PTT 起手）、电池 ADC（`adc_bsp` — ADC1_CH3/GPIO4 + 曲线校准 ×3 分压）、SD 卡 BSP（SDMMC 1-line + 5s 热插拔探活）、**音频底座 `audio_bsp`**（ES8311 DAC 出声 + ES7210 双麦收音，I2S std 全双工 + esp_codec_dev；I2C 复用 SHTC3 总线、地址逐个 probe；PA=GPIO46 仅会话期间开）。引脚见 `main/user_config.h`。
- **app_bsp** — LVGL v9 端口：tick 定时器、任务 handler、互斥（Lvgl_lock/unlock）。
- **ui** — 共享 UI。`ui_home_create()` 建树；其他任务写 `ui_model_get()` 后调 `ui_home_request_refresh()` 或 `ui_home_apply_locked()`。
- **ui_bot** — 机器人页：bloub C 引擎实时渲染 + xiaozhi 对话文本。bloub（`/home/chen/demo/bloub`，MIT）的 `src/bot/` 是纯函数引擎，已移植为 C（`src/bloub/`：math/face/states/engine/render 五个文件，`bloub_bot.h` 对外）——状态机子集 idle/wink/wide/thinking/sleep + Catmull-Rom 轮廓 → 扫描线 1-bit 光栅化。`ui_bot.c` lv_timer 80ms 一拍：引擎时钟推进 → `ui_model` 的 bot_state/bot_emotion 映射成 bloub 姿态（LISTENING→wide、THINKING→thinking、SPEAKING→跟情绪等）→ sample → render → 刷 I1 位图。引擎与 Node 真值逐帧像素比对：`python3 tools/bloub_golden.py`（100 帧全蒙太奇 <0.5%，阈值 2.5%），离线渲染入口 `rlcd_sim --bot-frame <t> --bot-out <pbm>`。**旧的 bloub_seq.bin 预渲染方案已删除**；改蒙太奇/姿态先看 bloub 仓库 CLAUDE.md（常数勿取整）。
- **net_bsp** — WiFi STA/SoftAP、HTTP 配网门户、NVS 持久化、天气拉取（**QWeather** 单一 provider）。按职责拆为多文件：`net_bsp.c`（入口 + 共享状态 + NVS + 看门狗）/ `net_wifi.c`（WiFi 事件 + SNTP）/ `net_portal.c`（管理门户：静态资源 + JSON API）/ `net_http.c`（**共享** HTTPS GET + gzip 解压 + 轻量 JSON 取值，QWeather 与 UAPI 共用）/ `net_weather.c`（QWeather API）/ `net_uapi.c`（**UAPI uapis.cn** `/network/myip`：公网 IP + 自动城市）/ `net_apistat.c`（**外部接口调用统计**，明细存 SD 按月分文件）/ `net_calendar.c`（日历存 SD，原子写）/ `net_ota.c`（`POST /api/ota` 流式接收固件写备用 app 槽）/ `net_xiaozhi.c`（**xiaozhi AI 对话**：激活 + WS 文本/语音会话，见「语音对话」）/ `net_internal.h`（组件内共享声明）。对外 API 仍只在 `net_bsp.h`。门户前端**权威源码在 `components/net_bsp/portal/`**（index.html / style.css / app.js），改完必须跑 `python3 tools/gen_portal.py` 重新生成 `src/portal_assets.h`（gzip 字节数组，勿手改）+ `simulator/portal_preview.html`（带 mock，浏览器直接打开可预览）。
- **user_app** — 传感器 / 电池 ADC 初始化 + 1Hz tick 任务把读数写入 ui_model（温湿度每秒；电量、SD 探活、无网看门狗共用 5s 慢节拍）；独立 CSV 日志任务（fsync 落盘）。RTC 待硬件到货再接。

## 后台任务与节拍

常驻任务 4 个 + esp_timer 周期回调 2 个。传感器/设备采样统一收敛在 user_tick 一条主线，避免多个独立倒计时。

| 任务 / 回调              | 节拍                  | 职责                                                                                     |
| ------------------------ | --------------------- | ---------------------------------------------------------------------------------------- |
| user_tick                | 1s；**5s 慢节拍**     | 每秒读时间 + SHTC3；5s 慢节拍做 SD 热插拔探活 + 电池采样 +`NetBsp_OfflineWatchdogTick()` + `NetBsp_OtaSelfTestTick()` |
| csv_log                  | 10min                 | 追加一行 CSV 到 SD（首帧延迟 15s，fsync 落盘）                                           |
| weather                  | 成功 10min / 失败 30s | 拉 QWeather；**城市留空时顺带每日一次 UAPI 定位**（自动城市 + 公网 IP）；事件位可提前唤醒 |
| xiaozhi                  | 常驻，按需会话        | 激活流程（无 NVS 配置时轮询）+ 文本/语音对话（每轮独立 WS，见「语音对话」）               |
| LVGL                     | 自适应 1~500ms        | `lv_timer_handler()` 渲染                                                                |
| button tick（esp_timer） | 5ms                   | multi_button 按键去抖                                                                    |
| lvgl tick（esp_timer）   | 5ms                   | 给 LVGL 喂 tick                                                                          |

> 无网看门狗不自带任务：做成一次性 `NetBsp_OfflineWatchdogTick()`，由 user_tick 的 5s 慢节拍调用（状态未就绪时函数自身 early-return，早启无害）。
>
> **OTA 自检确认**同样是一次性的 `NetBsp_OtaSelfTestTick()`，挂在同一条 5s 慢节拍上：新固件首次启动处于 `PENDING_VERIFY`，稳定跑满 60s 才调 `esp_ota_mark_app_valid_cancel_rollback()`；若这之前崩溃重启，bootloader 自动回滚到旧槽。非 OTA 启动时函数第一次调用就 early-return 并自锁，零开销。依赖 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。

## 语音对话（xiaozhi + ES8311/ES7210）

xiaozhi AI 对话：文本（门户聊天框）+ 语音（板载 ES8311 DAC 出声 / ES7210 双麦收音）。

- **激活**：设备联网后 POST `https://api.tenclass.net/xiaozhi/ota/` → 未激活时响应带
  `activation.code`（6 位码），管理门户「AI 助手」卡片展示；用户到 **xiaozhi.me**
  控制台添加设备输码绑定；设备轮询 `<ota>/activate`（202=等待），成功后 check 响应
  携带 `websocket{url,token}` 存 NVS（命名空间 `xiaozhi`）。
- **按键说话（PTT）**：KEY 按住 ≥350ms 开始收音、松开结束（`button_bsp.c` 用
  esp_timer 单次 350ms 起判，快速点击仍是切页）。流：listen start(manual) →
  麦克风 16k/16bit/mono 60ms/帧 → opus 编码 → WS binary；listen stop 后服务端
  STT→LLM→TTS，下行 opus → 解码 → 线性重采样到 16k → 扬声器；tts stop 且播完即断开。
- **采样率注意**：服务端 TTS 常见 **24k**（以 hello 的 audio_params 为准），而 I2S
  全双工收发共享时钟、麦克风固定 16k —— 播放侧必须重采样（`net_xiaozhi.c` 的
  `resamp_16k`，相位跨帧保持）。
- **音频底座**：`port_bsp/audio_bsp.c`（esp_codec_dev：ES8311 只 DAC + ES7210 只
  ADC，共享 I2S 时钟、数据线分开）。通过 `NetBsp_XiaozhiSetAudioOps()` **反向注册**
  给 net_bsp（不能正向依赖：button_bsp→net_bsp 已成环，net_bsp→port_bsp 会闭环）。
  未注册/探测失败（无芯片）自动回落纯文本，模拟器即纯文本路径。
- **I2C 地址 strap 未知**：初始化逐个 probe（ES8311: 0x18/0x19，ES7210:
  0x40-43/0x20-23，esp_codec_dev 的 `cfg.addr` 要 8bit 形式）。
- **调试台**：`bot_state`（0-7）/`bot_emotion`（0-9）/`bot_chat_reply` 三个字段 +
  8 个 BOT 场景预设（聆听/思考/播报等），机器人姿态实时跟 `ui_model` 走。

## 自动城市 & 公网 IP（UAPI uapis.cn）

管理页城市**留空 = 自动定位**：调 `https://uapis.cn/api/v1/network/myip?source=commercial`，
用返回的 `district`（行政区，如"青秀区"）当查询词喂 QWeather 城市解析；`district` 缺失时退到
`region`（"国家 省份 城市"）末段。

- **优先级**：用户手填城市**永远优先**。`s_cfg.city` 非空时后台**一次都不调** UAPI（跨天
  也不调），不消耗配额 —— 自动结果此时既不参与查询词也没人消费。
  自动结果只存运行期缓存（`s_uapi_info` / weather_task 局部 `auto_city`），**绝不写回
  `s_cfg.city`** —— 否则自动值会伪装成"用户手填过"。
- **频率**：两条路，都挂在 weather_task 里（无独立任务）。
  ① **每日自动**：**仅城市留空时**，与 daily 天气共用「跨天」判据。失败不记当天、下轮
  重试，但每天最多试 5 次（`UAPI_MAX_TRIES_PER_DAY`），避免限流时空转。
  ② **门户按钮强制**（`s_uapi_city_kick`）：无视每日节拍，**也无视城市是否手填** ——
  手填城市的用户想看公网 IP 只有这一条路，点了才消耗配额。
  定位**不受**天气凭据缺失影响 —— 没配 QWeather 也能看到自己的公网 IP。
  > 手填城市期间 `last_ip_day` 一直复位成 -1，用户之后清空城市（保存时门户会 kick
  > 天气任务）下一轮立刻补一次定位，不用等到明天。
- **鉴权**：`Authorization: Bearer <uapi-…>`，key 存 NVS（`uapi_key`，门户天气页可填），
  **不硬编码、不进 URL**。留空则按访客配额调用（文档：1500 credits/月/IP、4 QPS）。
- **门户**：天气页「重新定位」按钮（`POST /api/city_refresh`，城市非空时后端回 409）；
  网络页「公网 IP」卡片用同一套 `.grid/.kv` 布局展示 ip/region/district/isp，其「刷新」
  按钮走 `POST /api/pubip_refresh`（**任何时候都受理**，会真的发一次请求）+ `GET /api/pubip`
  读缓存。

## 外部接口调用统计（net_apistat.c）

门户「数据」页显示「哪个外部接口调了几次」，**只有次数，不画折线图**。

- **存储**：`/sdcard/rlcd/api/YYYY-MM.csv`，一行一次调用 `timestamp,api,ok`（存**完整调用时间**）。
  **按月分文件**：查询窗口最长「本年」，今日/本月只需读 1 个文件、本周跨月最多 2 个、
  本年最多 12 个 —— 每次查询的读取量都有上界；单文件方案里"查今天"也得扫全年。
  删旧数据 = 删整个文件。约 40B/行，天气 10min×2 端点 ≈ 300 行/天。
- **不维护持久化计数器**：聚合在查询时用块扫（1KB 块 + 手工切行，不用 fgets）现算。
  计数器一旦和明细不一致就没法对账，掉电还会丢增量。
- **窗口是自然周期**（今日 / 本周一起 / 本月 1 号起 / 本年 1 月 1 日起），和服务商配额的
  重置口径一致；滚动窗口（近 30 天）对不上账单。行内比较用打包的 `YYYYMMDD` 整数而非
  `mktime` —— 查本年约 10 万行，而 httpd 是单任务串行的。
- **记录时机**：`wx_fetch()` / `NetBsp_FetchPublicIp()` 里**发出去就记一条**，成败分列
  （`count` / `fail`）。只记成功的话，限流或凭据错时统计会显示"几乎没调用"，正好和实际相反。
- **加一个接口**：`net_bsp.h` 的 `api_call_id_t` 加枚举 + `net_apistat.c` 的 `API_TABLE` 加一行，
  调用处 `NetBsp_ApiCallRecord(id, ok)`。记录/聚合/门户展示全自动跟上（注意 `apistat_get`
  的 `buf[512]` 按接口数放大）。
- 接口：`GET /api/apistat?p=day|week|month|year`。无 SD / 未 SNTP 校时 → `valid:false`。

## 超长文本走马灯（ui_label_marquee）

系统信息页的值 label 一律用 `ui_label_marquee()`（`ui_common.c`）而不是
`LV_LABEL_LONG_DOT`：文本超出格子宽度就横向循环滚动，不超宽的行 LVGL 自己
不起动画，视觉与静态一致。已知会超宽的是 IP / 名称 / 版本。

- **必须配 `ui_label_set_text_if_changed()`**。`lv_label_set_text()` 会重启
  滚动动画（新动画 `act_time` 从 0 起算），而各页 `*_apply_locked()` 是
  1s/500ms 周期调用的 —— 无条件重写会把偏移永远钉在起点，表现为"设了滚动模式
  但根本不滚"。这是唯一的坑，加走马灯 label 时先想到它。
- **高度不能写死**。`LV_LABEL_LONG_SCROLL_CIRCULAR` 同时判纵向溢出，cjk 字库
  行高是 18px，沿用旧的 `lv_obj_set_height(v, 16)` 会让短文本被判成"竖着超出"
  而上下滚。`ui_label_marquee()` 内部按 `lv_font_get_line_height()` 兜了一次。
- 速度用 `lv_anim_speed_clamped()` 编码存进 `anim_duration` 样式，所以是恒定
  px/s，长文本不会因为距离长而变快。设备页取 30px/s（`DEVICE_MARQUEE_SPEED`）
  —— 单色屏是全屏刷新，再快就糊了。
- 验证办法：模拟器 `--page 3` 配 TCP 灌长文本，隔 ~700ms 抓两帧比对值区像素
  （`set ssid ...` / `set app_ver ...`，注意字段名是 `wifi_connected`）。
  只比整帧 md5 会被右上角 uptime 秒数干扰，必须按值区坐标切窗口比。

## 状态栏显示规则

- **WiFi**：connected=false → 满信号 + "\" 划掉；rssi ≥ -55 → 3 弧；-65 → 2 弧；-75 → 1 弧；<-75 → 仅圆点。
- **电池**：percent 分 4 档（25/50/75）段数；≤10% 加警示下划线。数据来自 `adc_bsp` 实测电压（3.0V→0% / 4.12V→100%）。硬件无充电检测引脚，**不做充电状态显示**。

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
