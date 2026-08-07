# simulator — 桌面 SDL2 模拟器

用 SDL2 + 同一份 ui_home.c 显示 400×300 单色屏效果。UI 代码（`components/ui/`）
与真机**完全共用**，RGB565→mono 二值化（`<0x7fff` 黑）。

## 构建

```sh
sudo apt install libsdl2-dev cmake build-essential
cd simulator && cmake -B build && cmake --build build -j
```

## 命令行参数

```
./build/rlcd_sim
./build/rlcd_sim --page N                    # 启动切到第 N 页（0=HOME,1=WEATHER,2=CALENDAR,3=DEVICE）
./build/rlcd_sim --temp <T>                  # 强制室内温度（测试心情表情）
./build/rlcd_sim --humi <H>                  # 强制室内湿度
./build/rlcd_sim --capture /tmp/out.ppm      # 渲染后存帧退出（无窗口）
./build/rlcd_sim --capture-ms 1500           # 抓帧前等待的毫秒（默认 1500）
./build/rlcd_sim --gallery                   # 循环切换 8 种天气图标（1.5s/张）
./build/rlcd_sim --gallery-capture dir/      # 8 张连拍到目录，退出
```

键（窗口模式）：`ESC` 退出 / `S` 存帧 / `+ -` 缩放。

## 控制台

非 capture 模式下，后台线程读 stdin，主线程 drain 队列执行（不在 LVGL 线程
直接调，避免多线程崩溃）。同时监听 **TCP 127.0.0.1:9000**，可用 `nc` 远程发送
同样命令（便于自动化/调试 GUI 接入）。

### 新协议（推荐）

```
set <field> <value>     # 写 ui_model 字段（如 set outdoor_temp -5.3）
get <field>             # 读字段当前值 → 回 `val <field> <value>`
dump                    # 读全部字段，每行一条 val
ovr                     # 列出当前被 override 的字段名
clear                   # 清除所有 override（恢复模拟数据）
help                    # 列帮助（字段列表由 FIELDS[] 自动生成，不会过期）
quit                    # 退出
```

`set` / `get` 支持的字段由 `sim_console.c` 的 **`FIELDS[]` 字段表**决定
（48 个，覆盖时间 / 室内 / 天气 / 状态栏 / 设备信息 / 配网 / SD·Flash），
跑 `help` 现场列出即可，不在这里重复维护。**加字段只要两处**：
`FIELDS[]` 加一行 `FLD(...)` + `sim_console.h` 加一个 `OVR_*` 位。

> **帧终止**：每条命令的响应之后**再发一个空行**（`"\n"`）作为帧结束标记，
> 客户端读到空行即可确定一帧读完，无需靠 recv 超时判断结束
> （`dump` 这类多行响应也因此能被正确切分）。逐行读的旧客户端
> （`tools/rlcd_debug`）不受影响 —— 空行只是它下一次读到的空串。

### 日历页命令

```
mark MM-DD              # 标注日期（高亮方块）
unmark MM-DD            # 清除单条标注
marks 01-01,10-01,12-25 # 批量标注（逗号分隔）
events 01-01=元旦;...   # 设置预定事件（分号分隔）
labels a;b;c;...        # 设置底部随机标签
```

### 兼容旧语法

```
weather <code> [text]   # ≡ set weather_code + set weather_text
temp <T> [H]            # ≡ set indoor_temp + set indoor_humi
page N / next / prev    # 切页
```

## 图形化调试客户端

`python3 tools/rlcd_debug_gui/rlcd_debug_gui.py` 走同一个 TCP 协议，
左右分栏一次看完 48 个字段 + 12 个场景预设，见
[tools/rlcd_debug_gui/README.md](../tools/rlcd_debug_gui/README.md)。

## 注意

- main.c 启动顺序：先 `sim_tick_data()` 把 ui_model 填好，再 `ui_pages_create()`，
  这样初始状态栏（电池/WiFi）画的是终态（82% / 3 弧满信号），不会画出默认值 0 的空框。
- `sim_tick_data()` **每 500ms 一拍**，且每拍都按 override 位决定要不要重填 ——
  所以 `clear` 之后要等 >500ms 才读得到恢复后的默认值。
- 抓帧模式无 stdin 线程（靠 `SDL_VIDEODRIVER=dummy` 无窗口渲染）。
- 字库从 `partitions/fonts/*.bin` 加载（与真机同一批文件，保证像素一致），
  缺字库会打印路径提示。
- 字库从 `partitions/fonts/*.bin` 加载（与真机同一批文件，保证像素一致），
  缺字库会打印路径提示。
