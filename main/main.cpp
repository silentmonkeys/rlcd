// 顶层入口：初始化硬件 → 启动 LVGL 端口 → 建 UI → 起后台任务
//
// 单色屏 400x300：LVGL 使用 RGB565 全屏渲染，flush 回调里做 < 0x7fff → Black
// 的二值化（沿用 09_LVGL_V9_Test 的做法）。UI 代码本身对屏幕类型无感知，
// 因此同一份 ui/ 组件可以直接在 SDL 模拟器里运行。

#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"
#include "user_app.h"
#include "net_bsp.h"
#include "ui_home.h"
#include "ui_pages.h"
#include "button_bsp.h"

static const char *TAG = "app_main";

static DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN,
                            RLCD_CS_PIN,   RLCD_RST_PIN,
                            LCD_WIDTH,     LCD_HEIGHT);

// LVGL flush：把 RGB565 帧缓冲逐像素二值化写入 RLCD，然后触发一次刷新。
// RLCD 面板本身是全屏刷新的（RLCD_Display 会重发整张 DispBuffer），所以对
// 频繁小区域重绘并不友好——用 LV_DISPLAY_RENDER_MODE_FULL 让 LVGL 每次都
// 把整屏送过来，代价可控。
static void lvgl_flush_cb(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
    uint16_t *buffer = (uint16_t *) color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            RlcdPort.RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    RlcdPort.RLCD_Display();
    lv_disp_flush_ready(drv);
}

extern "C" void app_main(void)
{
    // 1. NVS —— 保存 WiFi / weather / 城市 等配置
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 2. 应用后端（传感器 / RTC / 数据模型）
    UserApp_AppInit();

    // 3. 显示
    RlcdPort.RLCD_Init();
    Lvgl_PortInit(LCD_WIDTH, LCD_HEIGHT, lvgl_flush_cb);

    if (Lvgl_lock(-1)) {
        ui_pages_create();          // 建所有页面，默认激活主页
        Lvgl_unlock();
    }

    // 4. 数据 → UI 的刷新任务（先起来，UI 立即开始跑，不依赖网络）
    UserApp_TaskInit();

    // 5. 按键（BOOT 键切页）
    ButtonBsp_Init();

    // 6. 网络后台（WiFi 连接 / 配网 web server / 天气轮询）
    NetBsp_Start();

    ESP_LOGI(TAG, "boot complete");
}
