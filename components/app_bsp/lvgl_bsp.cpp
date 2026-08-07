#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "lvgl_bsp.h"

static SemaphoreHandle_t lvgl_mux = NULL;
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))


static const char *TAG = "LvglPort";

static void Increase_lvgl_tick(void *arg)
{
  	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool Lvgl_lock(int timeout_ms)
{
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

void Lvgl_unlock(void)
{
  	assert(lvgl_mux && "bsp_display_start must be called first");
  	xSemaphoreGive(lvgl_mux);
}

// LVGL 渲染任务栈。CJK 字体渲染（16px 6763 字字库 + 96px 时钟）在 glyph
// 解码路径上会用掉不少局部缓冲，8 KiB 余量偏薄；抬到 12 KiB 并周期性报告
// 高水位，方便在真机日志里确认实际余量（低于 1 KiB 就该继续加）。
#define LVGL_TASK_STACK_BYTES   (12 * 1024)
#define LVGL_STACK_REPORT_MS    (60 * 1000)
#define LVGL_STACK_WARN_BYTES   1024

static void Lvgl_report_stack(void)
{
    static uint32_t s_next_report_ms = LVGL_STACK_REPORT_MS;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms < s_next_report_ms) return;
    s_next_report_ms = now_ms + LVGL_STACK_REPORT_MS;

    size_t free_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (free_bytes < LVGL_STACK_WARN_BYTES) {
        ESP_LOGW(TAG, "LVGL 任务栈余量仅 %u B（栈 %u B），建议加大",
                 (unsigned)free_bytes, (unsigned)LVGL_TASK_STACK_BYTES);
    } else {
        ESP_LOGI(TAG, "LVGL 任务栈余量 %u B / %u B",
                 (unsigned)free_bytes, (unsigned)LVGL_TASK_STACK_BYTES);
    }
}

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	for(;;)
  	{
  	  	if (Lvgl_lock(-1))
  	  	{
  	  	  	task_delay_ms = lv_timer_handler();
  	  	  	//Release the mutex
  	  	  	Lvgl_unlock();
  	  	}
  	  	if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	  	} else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
  	  	}
  	  	Lvgl_report_stack();
  	  	vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  	}
}


void Lvgl_PortInit(int width, int height, DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_display_t * disp = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(disp, flush_cb);
	
	size_t buffer_size = width * height * BYTES_PER_PIXEL;
	uint8_t *buffer_1 = NULL;
    uint8_t *buffer_2 = NULL;
    buffer_1 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
	buffer_2 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    assert(buffer_2);

    lv_display_set_buffers(disp, buffer_1, buffer_2, buffer_size, LV_DISPLAY_RENDER_MODE_FULL);

    ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	lvgl_tick_timer_args.callback = &Increase_lvgl_tick;
  	lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreatePinnedToCore(Lvgl_port_task, "LVGL", LVGL_TASK_STACK_BYTES, NULL, 5, NULL, 0);
}
