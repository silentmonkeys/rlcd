# user_app — 后台数据采集

把传感器 / 电池 / 芯片信息写入 ui_model，并触发 UI 刷新。

- 1Hz tick 任务：读 SHTC3 室内温湿度 → 本机时间 → 设备信息（heap/uptime）→
  **5s 慢节拍**（电池采样 + 电压趋势推断充电态 + SD 卡热插拔探活 +
  `NetBsp_OfflineWatchdogTick()`）→ `ui_pages_apply_locked()`
- CSV 日志任务：每 10min 一行，无网也写；`fflush + fsync` 确保落盘，SD 未挂载静默跳过
- 启动时一次性填静态设备信息（chip_model / flash / mac / idf_ver / app_ver）

> 传感器/设备采样与网络看门狗都收敛在这条 tick 主线，不再各开独立任务/倒计时。

## 数据源现状

| 字段 | 来源 |
|---|---|
| 室内温湿度 | SHTC3（i2c_bsp）|
| 电池电量 % | ADC1_CH3/GPIO4（adc_bsp）实测电压映射 |
| 电池充电态 | 电压趋势启发式（无充电检测引脚，连续上升判充电）|
| 时间 | 系统本地时钟（SNTP 校时；RTC 待硬件到货再接）|
