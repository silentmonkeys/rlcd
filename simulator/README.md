# simulator — 桌面 SDL2 模拟器

用 SDL2 + 同一份 ui_home.c 显示 400×300 单色屏效果。

## 构建

```sh
sudo apt install libsdl2-dev cmake build-essential
cd simulator && cmake -B build && cmake --build build -j
```

## 命令行

```
./build/rlcd_sim
./build/rlcd_sim --page N                    # 启动切到第 N 页（0=HOME,1=WEATHER,2=CALENDAR,3=DEVICE）
./build/rlcd_sim --capture /tmp/out.ppm      # 渲染后存帧退出
./build/rlcd_sim --capture-ms 1500           # 抓帧前等待的毫秒（默认 1500）
./build/rlcd_sim --gallery                   # 循环切 8 种天气名
./build/rlcd_sim --gallery-capture dir/      # 8 张连拍
```

## 控制台（非 capture 模式下后台线程读 stdin）

```
mark MM-DD    # 标注日期到日历页（高亮方块）
unmark/clear  # 清除标注
page N        # 切页
```

## 注意

- main.c 启动顺序：先 sim_tick_data() 把 ui_model 填好，再 ui_pages_create()，这样初始状态栏（电池/WiFi）画的是终态（82% / 3 弧满信号），不会画出默认值 0 的空框。
- 抓帧模式无 stdin 线程（靠 SDL_VIDEODRIVER=dummy 无窗口渲染）。
