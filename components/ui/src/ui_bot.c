// ui_bot —— 机器人页：播放 bloub 预渲染 1-bit 动画（BLO1 帧序列）
//
// 布局：scaffold（标题"机器人" + 状态栏 + 底线页码点）+ 居中 160×160 位图 +
// 底部 bloub 出处小字。播放靠 lv_timer（周期 = 1000/fps）：仅当本页是当前页时
// 才读帧，读完后 set_src + 失效；切到别的页 timer 仍在但空转，代价一次比较。
//
// 动画数据缺失（fonts 分区没烧 bloub_seq.bin）时显示占位提示，不影响其余页面。

#include "ui_bot.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_pages.h"
#include "ui_font.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

// 与 ui_setup.c 一致的页面尺寸（SCR_W/H 不由 ui_common.h 导出）
#define SCR_W   400
#define SCR_H   300

// scaffold 占位：标题/状态栏到 ~50，底线 278 —— 动画区 52..276
#define BOT_AREA_TOP    52
#define BOT_AREA_BOTTOM 276

// ── 动画文件 ──────────────────────────────────────────────────────
#define BLOB_FILE   "bloub_seq.bin"
#define BLOB_LETTER "A"          // 与 ui_font.c / ui_weather_icon.c 的盘符一致
#define BLOB_HDR_SIZE 16

// 帧缓冲上限：当前 160×160（3200B），留到 200×200 余量
#define BLOB_MAX_STRIDE 25
#define BLOB_MAX_H      200
#define BLOB_MAX_BITS   (BLOB_MAX_STRIDE * BLOB_MAX_H)

// 页面私有状态
static lv_obj_t *s_screen = NULL;
static lv_obj_t *bot_img = NULL;      // 动画位图
static lv_obj_t *lbl_placeholder = NULL; // 数据缺失时的占位提示
static ui_status_bar_t *s_bar;

// BLO1 头部缓存
static bool     s_seq_ok = false;
static uint16_t s_w, s_h, s_fps, s_count;
static uint32_t s_frame_bytes;

// I1 图描述：s_buf = palette(8B) + bits；位图数据原地更新
static uint8_t         s_buf[8 + BLOB_MAX_BITS];
static lv_image_dsc_t  s_dsc;
static lv_fs_file_t    s_file;
static uint16_t        s_frame = 0;

#ifdef ESP_PLATFORM
static const char * base_dir(void) { return "/spiffs"; }
#else
static const char * base_dir(void) { return RLCD_FONTS_DIR; }   // 模拟器 = partitions/fonts
#endif

// 打开动画文件并读头部。成功后句柄常驻，播放时只 seek+read。
static bool seq_open(void)
{
    if (s_seq_ok) return true;

    char path[128];
    snprintf(path, sizeof(path), BLOB_LETTER ":%s/" BLOB_FILE, base_dir());
    if (lv_fs_open(&s_file, path, LV_FS_MODE_RD) != LV_FS_RES_OK) return false;

    uint8_t hdr[BLOB_HDR_SIZE];
    uint32_t got = 0;
    if (lv_fs_read(&s_file, hdr, BLOB_HDR_SIZE, &got) != LV_FS_RES_OK || got != BLOB_HDR_SIZE)
        goto fail;
    if (memcmp(hdr, "BLO1", 4) != 0) goto fail;

    s_w     = (uint16_t)(hdr[6]  | (hdr[7]  << 8));
    s_h     = (uint16_t)(hdr[8]  | (hdr[9]  << 8));
    s_fps   = (uint16_t)(hdr[10] | (hdr[11] << 8));
    s_count = (uint16_t)(hdr[12] | (hdr[13] << 8));

    uint32_t stride = ((uint32_t)s_w + 7) / 8;
    s_frame_bytes = stride * (uint32_t)s_h;
    if (s_count == 0 || s_fps == 0 || stride > BLOB_MAX_STRIDE || s_h > BLOB_MAX_H ||
        8 + s_frame_bytes > sizeof(s_buf))
        goto fail;

    // 调色板：0 = 白底，1 = 黑墨。lv_color32_t 布局是 R,G,B,A —— 黑的 alpha
    // 必须是 0xFF，写 0 会整块透明（与 ui_weather_icon.c 的 s_palette 一致）
    s_buf[0] = 0xFF; s_buf[1] = 0xFF; s_buf[2] = 0xFF; s_buf[3] = 0xFF;
    s_buf[4] = 0x00; s_buf[5] = 0x00; s_buf[6] = 0x00; s_buf[7] = 0xFF;

    s_dsc.header.magic      = 0x19;
    s_dsc.header.cf         = LV_COLOR_FORMAT_I1;
    s_dsc.header.flags      = 0;
    s_dsc.header.w          = s_w;
    s_dsc.header.h          = s_h;
    s_dsc.header.stride     = stride;
    s_dsc.header.reserved_2 = 0;
    s_dsc.data_size         = 8 + s_frame_bytes;
    s_dsc.data              = s_buf;
    s_dsc.reserved          = NULL;

    s_seq_ok = true;
    return true;

fail:
    lv_fs_close(&s_file);
    s_seq_ok = false;
    return false;
}

