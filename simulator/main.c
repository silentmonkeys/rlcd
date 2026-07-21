/*
 * rlcd_sim —— 桌面模拟器
 *
 * 把 LVGL 渲染到一个 400x300 的 SDL 窗口，并且严格按真机上 flush 回调
 * 里的 `< 0x7fff -> Black` 二值化规则显示，让你在开发时就能看到真机
 * 的最终画面。
 *
 * 键：
 *   ESC / 关窗口   —— 退出
 *   TAB / SPACE    —— 下一页（对应真机 BOOT 键）
 *   S              —— 保存当前帧到 frame.ppm (方便贴到 issue)
 *   +/-            —— 缩放窗口（放大更容易看清像素结构）
 */

#include "lvgl.h"
#include "ui_home.h"
#include "ui_device.h"
#include "ui_pages.h"
#include "ui_calendar.h"
#include "ui_model.h"

#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define LCD_W   400
#define LCD_H   300
#define SCALE   2       // 窗口放大倍率

// 硬件像素——0/255，只有两种值
static uint8_t framebuf[LCD_W * LCD_H];

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;

static uint32_t millis(void)
{
    return SDL_GetTicks();
}

// LVGL v9 flush cb —— 参数与真机 main.cpp:lvgl_flush_cb 完全一致
static void sim_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t *buffer = (uint16_t *) px_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            // 与真机相同：亮度 <0x7fff 视为黑
            uint8_t color = (*buffer < 0x7fff) ? 0 : 0xff;
            framebuf[y * LCD_W + x] = color;
            buffer++;
        }
    }

    // 把 framebuf 灌到 SDL texture
    void *pixels;
    int pitch;
    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
        uint32_t *out = (uint32_t *) pixels;
        for (int y = 0; y < LCD_H; y++) {
            for (int x = 0; x < LCD_W; x++) {
                uint8_t c = framebuf[y * LCD_W + x];
                // 单色反射式 LCD 只有"黑"和"不显示"两态 —— 用纯黑/纯白模拟。
                if (c == 0) out[y * (pitch / 4) + x] = 0xFF000000;
                else        out[y * (pitch / 4) + x] = 0xFFFFFFFF;
            }
        }
        SDL_UnlockTexture(texture);
    }

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    lv_display_flush_ready(disp);
}

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", LCD_W, LCD_H);
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        uint8_t c = framebuf[i] ? 0xFF : 0x00;
        uint8_t rgb[3] = { c, c, c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("saved %s\n", path);
}

// 8 种天气名，用于 --gallery 循环展示
static const char *GALLERY_WEATHER[] = {
    "晴", "多云", "阴", "小雨", "大雪", "雷阵雨", "雾", "unknown-xxx",
};
#define GALLERY_N ((int)(sizeof(GALLERY_WEATHER)/sizeof(GALLERY_WEATHER[0])))
static int  s_gallery_idx = 0;
static bool s_gallery_mode = false;

// 让模拟器的 ui_model 数据像真机一样每秒更新
static void sim_tick_data(void)
{
    ui_model_t *m = ui_model_get();
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    m->hour = tm_local.tm_hour;
    m->minute = tm_local.tm_min;
    m->year = tm_local.tm_year + 1900;
    m->month = tm_local.tm_mon + 1;
    m->day = tm_local.tm_mday;
    m->weekday = tm_local.tm_wday;

    static float phase = 0;
    phase += 0.02f;
    m->indoor_temp = 24.0f + 1.5f * sinf(phase);
    m->indoor_humi = 55.0f + 4.0f * cosf(phase * 1.3f);
    m->outdoor_temp = 27.0f + 2.0f * sinf(phase * 0.4f);

    if (s_gallery_mode) {
        strncpy(m->weather_text, GALLERY_WEATHER[s_gallery_idx],
                sizeof(m->weather_text) - 1);
        m->weather_text[sizeof(m->weather_text) - 1] = 0;
    } else {
        strcpy(m->weather_text, "多云");
    }
    strcpy(m->city, "北京");
    snprintf(m->weather_update, sizeof(m->weather_update), "%02d:%02d", m->hour, m->minute);

    // --- 天气详情页假数据 ---
    m->outdoor_temp     = 27.0f + 2.0f * sinf(phase * 0.4f);
    m->outdoor_humi     = 62.0f;
    m->feels_like_temp  = m->outdoor_temp + 1.5f;
    m->wind_speed_kmh   = 12.0f;
    strcpy(m->wind_dir, "东北");
    m->cloud_pct        = 45;
    m->pressure_hpa     = 1013;
    m->visibility_km    = 10;
    m->uv_index         = 5;
    m->temp_min         = 22;
    m->temp_max         = 31;
    strcpy(m->sunrise, "06:12");
    strcpy(m->sunset,  "18:45");

    m->wifi_connected  = false;
    m->wifi_rssi       = 0;
    m->battery_percent = 80;
    m->battery_charging = false;

    // --- 设备信息页假数据（真机由 net_bsp / esp_chip_info 填） -------
    strcpy(m->chip_model, "ESP32-S3");
    m->cpu_cores        = 2;
    m->flash_size_mb    = 8;
    m->free_heap_kb     = 215;
    strcpy(m->idf_ver,   "v6.0.1");
    strcpy(m->app_ver,   "RLCD 0.1");
    strcpy(m->ssid,      "MyPhone-2.4G");
    strcpy(m->ip,        "10.217.129.06");
    strcpy(m->mac,       "84:F7:03:6C:AA:BB");

    // --- SD 卡假数据（真机由 sdcard_bsp / user_app 填） ---
    m->sd_mounted   = true;
    m->sd_total_mb  = 32 * 1024;    // 32 GB
    m->sd_used_mb   = 3 * 1024 + 200;
    // --- Flash 用量假数据（真机由 user_app 扫描分区表填） ---
    m->flash_used_kb = 4 * 1024;    // 4 MB 已用
    m->flash_free_kb = 12 * 1024;   // 12 MB 剩余

    // --- 配网页假数据 ---
    strcpy(m->ap_ssid, "RLCD-Setup");
    strcpy(m->ap_ip,   "192.168.4.1");
    static uint32_t s_started = 0;
    if (!s_started) s_started = (uint32_t)time(NULL);
    m->uptime_sec = (uint32_t)time(NULL) - s_started;

    ui_pages_apply_locked();
}

