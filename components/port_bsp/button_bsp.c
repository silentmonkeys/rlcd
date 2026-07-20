// 按键 BSP —— 两个按键映射：
//   BOOT  短按 → 下一页（ui_pages_next）
//   BOOT  长按 → 重建全部页面（ui_pages_rebuild）
//   KEY   短按 → 上一页（ui_pages_prev）
//   KEY   长按 → 在 WEATHER 页时立即拉取一次天气（NetBsp_TriggerWeatherFetch）
//
// 引脚：直接在这里内联，避免拉 main 组件（防止循环依赖）。
// 参考 10_FactoryProgram/components/port_bsp/button_bsp.c。

#include "button_bsp.h"
#include "multi_button.h"
#include "ui_pages.h"
#include "net_bsp.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

// 按键引脚（板上实际引脚可依原理图修改）
#define BTN_BOOT_PIN   0
#define BTN_KEY_PIN    18

#define BOOT_KEY_ID    1
#define KEY_KEY_ID     2
#define BTN_ACTIVE     0   // 两个键都是按下拉低

static const char *TAG = "button_bsp";
static Button s_boot_btn;
static Button s_key_btn;

// ---------- BOOT ----------
static void on_boot_click(Button *btn)
{
    (void)btn;
    ESP_LOGI(TAG, "BOOT click → next page");
    ui_pages_next();
}

static void on_boot_long(Button *btn)
{
    (void)btn;
    ESP_LOGI(TAG, "BOOT long press → rebuild UI");
    ui_pages_rebuild();
}

// ---------- KEY ----------
static void on_key_click(Button *btn)
{
    (void)btn;
    ESP_LOGI(TAG, "KEY click → prev page");
    ui_pages_prev();
}

// 长按 KEY：只在 WEATHER 详情页触发，立即拉一次天气数据；
// 其它页面上长按无副作用（避免误触）。
static void on_key_long(Button *btn)
{
    (void)btn;
    if (ui_pages_current() == UI_PAGE_WEATHER) {
        ESP_LOGI(TAG, "KEY long press on WEATHER → trigger weather fetch");
        NetBsp_TriggerWeatherFetch();
    } else {
        ESP_LOGI(TAG, "KEY long press (page=%d, ignored)", (int)ui_pages_current());
    }
}

// HAL 读引脚电平
static uint8_t read_button_gpio(uint8_t id)
{
    switch (id) {
        case BOOT_KEY_ID: return gpio_get_level(BTN_BOOT_PIN);
        case KEY_KEY_ID:  return gpio_get_level(BTN_KEY_PIN);
        default:          return 1;
    }
}

// 5ms 周期 tick
static void button_tick_cb(void *arg) { button_ticks(); }

static void button_gpio_config(void)
{
    gpio_config_t cfg = {};
    cfg.intr_type     = GPIO_INTR_DISABLE;
    cfg.mode          = GPIO_MODE_INPUT;
    cfg.pin_bit_mask  = (1ULL << BTN_BOOT_PIN) |
                        (1ULL << BTN_KEY_PIN);
    cfg.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    cfg.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
}

void ButtonBsp_Init(void)
{
    button_gpio_config();

    // BOOT
    button_init(&s_boot_btn, read_button_gpio, BTN_ACTIVE, BOOT_KEY_ID);
    button_attach(&s_boot_btn, BTN_SINGLE_CLICK,     on_boot_click);
    button_attach(&s_boot_btn, BTN_LONG_PRESS_START, on_boot_long);
    button_start(&s_boot_btn);

    // KEY
    button_init(&s_key_btn, read_button_gpio, BTN_ACTIVE, KEY_KEY_ID);
    button_attach(&s_key_btn, BTN_SINGLE_CLICK,     on_key_click);
    button_attach(&s_key_btn, BTN_LONG_PRESS_START, on_key_long);
    button_start(&s_key_btn);

    // 5ms tick 定时器
    esp_timer_create_args_t args = {};
    args.callback = &button_tick_cb;
    args.name     = "btn_tick";
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000 * TICKS_INTERVAL));

    ESP_LOGI(TAG, "BOOT=GPIO%d KEY=GPIO%d",
             BTN_BOOT_PIN, BTN_KEY_PIN);
}
