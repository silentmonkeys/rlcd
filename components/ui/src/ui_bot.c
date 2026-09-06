// ui_bot —— 机器人页（第二页）：bloub C 引擎实时渲染 + xiaozhi 对话文本
//
// 布局（无标题、无底部出处 —— 顶部只有状态栏，底部是文本区）：
//   - 状态栏（WiFi/电池）
//   - 居中 200×200 位图：bloub 引擎逐帧渲染的 1-bit 动画
//   - 底部：对话文本行（走马灯）—— AI 回答优先，没有回答时显示用户问题
//   - 底线 + 页码点
//
// 渲染：bloub 引擎（components/ui/src/bloub_*.c）是纯时间函数，lv_timer
// 80ms 一拍推进引擎时钟，sample → 1-bit 光栅化 → I1 调色板位图刷进 lv_img。
// 机器人姿态由 ui_model 的 xiaozhi 状态/情绪驱动（bot_state / bot_emotion）。
// 动画只在本页可见时推进（timer 空转一次比较的代价）。

#include "ui_bot.h"
#include "ui_common.h"
#include "ui_model.h"
#include "ui_pages.h"
#include "ui_font.h"
#include "bloub_bot.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

// 与 ui_setup.c 一致的页面尺寸（SCR_W/H 不由 ui_common.h 导出）
#define SCR_W   400
#define SCR_H   300

// 布局：状态栏 ~0..24，对话行 240，底线 278，页码点 291
#define BOT_BM_SIZE   200
#define BOT_BM_Y      28
#define CHAT_Y        240

// I1 位图缓冲：palette(8B) + bits（stride×h）
#define BOT_STRIDE ((BOT_BM_SIZE + 7) / 8)
static uint8_t s_buf[8 + BOT_STRIDE * BOT_BM_SIZE];
static lv_image_dsc_t s_dsc;
static bloub_bitmap_t s_bm;
static bloub_scene_t  s_scene;

// 页面私有状态
static lv_obj_t *s_screen = NULL;
static lv_obj_t *bot_img = NULL;
static lv_obj_t *lbl_chat = NULL;
static ui_status_bar_t *s_bar;

// 引擎 + 时钟
static bloub_engine_t *s_engine = NULL;
static float s_clock = 0.0f;
static bloub_state_t s_engine_state = BLOUB_IDLE;

// xiaozhi 状态/情绪 → bloub 姿态
static bloub_state_t state_to_bloub(void)
{
    const ui_model_t *m = ui_model_get();
    int8_t emo = m->bot_emotion;
    bloub_state_t emo_state = BLOUB_IDLE;
    switch (emo) {
        case UI_BOT_EMO_HAPPY:
        case UI_BOT_EMO_LOVING:
        case UI_BOT_EMO_COOL:      emo_state = BLOUB_WINK;     break;
        case UI_BOT_EMO_SAD:
        case UI_BOT_EMO_SLEEPY:    emo_state = BLOUB_SLEEP;    break;
        case UI_BOT_EMO_ANGRY:
        case UI_BOT_EMO_SURPRISED:
        case UI_BOT_EMO_CONFUSED:  emo_state = BLOUB_WIDE;     break;
        case UI_BOT_EMO_THINKING:  emo_state = BLOUB_THINKING; break;
        default:                   emo_state = BLOUB_IDLE;     break;
    }

    switch (m->bot_state) {
        case UI_BOT_ST_LISTENING:   return BLOUB_WIDE;
        case UI_BOT_ST_THINKING:    return BLOUB_THINKING;
        case UI_BOT_ST_CONNECTING:
        case UI_BOT_ST_ACTIVATING:  return BLOUB_THINKING;
        case UI_BOT_ST_SPEAKING:
            // 播报中跟随 LLM 情绪，没给情绪就当开心说
            return (emo != UI_BOT_EMO_NEUTRAL) ? emo_state : BLOUB_WINK;
        default:                    return emo_state;   // idle/offline/error 跟情绪
    }
}

static void render_frame(void)
{
    bloub_engine_sample(s_engine, s_clock, &s_scene);
    bloub_render(&s_scene, &s_bm);
    lv_img_set_src(bot_img, &s_dsc);
}

