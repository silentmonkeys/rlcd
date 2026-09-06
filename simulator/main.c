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
#include "ui_font.h"
#include "sim_console.h"
#include "bloub_bot.h"

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

// --bot-frame 离线渲染的位图尺寸（与 Node 真值采样同尺寸才有逐像素可比性）
#define BOT_OUT_SIZE   160
#define BOT_OUT_STRIDE ((BOT_OUT_SIZE + 7) / 8)

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

// 8 种天气名，用于 --gallery 循环展示 —— 与 QWeather icon 代码一一对应
static const char *GALLERY_WEATHER[] = {
    "晴", "多云", "阴", "小雨", "大雪", "雷阵雨", "雾", "unknown-xxx",
};
static const int GALLERY_CODE[] = {
    100, 101, 104, 305, 402, 302, 501, 999,
};
#define GALLERY_N ((int)(sizeof(GALLERY_WEATHER)/sizeof(GALLERY_WEATHER[0])))
static int  s_gallery_idx = 0;
static bool s_gallery_mode = false;

// 让模拟器的 ui_model 数据像真机一样每秒更新
// 强制温湿度（--temp / --humi 命令行参数）：测试心情表情用
// 文件作用域，sim_tick_data 和 main 都能访问
static float s_force_temp = NAN;
static float s_force_humi = NAN;

