# tools

字模与天气图标生成工具。所有输出都是 LVGL binfont 二进制（`.bin`），
落到 `partitions/fonts/`，由 CMake `spiffs_create_partition_image()` 打包成
SPIFFS 镜像烧进 fonts 分区（2 MB）。运行时 `ui_font.c` 用
`lv_binfont_create("A:/spiffs/xxx.bin")` 流式加载。

## gen_font.sh — 主字库（3 份）

用 lv_font_conv（npm 包）从系统 TTF 生成 3 份字模到 `partitions/fonts/`：

- `ui_font_cjk_16.bin`（16px CJK，GB2312 一级 3755 字 + 标点 + ASCII）
- `ui_font_digit_big.bin`（96px 数字，时钟 HH:MM）
- `ui_font_digit_mid.bin`（28px 数字，卡片 24℃ / 68%）

```sh
export PATH=$HOME/.npm-global/bin:$PATH
bash tools/gen_font.sh
```

新增汉字：改 `gen_font.sh` 的字符集范围 → 重跑 → `idf.py flash-fonts`（只烧 2 MB
字库分区，不动 4 MB app）。**不用改 C 代码**。

## gen_mood_font.sh — 心情表情字库

生成 `ui_font_mood_16.bin`（主页"心情表情"专用）。自包含所有表情符号 +
中文标签字（CJK 字库里没有 ▽ 等符号，必须由 mood 字体提供，否则 fallback 后显示空白）。

```sh
export PATH=$HOME/.npm-global/bin:$PATH
bash tools/gen_mood_font.sh
```

## gen_weather_icons.py — 天气图标位图

把 QWeather 线稿 SVG 栅格化成 1-bit 位图，打包成 `ui_font_weather_40.bin`
（与字库文件同级，一起打包进 fonts 分区）。

- 数据源：`NEEDS/weather_icons/QWeather-Icons-1.8.0/icons/{code}.svg`（线稿版，**不用** `-fill` 填充版）
- 处理：cairosvg 4× 超采样栅格化 → LANCZOS 降到 40×40 → 128 阈值二值化
- 输出格式：`header('WETH'+ver+count) | index[count×8B] | data[各图标 w,h,bits 拼接]`
- 只打包实况 API 会返回的 62 个代码（100-153 / 300-399 / 400-499 / 500-515 / 900-999）

```sh
python3 tools/gen_weather_icons.py
```

运行时由 `ui_weather_icon.c` 的 `ui_weather_icon_load(img, code)` 按
`weather_code` 二分查索引 → 读数据 → 拼 `lv_image_dsc_t`（`LV_COLOR_FORMAT_I1`），
找不到代码时回落 999（未知图标）。

## binfo.py — 读 .bin 里的版本信息

不用烧板子、不用翻 `build/*.json`，直接从二进制里把版本读出来。发 Release
前核对"这个 bin 到底是哪一版、字库对不对"用这个。

```sh
python3 tools/binfo.py build/rlcd_home.bin        # 应用镜像
python3 tools/binfo.py build/*.bin                # 一次看多个
python3 tools/binfo.py -j build/rlcd_home.bin     # JSON，给脚本用
python3 tools/binfo.py --diff a_full.bin b_full.bin   # 两版逐项对比
```

自动判类型（判错时用 `-t app|full|spiffs|otadata` 强制）：

| 类型        | 典型文件                             | 读出什么                                                                       |
| ----------- | ------------------------------------ | ------------------------------------------------------------------------------ |
| `app`     | `rlcd_home.bin`（OTA 上传的）      | 版本 / 项目名 / 编译时间 / IDF 版本 / ELF SHA256 / 芯片，并校验镜像尾部 SHA256 |
| `full`    | `rlcd_home_*_full.bin`             | 解分区表 → 每个 app 槽各一份版本 + otadata 启动槽 + fonts 分区内容            |
| `spiffs`  | `fonts.bin`                        | 列字库文件；binfont 读字号/bpp/字数，`ui_font_weather_40.bin` 读图标数       |
| `otadata` | `ota_data_initial.bin` 或设备 dump | 两槽 ota_seq / 状态（`PENDING_VERIFY` 等）/ CRC，算出生效槽                  |

**`full.bin` 那条最有用**：`fonts` 分区不走 OTA，改过字库的版本必须整片重烧
（见 `.github/release_notes.md`）。发布前 `--diff` 一下新旧 `full.bin`，
如果 `fonts.*` 行有差异，说明这版**不能只 OTA**。字数对不上也能立刻发现
——`ui_font_cjk_16.bin` 应该是 7597 字，明显偏小就是 `gen_font.sh` 的字符集被改坏了。

解析全部是纯 `struct` 读字节，不依赖 esptool / ESP-IDF 环境，只用标准库。