// 播放节拍：只在本页可见时推进（LVGL timer 回调已在 lv_timer_handler 持锁
// 上下文里跑，不能再拿 Lvgl_lock）
static void play_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_engine || !bot_img) return;
    if (ui_pages_current() != UI_PAGE_BOT) return;

    s_clock += 0.08f;   // 12.5fps：反射屏全屏刷新下的稳妥节奏

    bloub_state_t want = state_to_bloub();
    if (want != s_engine_state) {
        bloub_engine_set_state(s_engine, want, s_clock);
        s_engine_state = want;
    }
    render_frame();
}

lv_obj_t *ui_bot_create(void)
{
    if (s_screen) return s_screen;

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    const ui_model_t *m = ui_model_get();
    s_bar = ui_status_bar_create(s_screen, m->wifi_rssi, m->wifi_connected,
                                 m->battery_percent);

    // 底线 + 页码点（不用 scaffold：本页没有标题）
    ui_pixel_rect(s_screen, 9, 278, 378, 2);
    ui_draw_page_dots(s_screen, UI_PAGE_BOT, 291, 3, 14);

    // 机器人位图：I1 调色板（0=白底 1=黑墨，与 ui_weather_icon.c 一致；
    // 注意 lv_color32_t 是 R,G,B,A 布局 —— 黑的 alpha 必须是 0xFF）
    s_buf[0] = 0xFF; s_buf[1] = 0xFF; s_buf[2] = 0xFF; s_buf[3] = 0xFF;
    s_buf[4] = 0x00; s_buf[5] = 0x00; s_buf[6] = 0x00; s_buf[7] = 0xFF;
    s_dsc.header.magic      = 0x19;
    s_dsc.header.cf         = LV_COLOR_FORMAT_I1;
    s_dsc.header.flags      = 0;
    s_dsc.header.w          = BOT_BM_SIZE;
    s_dsc.header.h          = BOT_BM_SIZE;
    s_dsc.header.stride     = BOT_STRIDE;
    s_dsc.header.reserved_2 = 0;
    s_dsc.data_size         = 8 + BOT_STRIDE * BOT_BM_SIZE;
    s_dsc.data              = s_buf;
    s_dsc.reserved          = NULL;

    s_bm.w = BOT_BM_SIZE;
    s_bm.h = BOT_BM_SIZE;
    s_bm.bits = s_buf + 8;

    s_engine = bloub_engine_create();
    s_engine_state = BLOUB_IDLE;

    bot_img = lv_img_create(s_screen);
    lv_obj_set_size(bot_img, BOT_BM_SIZE, BOT_BM_SIZE);
    lv_obj_set_pos(bot_img, (SCR_W - BOT_BM_SIZE) / 2, BOT_BM_Y);
    lv_obj_clear_flag(bot_img, LV_OBJ_FLAG_SCROLLABLE);

    // 首帧立即上屏，避免第一拍之前空白
    render_frame();
    lv_timer_create(play_timer_cb, 80, NULL);

    // 底部对话文本：AI 回答优先，没有回答时显示用户问题
    lbl_chat = ui_make_label(s_screen, ui_font_cjk_16(), 0, CHAT_Y, "");
    lv_obj_set_width(lbl_chat, SCR_W);
    lv_obj_set_style_text_align(lbl_chat, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_marquee(lbl_chat, 30);

    ui_bot_apply_locked();
    return s_screen;
}

void ui_bot_apply_locked(void)
{
    if (!s_screen) return;
    const ui_model_t *m = ui_model_get();

    // 状态栏动态刷新
    ui_status_bar_update(s_bar, m->wifi_rssi, m->wifi_connected,
                         m->battery_percent);

    // 对话文本：激活提示 > AI 回答 > 用户问题
    const char *text = "";
    if (m->bot_state == UI_BOT_ST_ACTIVATING && m->bot_xz_code[0]) {
        static char code_line[72];
        snprintf(code_line, sizeof(code_line),
                 "激活码 %s —— 到 xiaozhi.me 控制台输入绑定",
                 m->bot_xz_code);
        text = code_line;
    } else if (m->bot_chat_reply[0]) text = m->bot_chat_reply;
    else if (m->bot_chat_user[0]) text = m->bot_chat_user;
    ui_label_set_text_if_changed(lbl_chat, text);
}