static void sim_tick_data(void)
{
    ui_model_t *m = ui_model_get();
    uint64_t ovr = sim_console_override_mask();

    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    // 时间 6 个字段每帧都写 —— 必须让 override 生效，否则调试客户端设的
    // 时间下一帧就被真实时间冲掉（测时钟渲染/特定日期的日历时用得到）。
    if (!(ovr & OVR_TIME)) {
        m->hour = tm_local.tm_hour;
        m->minute = tm_local.tm_min;
        m->year = tm_local.tm_year + 1900;
        m->month = tm_local.tm_mon + 1;
        m->day = tm_local.tm_mday;
        m->weekday = tm_local.tm_wday;
    }

    static float phase = 0;
    phase += 0.02f;

    // 强制温湿度（--temp / --humi 命令行参数）：测试心情表情用
    // 同时设 override 位，避免被后面的默认值覆盖
    if (!isnan(s_force_temp)) {
        m->indoor_temp = s_force_temp;
        // 不在这里设 ovr，由调用方在 main() 里统一处理
    } else if (!(ovr & OVR_INDOOR_TEMP)) {
        m->indoor_temp = 24.0f + 1.5f * sinf(phase);
    }

    if (!isnan(s_force_humi)) {
        m->indoor_humi = s_force_humi;
    } else if (!(ovr & OVR_INDOOR_HUMI)) {
        m->indoor_humi = 55.0f + 4.0f * cosf(phase * 1.3f);
    }

    if (!(ovr & OVR_OUTDOOR_TEMP))
        m->outdoor_temp = 27.0f + 2.0f * sinf(phase * 0.4f);

    if (s_gallery_mode) {
        if (!(ovr & OVR_WEATHER_TEXT)) {
            strncpy(m->weather_text, GALLERY_WEATHER[s_gallery_idx],
                    sizeof(m->weather_text) - 1);
            m->weather_text[sizeof(m->weather_text) - 1] = 0;
        }
        if (!(ovr & OVR_WEATHER_CODE))
            m->weather_code = GALLERY_CODE[s_gallery_idx];
    } else {
        if (!(ovr & OVR_WEATHER_TEXT)) strcpy(m->weather_text, "多云");
        if (!(ovr & OVR_WEATHER_CODE)) m->weather_code = 101;
    }

    if (!(ovr & OVR_CITY)) strcpy(m->city, "北京");
    if (!(ovr & OVR_WEATHER_UPD))
        snprintf(m->weather_update, sizeof(m->weather_update), "%02d:%02d", m->hour, m->minute);

    // --- 天气详情页假数据 ---
    if (!(ovr & OVR_OUTDOOR_TEMP))
        m->outdoor_temp     = 27.0f + 2.0f * sinf(phase * 0.4f);
    if (!(ovr & OVR_OUTDOOR_HUMI))
        m->outdoor_humi     = 62.0f;
    if (!(ovr & OVR_FEELS_LIKE))
        m->feels_like_temp  = m->outdoor_temp + 1.5f;
    if (!(ovr & OVR_WIND_SPD))
        m->wind_speed_kmh   = 12.0f;
    if (!(ovr & OVR_WIND_DIR))
        strcpy(m->wind_dir, "东北风");
    if (!(ovr & OVR_CLOUD))
        m->cloud_pct        = 45;
    if (!(ovr & OVR_PRESSURE))
        m->pressure_hpa     = 1013;
    if (!(ovr & OVR_VISIBILITY))
        m->visibility_km    = 10;
    if (!(ovr & OVR_UV))
        m->uv_index         = 5;
    if (!(ovr & OVR_TEMP_MIN))
        m->temp_min         = 22;
    if (!(ovr & OVR_TEMP_MAX))
        m->temp_max         = 31;
    if (!(ovr & OVR_SUNRISE))  strcpy(m->sunrise, "06:12");
    if (!(ovr & OVR_SUNSET))   strcpy(m->sunset,  "18:45");

    if (!(ovr & OVR_WIFI_CONN)) m->wifi_connected  = false;
    if (!(ovr & OVR_WIFI_RSSI))  m->wifi_rssi       = 0;
    if (!(ovr & OVR_BAT_PCT))    m->battery_percent = 80;

    // --- 设备信息页假数据（真机由 net_bsp / esp_chip_info 填） -------
    // 这些字段不随时间变化，但每帧按 override 位重填 —— 这样 GUI 的
    // 「清除 override」能让它们恢复默认值（旧版用 static 只填一次，
    // clear 之后仍停在调试值上，看起来像 clear 失效）。
    if (!(ovr & OVR_CHIP_MODEL)) strcpy(m->chip_model, "ESP32-S3");
    if (!(ovr & OVR_CPU_CORES))  m->cpu_cores     = 2;
    if (!(ovr & OVR_FLASH_SIZE)) m->flash_size_mb = 8;
    if (!(ovr & OVR_FREE_HEAP))  m->free_heap_kb  = 215;
    if (!(ovr & OVR_IDF_VER))    strcpy(m->idf_ver, "v6.0.1");
    if (!(ovr & OVR_APP_VER))    strcpy(m->app_ver, "RLCD 0.1");
    if (!(ovr & OVR_SSID))       strcpy(m->ssid,    "MyPhone-2.4G");
    if (!(ovr & OVR_IP))         strcpy(m->ip,      "10.217.129.06");
    if (!(ovr & OVR_MAC))        strcpy(m->mac,     "84:F7:03:6C:AA:BB");

    // --- SD 卡假数据（真机由 sdcard_bsp / user_app 填） ---
    if (!(ovr & OVR_SD_MOUNTED)) m->sd_mounted  = true;
    if (!(ovr & OVR_SD_TOTAL))   m->sd_total_mb = 32 * 1024;        // 32 GB
    if (!(ovr & OVR_SD_USED))    m->sd_used_mb  = 3 * 1024 + 200;

    // --- Flash 用量假数据（真机由 user_app 扫描分区表填） ---
    if (!(ovr & OVR_FLASH_USED)) m->flash_used_kb = 4 * 1024;       // 4 MB 已用
    if (!(ovr & OVR_FLASH_FREE)) m->flash_free_kb = 12 * 1024;      // 12 MB 剩余

    // --- 配网页假数据 ---
    static uint32_t s_started = 0;
    if (!s_started) s_started = (uint32_t)time(NULL);
    if (!(ovr & OVR_UPTIME))
        m->uptime_sec = (uint32_t)time(NULL) - s_started;
    if (!(ovr & OVR_AP_SSID)) strcpy(m->ap_ssid, "RLCD-Setup");
    if (!(ovr & OVR_AP_IP))   strcpy(m->ap_ip,   "192.168.4.1");

    ui_pages_apply_locked();
}

