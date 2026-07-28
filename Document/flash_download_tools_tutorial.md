# 使用 flash_download_tools 烧录固件教程

> 适用于 Waveshare ESP32-S3-RLCD-4.2 开发板

## 一、准备工作

### 1. 硬件

- ESP32-S3-RLCD-4.2 开发板
- USB Type-C 数据线（**必须支持数据传输**，纯充电线无法使用）
- Windows 电脑（flash_download_tools 仅支持 Windows 平台）

### 2. 软件

从乐鑫官网下载 **Flash 下载工具 (ESP32-S3)**：

🔗 https://www.espressif.com.cn/zh-hans/support/download/other-tools

下载对应版本的 `flash_download_tools_x.x.x.zip`，解压到任意目录。

### 3. 固件

从本项目的 [Releases](https://github.com/silentmonkey/rlcd/releases) 页面下载最新版本的 `rlcd_home_*_full.bin` 文件。

> `full.bin` 是已经合并好的完整镜像，包含 bootloader、分区表、应用程序和字库，**只需烧录这一个文件**。

---

## 二、安装串口驱动

开发板通过 Type-C 连接电脑后：

1. 打开 **设备管理器**（Win+X → 设备管理器）
2. 查看 **端口 (COM 和 LPT)** 下是否出现新设备
   - 常见芯片：`USB-SERIAL CH340 (COMx)` 或 `Silicon Labs CP210x (COMx)`
3. 如果显示黄色叹号或未识别，需安装对应驱动：
   - CH340 驱动：https://www.wch.cn/downloads/CH341SER_ZIP.html
   - CP210x 驱动：https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

---

## 三、配置烧录参数

运行解压目录中的 `flash_download_tools_x.x.x.exe`，按以下步骤配置：

### 步骤 1：选择芯片类型

在工具顶部菜单栏选择：`Developer Mode` → `ESP32-S3` → `Download`

### 步骤 2：添加固件文件

1. 在 **spiAttFile** 行（第一行），点击 `...` 按钮
2. 选择下载的 `rlcd_home_*_full.bin` 文件
3. 在该行左侧的地址框中填入：**`0x0`**

> 因为 full.bin 已经包含 bootloader（位于 0x0），所以只需一个文件、地址填 0x0 即可。

### 步骤 3：勾选固件行

确保 **spiAttFile** 行左侧的复选框 **☑ 已勾选**。

### 步骤 4：设置 SPI 参数

| 参数 | 推荐值 | 说明 |
|---|---|---|
| SPI Speed | `80MHz` | 通信速率 |
| SPI Mode | `DIO` | 通信模式 |

### 步骤 5：选择串口和波特率

| 参数 | 推荐值 | 说明 |
|---|---|---|
| COM | 选择设备管理器中看到的 COM 口 | 如 COM3、COM5 |
| BAUD | `115200` | 烧录波特率，不稳定可降低；可尝试 460800/921600 提速 |

### 配置完成后的界面示意

```
┌────┬────────────────┬───────────┬──────────────────────────────────┐
│ ☑  │ spiAttFile     │ 0x0       │ [rlcd_home_v1.0.0_full.bin] ...  │
│ ☐  │                │ 0x8000    │                                  │
│ ☐  │                │ 0x10000   │                                  │
│ ☐  │                │ 0x410000  │                                  │
├────┴────────────────┴───────────┴──────────────────────────────────┤
│  SPI Speed: [80MHz ▼]    SPI Mode: [DIO ▼]                         │
│  COM:       [COM3 ▼]     BAUD:    [115200 ▼]                       │
│                                                                     │
│  [ERASE]  [START]  [STOP]                                          │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 四、进入下载模式

### 自动连接（推荐先尝试）

直接点击 **START**，工具会自动尝试连接。如果显示 `Connecting...` 后成功进入烧录流程，则无需手动操作。

### 手动进入下载模式（自动连接失败时）

如果工具一直停留在 `Connecting...` 或报错，需要手动操作：

1. **按住**开发板上的 **BOOT** 按键**不松手**
2. 按一下 **PWR** 按键（或重新插拔 Type-C 数据线上电）
3. **松开** BOOT 按键
4. 此时开发板进入下载模式，工具应能识别并自动开始烧录

> 💡 技巧：如果还是不行，可以尝试先按住 BOOT，再插 Type-C 线，然后松开 BOOT。

---

## 五、开始烧录

1. 点击底部 **START** 按钮
2. 工具会先 **擦除 (ERASE)** Flash，然后开始 **下载 (DOWNLOAD)**
3. 观察进度条，等待完成（通常 1~3 分钟，取决于固件大小和波特率）
4. 出现绿色 **FINISH** 字样表示烧录成功
5. 点击 **STOP** 停止
6. 按开发板上的 **PWR** 或 **RST** 按键重启

### 烧录过程日志示例

```
connecting...
Chip sync...
erase flash:
  progress: 100%
download file:
  progress: 100%
Leaving...
FINISH
```

---

## 六、验证烧录结果

重启后：

1. 开发板屏幕应**立即点亮**，显示家庭信息面板
2. 界面包含：顶部状态栏（WiFi/电池图标）、大字号时钟、温湿度/天气卡片
3. 如果屏幕无内容，检查供电或按 RST 重启

### 首次使用：配网

开发板出厂或清除配置后，首次启动会进入 **配网提示页**：

1. 手机搜索并连接名为 **`RLCD-Setup`** 的 WiFi 网络
2. 手机浏览器打开 **`http://192.168.4.1`**
3. 在配置页面填写：
   - **WiFi SSID**：家中 2.4GHz WiFi 名称（ESP32-S3 不支持 5GHz）
   - **WiFi Password**：WiFi 密码
   - **城市**：天气查询城市，如 `北京`、`上海`、`广州`
   - **QWeather API Key**：和风天气 API Key（在 https://dev.qweather.com 注册获取）
   - **QWeather API Host**：如 `xxx.re.qweatherapi.com`
4. 点击 **保存网络并重启**
5. 开发板重启后会自动连接 WiFi 并开始拉取天气数据

---

## 七、常见问题排查

### Q1：工具识别不到 COM 口

**排查步骤：**
- 确认 Type-C 数据线支持数据传输（换一根线试试）
- 检查设备管理器是否有未识别的设备（黄色叹号）
- 安装 CH340 或 CP210x 串口驱动
- 尝试更换 USB 口（优先使用主板后置 USB 口）
- 重启电脑后重试

### Q2：一直显示 Connecting... 无法连接

**排查步骤：**
- 手动进入下载模式（按住 BOOT → 上电 → 松开 BOOT）
- 降低波特率到 115200
- 缩短 Type-C 数据线长度
- 关闭可能占用 COM 口的软件（串口调试助手、Arduino IDE 等）
- 重新插拔数据线

### Q3：烧录成功但屏幕不显示

**排查步骤：**
- 确认固件是 **esp32s3** 目标编译（不是 esp32/esp32s2）
- 按 RST 按键重启开发板
- 检查开发板供电是否充足（Type-C 供电应足够）
- 确认下载的是 `full.bin`（完整镜像），不是单独的 `rlcd_home.bin`

### Q4：烧录后一直进入配网页面

**原因：** 开发板尚未配置 WiFi，或之前的配置已清除。

**解决：** 按上述「首次使用：配网」流程完成配置即可。

### Q5：如何更新固件

重复以上步骤重新烧录新版本的 `full.bin` 即可。重新烧录会覆盖应用和字库，但 **NVS 中的 WiFi 配置会被保留**（除非勾选了 ERASE）。

---

## 八、分区布局参考

本固件使用以下分区表（已包含在 full.bin 中）：

| 名称 | 偏移地址 | 大小 | 内容 |
|---|---|---|---|
| bootloader | `0x0` | ~24 KB | ESP32-S3 引导程序 |
| partition table | `0x8000` | 3 KB | 分区表 |
| factory (app) | `0x10000` | 4 MB | 主应用程序 |
| fonts (spiffs) | `0x410000` | 2 MB | 中文字库 + 天气图标 |

> 开发板搭载 **16MB Flash**，以上分区仅使用约 6MB，剩余空间可供后续扩展。

---

## 九、相关链接

- 乐鑫 Flash 下载工具：https://www.espressif.com.cn/zh-hans/support/download/other-tools
- 和风天气开发平台：https://dev.qweather.com
- 本项目 GitHub：https://github.com/silentmonkey/rlcd
