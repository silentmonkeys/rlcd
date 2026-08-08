# rlcd — ESP32-S3 4.2" 单色屏智能面板（含桌面模拟器）

面向 Waveshare **ESP32-S3-RLCD-4.2**（400×300 单色反射 LCD）的家庭信息面板。
主界面按 [NEEDS/](NEEDS/)：状态栏 + 大字号 7 段 HH:MM + 三卡片（温度/湿度/天气），
另有 4 个子页（天气详情 / 日历 / 设备信息 / 配网提示）。

`components/ui/` 与桌面模拟器 **完全共用**，RGB565→mono 二值化（`<0x7fff` 黑）。

## 上手

```sh
# 真机
. ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py set-target esp32s3 && idf.py build && idf.py -p /dev/ttyUSB0 flash monitor

# 桌面模拟器
sudo apt install libsdl2-dev cmake build-essential
cd simulator && cmake -B build && cmake --build build -j && ./build/rlcd_sim
```

首次开机无 NVS → SoftAP `RLCD-Setup`，访问 http://192.168.4.1 配网。

## 目录结构

```
rlcd/
├── CLAUDE.md              对 Claude 的工作规范
├── README.md              本文件
├── CMakeLists.txt / sdkconfig* / partitions.csv / dependencies.lock
├── _logs/                 历史对话 log（手动归档）
├── main/                   app_main + user_config.h（引脚/NVS）[README](main/README.md)
├── components/
│   ├── port_bsp/           硬件抽象（Display / I2C / 按键）[README](components/port_bsp/README.md)
│   ├── app_bsp/            LVGL v9 端口         [README](components/app_bsp/README.md)
│   ├── ui/                 ★ 与 simulator 共享的 UI 代码 [README](components/ui/README.md)
│   ├── net_bsp/            WiFi / 配网 / 天气     [README](components/net_bsp/README.md)
│   └── user_app/           传感器→ui_model 桥     [README](components/user_app/README.md)
├── simulator/              SDL2 桌面模拟器        [README](simulator/README.md)
├── tools/gen_font.sh       字模生成工具           [README](tools/README.md)
└── NEEDS/                  参考 PNGs
```

每个子目录的 README 里有该模块职责、公开 API、依赖、注意事项。

## 数据流

```
SHTC3 温湿度 / 电池 ADC ─┐
                         ▼
net_bsp weather ──▶ ui_model_t (全局单例, ui/include/ui_model.h)
                         ▲
                         │  Lvgl_lock(); ui_pages_apply_locked(); Lvgl_unlock()
                         └── user_app 每秒 tick_task
```

UI 代码对硬件、网络、模拟器一无所知，只读写 `ui_model_t`。

## 引脚 / 硬件

见 `main/user_config.h`（SPI3 MOSI=12 SCK=11 DC=5 CS=40 RST=41；I2C SDA=13 SCL=14；按键 BOOT=0 KEY=18）。

## 已知 TODO

- PCF85063 RTC 接入（做后备时钟；待硬件到货，方案见 `_logs/plan-refactor-2026-07-21.md`）
