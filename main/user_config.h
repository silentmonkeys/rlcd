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

// I2C（SHTC3 温湿度 + RTC + 音频 codec 控制）
#define I2C_SDA_PIN      13
#define I2C_SCL_PIN      14

// --- 音频（ES8311 DAC 出声 + ES7210 双麦阵列收音）-----------
// 引脚来自 Document/rlcd/ESP32-S3-RLCD-pin_assignment.csv 的 ES8311 列。
// 两个 codec 共享 I2S 时钟（MCLK/BCLK/LRCK），数据线各走各的：
// DOUT 送 ES8311 播放，ASDOUT 收 ES7210 麦克风。I2C 复用 SHTC3 那条总线。
#define AUDIO_I2S_MCLK_PIN   16
#define AUDIO_I2S_BCLK_PIN   9
#define AUDIO_I2S_LRCK_PIN   45
#define AUDIO_I2S_DOUT_PIN   8     // ESP32 → ES8311（播放）
#define AUDIO_I2S_DIN_PIN    10    // ES7210 → ESP32（收音）
#define AUDIO_PA_PIN         46    // 功放使能，高电平开
#define AUDIO_PA_ACTIVE      1
#define AUDIO_ES8311_ADDR    0x18  // ES8311 I2C 地址（CE 拉低默认值）
#define AUDIO_ES7210_ADDR    0x20  // ES7210 I2C 地址（AD[1:0]=00 默认值）

// 按键引脚定义在 components/port_bsp/button_bsp.c 里内联（避免 port_bsp 反过来依赖 main）

// NVS 命名空间
#define NVS_NAMESPACE    "rlcd_cfg"

#endif