// 读第 no 帧进 s_buf 并刷新位图。返回 false = 读失败（保持上一帧）。
static bool show_frame(uint16_t no)
{
    uint32_t off = BLOB_HDR_SIZE + (uint32_t)no * s_frame_bytes;
    uint32_t got = 0;
    if (lv_fs_seek(&s_file, off, LV_FS_SEEK_SET) != LV_FS_RES_OK) return false;
    if (lv_fs_read(&s_file, s_buf + 8, s_frame_bytes, &got) != LV_FS_RES_OK ||
        got != s_frame_bytes)
        return false;
    lv_img_set_src(bot_img, &s_dsc);
    return true;
}

// 播放节拍：只在本页可见时读帧。LVGL timer 回调本身就在 lv_timer_handler
// （持锁）上下文里跑，不能再拿 Lvgl_lock。
static void play_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_seq_ok || !bot_img) return;
    if (ui_pages_current() != UI_PAGE_BOT) return;

    if (!show_frame(s_frame)) return;
    s_frame = (uint16_t)((s_frame + 1) % s_count);
}

lv_obj_t *ui_bot_create(void)
{
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    const ui_model_t *m = ui_model_get();
    s_bar = ui_page_create_scaffold(s_screen, "机器人", UI_PAGE_BOT,
                                    m->wifi_rssi, m->wifi_connected,
                                    m->battery_percent);

    if (seq_open()) {
        // 动画区垂直居中
        bot_img = lv_img_create(s_screen);
        lv_obj_set_size(bot_img, s_w, s_h);
        lv_obj_set_pos(bot_img, (SCR_W - (int)s_w) / 2,
                       BOT_AREA_TOP + (BOT_AREA_BOTTOM - BOT_AREA_TOP - (int)s_h) / 2);
        lv_obj_clear_flag(bot_img, LV_OBJ_FLAG_SCROLLABLE);

        // 首帧立即上屏，避免第一拍之前空白
        s_frame = 0;
        show_frame(0);

        lv_timer_create(play_timer_cb, 1000 / s_fps, NULL);
    } else {
        lbl_placeholder = ui_make_label(s_screen, ui_font_cjk_16(), 0, 130,
                                        "动画数据未安装");
        lv_obj_set_width(lbl_placeholder, SCR_W);
        lv_obj_set_style_text_align(lbl_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    }

    // 出处小字（底线 278 之上）
    lv_obj_t *credit = ui_make_label(s_screen, ui_font_mood_16(), 0, 254, "bloub (MIT)");
    lv_obj_set_width(credit, SCR_W);
    lv_obj_set_style_text_align(credit, LV_TEXT_ALIGN_CENTER, 0);

    ui_bot_apply_locked();
    return s_screen;
}

void ui_bot_apply_locked(void)
{
    if (!s_screen) return;
    const ui_model_t *m = ui_model_get();
    ui_status_bar_update(s_bar, m->wifi_rssi, m->wifi_connected, m->battery_percent);
}
