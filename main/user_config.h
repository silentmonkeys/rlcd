#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// --- RLCD 4.2" 400x300 硬件接线（与 09_LVGL_V9_Test 相同） -----
#define LCD_WIDTH        400
#define LCD_HEIGHT       300

#define RLCD_MOSI_PIN    12
#define RLCD_SCK_PIN     11
#define RLCD_DC_PIN      5
#define RLCD_CS_PIN      40
#define RLCD_RST_PIN     41

// I2C（SHTC3 温湿度 + RTC，若板上有）
#define I2C_SDA_PIN      13
#define I2C_SCL_PIN      14

// 按键引脚定义在 components/port_bsp/button_bsp.c 里内联（避免 port_bsp 反过来依赖 main）

// NVS 命名空间
#define NVS_NAMESPACE    "rlcd_cfg"

#endif