// 后台线程读 stdin，接收 "mark MM-DD" / "unmark" 命令，标注日历页
static void *stdin_cmd_thread(void *arg)
{
    (void)arg;
    char line[128];
    fprintf(stderr, "[console] commands: 'mark MM-DD'  |  'unmark'  |  'page N' (0=home,1=weather,2=calendar,3=device)\n");
    while (fgets(line, sizeof(line), stdin)) {
        // 去尾部换行
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "mark ", 5) == 0) {
            ui_calendar_mark_date(line + 5);
            fprintf(stderr, "[console] marked %s\n", line + 5);
        } else if (strcmp(line, "unmark") == 0 || strcmp(line, "clear") == 0) {
            ui_calendar_mark_date(NULL);
            fprintf(stderr, "[console] cleared marks\n");
        } else if (strncmp(line, "page ", 5) == 0) {
            int p = atoi(line + 5);
            if (p >= 0 && p < (int)UI_PAGE_COUNT) {
                ui_pages_switch_to(p);
                fprintf(stderr, "[console] switched to page %d\n", p);
            }
        } else if (line[0]) {
            fprintf(stderr, "[console] unknown: %s\n", line);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    // --capture <path>          —— 无窗口，渲染几秒后存 PPM 退出
    // --gallery                 —— 循环切换 8 种天气图标（1.5s/张）
    // --gallery-capture <dir>   —— 8 张连拍到目录，退出
    // --page <id>               —— 启动时切到指定页（0=home, 1=device）
    const char *capture_path = NULL;
    const char *gallery_dir = NULL;
    int capture_ms = 1500;
    int start_page = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            capture_path = argv[++i];
        } else if (!strcmp(argv[i], "--capture-ms") && i + 1 < argc) {
            capture_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            start_page = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gallery")) {
            s_gallery_mode = true;
        } else if (!strcmp(argv[i], "--gallery-capture") && i + 1 < argc) {
            gallery_dir = argv[++i];
            s_gallery_mode = true;
        }
    }
    if (capture_path || gallery_dir) setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("RLCD 4.2\" simulator (400x300)",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              LCD_W * SCALE, LCD_H * SCALE, 0);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, LCD_W, LCD_H);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, LCD_W, LCD_H);

    // --- LVGL v9 初始化 -------------------------------------------
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    static uint8_t draw_buf_1[LCD_W * LCD_H * 2];
    static uint8_t draw_buf_2[LCD_W * LCD_H * 2];
    lv_display_set_buffers(disp, draw_buf_1, draw_buf_2, sizeof(draw_buf_1),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, sim_flush_cb);

    // 注意顺序：ui_pages_create 会画初始状态栏（此时 model 字段还是默认值 0）
    // → 所以先立刻 sim_tick_data 把 model 填好，再 create，这样初始状态栏就是终态
    sim_tick_data();
    ui_pages_create();
    ui_pages_apply_locked();
    if (start_page >= 0 && start_page < (int)UI_PAGE_COUNT) {
        ui_pages_switch_to_locked((ui_page_id_t)start_page);
    }

    // 后台线程处理控制台命令（不在 --capture / --gallery-capture 无窗口模式下启动）
    if (!capture_path && !gallery_dir) {
        pthread_t th;
        pthread_create(&th, NULL, stdin_cmd_thread, NULL);
        pthread_detach(th);
    }

    // --- 主循环 -----------------------------------------------------
    uint32_t last_tick = millis();
    uint32_t last_data = 0;
    uint32_t last_gallery = 0;
    uint32_t started   = millis();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                else if (ev.key.keysym.sym == SDLK_s) save_ppm("frame.ppm");
                else if (ev.key.keysym.sym == SDLK_TAB ||
                         ev.key.keysym.sym == SDLK_SPACE) ui_pages_next_locked();
            }
        }

        uint32_t now = millis();
        lv_tick_inc(now - last_tick);
        last_tick = now;

        if (s_gallery_mode && now - last_gallery >= 1500) {
            last_gallery = now;
            if (gallery_dir) {
                char path[256];
                snprintf(path, sizeof(path), "%s/gallery_%d_%s.ppm",
                         gallery_dir, s_gallery_idx, GALLERY_WEATHER[s_gallery_idx]);
                sim_tick_data();
                lv_timer_handler();
                save_ppm(path);
                s_gallery_idx++;
                if (s_gallery_idx >= GALLERY_N) running = false;
            } else {
                s_gallery_idx = (s_gallery_idx + 1) % GALLERY_N;
            }
        }

        if (now - last_data > 500) {
            sim_tick_data();
            last_data = now;
        }

        lv_timer_handler();
        SDL_Delay(5);

        if (capture_path && (int)(now - started) >= capture_ms) {
            save_ppm(capture_path);
            running = false;
        }
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
