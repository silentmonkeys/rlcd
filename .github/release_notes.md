适用于 **Waveshare ESP32-S3-RLCD-4.2**（400×300 单色反射 LCD）的一键烧录固件包。

## 包含文件

| 文件 | 说明 |
|---|---|
| `rlcd_home_*_full.bin` | 完整固件（bootloader + 分区表 + 应用 + 字库），可直接烧录 |
| `ESP32-S3-RLCD-4.2-resource.md` | 开发板资源介绍 |

## 硬件准备

- ESP32-S3-RLCD-4.2 开发板
- USB Type-C 数据线
- Windows 电脑（flash_download_tools 仅支持 Windows）

## 使用 flash_download_tools 烧录

### 1. 下载工具

从乐鑫官网下载 **Flash 下载工具 (ESP32-S3)**：
https://www.espressif.com.cn/zh-hans/support/download/other-tools

解压后运行 `flash_download_tools_x.x.x.exe`。

### 2. 配置烧录参数

打开工具后按以下步骤设置：

1. **选择芯片**：点击顶部菜单选择 `ESP32-S3`
2. **勾选固件**：点击 `...` 按钮选择本 Release 中的 `rlcd_home_*_full.bin`
3. **填写烧录地址**：在固件行左侧的地址框中填入 `0x0`（因为 full.bin 已包含 bootloader，从 0 地址开始烧）
4. **勾选 SPI 速率/模式**（保持默认通常即可）：
   - SPI Speed: `80MHz`
   - SPI Mode: `DIO`
5. **选择端口**：开发板通过 Type-C 连接电脑后，在 `COM` 口下拉框中选择对应的串口（可在设备管理器中查看）
6. **波特率**：建议 `115200`，可尝试 `460800` 或 `921600` 提速

配置示意：

```
┌─────────────────────────────────────────────────────┐
│  ☑  spiAttFile    0x0      [rlcd_home_*_full.bin]  │
│  ☐                0x8000                           │
│  ...                                               │
│  SPI Speed:  [80MHz ▼]   SPI Mode: [DIO ▼]         │
│  COM:        [COM3 ▼]        BAUD: [115200 ▼]      │
└─────────────────────────────────────────────────────┘
```

### 3. 进入下载模式

> 如果工具无法连接，需要手动进入下载模式：
> 1. 按住开发板上的 **BOOT** 按键不松手
> 2. 按一下 **PWR** 按键（或重新插拔 Type-C 上电）
> 3. 松开 BOOT 按键
> 4. 此时 flash_download_tools 应能识别到 ESP32-S3

### 4. 开始烧录

1. 点击底部 **START** 按钮
2. 等待进度条完成（通常 1~3 分钟，取决于固件大小和波特率）
3. 出现 **FINISH** 字样表示烧录成功
4. 按一下 **PWR** 或 **RST** 按键重启开发板

### 5. 验证

烧录完成后重启，开发板屏幕应显示家庭信息面板（状态栏 + 时钟 + 温湿度卡片）。

首次启动若无 WiFi 配置，会自动进入 **配网提示页**：
- 手机连接 `RLCD-Setup` WiFi
- 浏览器打开 `192.168.4.1`
- 填写 WiFi 凭据、城市（如 `北京`）、QWeather API Key 后保存重启

## 常见问题

| 现象 | 排查 |
|---|---|
| 工具识别不到 COM 口 | 检查 Type-C 数据线是否支持数据（非纯充电线）；安装 CH340/CP210x 驱动 |
| 连接超时 / 无法同步 | 确认已进入下载模式（按住 BOOT 再上电）；换低波特率 115200 重试 |
| 烧录成功但屏幕无显示 | 检查开发板供电；按 RST 重启；确认固件是 esp32s3 目标编译 |
| 配网页不弹出 | 开发板已保存过 WiFi 配置；长按 BOOT 按键重建 UI |

## 分区布局

| 名称 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| bootloader | 0x0 | ~24 KB | 引导程序 |
| partition table | 0x8000 | 3 KB | 分区表 |
| factory (app) | 0x10000 | 4 MB | 主应用程序 |
| fonts (spiffs) | 0x410000 | 2 MB | 字库 + 天气图标 |