// 后台线程读 stdin，把命令塞进线程安全队列（主循环 drain 时在主线程执行）。
// 不再直接调 LVGL —— 消除多线程崩溃。
// 为保持向后兼容，stdin 的旧语法（weather / temp）会被转成新协议。
static void *stdin_cmd_thread(void *arg)
{
    (void)arg;
    char line[256];
    fprintf(stderr, "[console] stdin commands (legacy syntax still supported):\n");
    fprintf(stderr, "  page N | next | prev\n");
    fprintf(stderr, "  weather <code> [text]  |  temp <T> [H]\n");
    fprintf(stderr, "  mark MM-DD | marks CSV | unmark | events SPEC | labels SPEC\n");
    fprintf(stderr, "  set <field> <value>  |  get <field>  |  clear  |  help  |  quit\n");
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;

        char buf[256];
        const char *out = buf;

        // 兼容旧语法：weather <code> [text] → set weather_code + set weather_text
        if (strncmp(line, "weather ", 8) == 0) {
            int code = atoi(line + 8);
            char code_str[16];
            snprintf(code_str, sizeof(code_str), "%d", code);
            snprintf(buf, sizeof(buf), "set weather_code %s", code_str);
            sim_console_submit(buf);
            char *sp = strchr(line + 8, ' ');
            if (sp && *(sp + 1)) {
                snprintf(buf, sizeof(buf), "set weather_text %s", sp + 1);
                out = buf;
            } else {
                continue;
            }
        }
        // 兼容旧语法：temp <T> [H] → set indoor_temp + set indoor_humi
        else if (strncmp(line, "temp ", 5) == 0) {
            float t = 0, h = 50;
            sscanf(line + 5, "%f %f", &t, &h);
            snprintf(buf, sizeof(buf), "set indoor_temp %.1f", t);
            sim_console_submit(buf);
            snprintf(buf, sizeof(buf), "set indoor_humi %.1f", h);
            out = buf;
        }
        // 兼容旧语法：clear → clear（清除 override）
        else if (strcmp(line, "clear") == 0) {
            out = "clear";
        }
        else {
            out = line;
        }

        sim_console_submit(out);
        if (strcmp(out, "quit") == 0) break;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    // --capture <path>          —— 无窗口，渲染几秒后存 PPM 退出
    // --gallery                 —— 循环切换 8 种天气图标（1.5s/张）
    // --gallery-capture <dir>   —— 8 张连拍到目录，退出
    // --page <id>               —— 启动时切到指定页（0=home, 1=device）
    // --pos <x>,<y>             —— 窗口左上角坐标（给 dev_sim.sh 排版用，默认居中）
    // --bot-frame <t> --bot-out <path.pbm>
    //                           —— 离线渲染 bloub 蒙太奇第 t 秒的 1-bit 帧
    //                              （纯 C 引擎，不经 LVGL/SDL），写 PBM P4 退出。
    //                              供 tools/bloub_golden.py 与 Node 真值比对。
    const char *capture_path = NULL;
    const char *gallery_dir = NULL;
    const char *bot_out = NULL;
    float bot_t = -1.0f;
    int capture_ms = 1500;
    int start_page = -1;
    int win_x = SDL_WINDOWPOS_CENTERED, win_y = SDL_WINDOWPOS_CENTERED;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            capture_path = argv[++i];
        } else if (!strcmp(argv[i], "--capture-ms") && i + 1 < argc) {
            capture_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            start_page = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bot-frame") && i + 1 < argc) {
            bot_t = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--bot-out") && i + 1 < argc) {
            bot_out = argv[++i];
        } else if (!strcmp(argv[i], "--pos") && i + 1 < argc) {
            int x, y;
            if (sscanf(argv[++i], "%d,%d", &x, &y) == 2) { win_x = x; win_y = y; }
            else fprintf(stderr, "--pos 格式应为 x,y（忽略）\n");
        } else if (!strcmp(argv[i], "--gallery")) {
            s_gallery_mode = true;
        } else if (!strcmp(argv[i], "--gallery-capture") && i + 1 < argc) {
            gallery_dir = argv[++i];
            s_gallery_mode = true;
        } else if (!strcmp(argv[i], "--temp") && i + 1 < argc) {
            s_force_temp = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--humi") && i + 1 < argc) {
            s_force_humi = atof(argv[++i]);
        }
    }

    // --bot-frame：纯引擎离线渲染，不进 LVGL/SDL（确定性，golden 比对用）
    if (bot_out && bot_t >= 0.0f) {
        bloub_engine_t *eng = bloub_engine_create();
        if (!eng) return 1;
        int idx = bloub_montage_block_at(bot_t, NULL);
        // 复刻 BloubBot.vue rendAt 的顺序推进：从第 1 块起逐块 setState 到其
        // 绝对偏移 —— 否则引擎没有历史链，淡入混合会从初始 idle 出发
        float off = 0.0f;
        for (int i = 0; i <= idx; i++) {
            if (i > 0) off += BLOUB_MONTAGE[i - 1].duration;
            bloub_engine_set_state(eng, BLOUB_MONTAGE[i].state, off);
        }
        bloub_scene_t sc;
        bloub_engine_sample(eng, bot_t, &sc);

        static uint8_t bits[BOT_OUT_STRIDE * BOT_OUT_SIZE];
        bloub_bitmap_t bm = { BOT_OUT_SIZE, BOT_OUT_SIZE, bits };
        bloub_render(&sc, &bm);
        bloub_engine_free(eng);

        FILE *fp = fopen(bot_out, "wb");
        if (!fp) { perror(bot_out); return 1; }
        fprintf(fp, "P4\n%d %d\n", BOT_OUT_SIZE, BOT_OUT_SIZE);
        fwrite(bits, 1, sizeof(bits), fp);
        fclose(fp);
        return 0;
    }

    // --temp / --humi 设了强制值：直接写模型 + 设 override 位
    // 此时还没进主循环，不能用队列，用 sim_console_set_field_override 直接写
    if (!isnan(s_force_temp)) sim_console_set_field_override("indoor_temp", s_force_temp);
    if (!isnan(s_force_humi)) sim_console_set_field_override("indoor_humi", s_force_humi);

    if (capture_path || gallery_dir) setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("RLCD 4.2\" simulator (400x300)",
                              win_x, win_y,
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
    // 字库：从 partitions/fonts/*.bin 加载（与真机同一批文件，保证像素一致）
    if (!UiFont_LoadFromDir(RLCD_FONTS_DIR)) {
        fprintf(stderr, "[sim] 字库加载失败，检查 %s 下是否有 ui_font_*.bin"
                        "（先跑 tools/gen_font.sh）\n", RLCD_FONTS_DIR);
    }
    // 日历页 demo 数据：标注、预定、随机标签
    ui_calendar_set_marks(  "01-01,10-01,12-25");
    ui_calendar_set_events( "01-01=元旦快乐;02-14=情人节;10-01=国庆节");
    ui_calendar_set_labels( "今天也要加油;好好吃饭;早点睡;保持微笑;多喝热水");
    sim_tick_data();
    ui_pages_create();
    ui_pages_apply_locked();
    if (start_page >= 0 && start_page < (int)UI_PAGE_COUNT) {
        ui_pages_switch_to_locked((ui_page_id_t)start_page);
    }

    // 启动 TCP 调试服务器（默认端口 9000，失败不退出）
    sim_console_init(0);

    // 后台线程读 stdin，把命令塞进队列（不在无窗口模式下启动）
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

        // 在主线程执行队列里的调试命令（来自 stdin / TCP 客户端）
        sim_console_drain();

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

    sim_console_shutdown();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
