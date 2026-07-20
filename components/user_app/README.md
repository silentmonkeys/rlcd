# user_app — 后台数据采集

把传感器 / RTC / 芯片信息写入 ui_model，并触发 UI 刷新。

- 1Hz 任务：读 SHTC3 室内温湿度 → 本机时间 → 设备信息（heap/uptime）→ ui_pages_apply_locked()
- 启动时一次性填静态设备信息（chip_model / flash / mac / idf_ver / app_ver）
