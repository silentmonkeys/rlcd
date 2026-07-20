// lcd_clock —— 主界面 HH:MM 大时钟
//
// 用 lv_font_conv 从 DejaVuSans-Bold 96px 裁的字体渲染，纯 <0x7fff → 黑
// 二值化下边缘依然清晰。
//
// 布局：一个 HH label + 一个冒号 label + 一个 MM label，三段固定位置。
// 冒号闪烁 = 隐藏/显示冒号 label —— 不去改任何字符串，数字位置永远不动，
// 也不会因为字体里没装空格字模而在冒号消失时出现"未知字符方块"。
#include "lcd_clock.h"
#include "ui_font.h"

#include <stdio.h>
#include <string.h>

// 96px DejaVuSans-Bold 每个数字实际宽度约 55~65px（"1" 窄、"8" 宽），
// 一对最宽的 "88" 约 130px；宽度留 140px 余量，再配合 LV_LABEL_LONG_CLIP
// 保证不会换行。冒号 label 宽度按 ":" 字模宽度算，34px。
#define DIGIT_PAIR_W   140
#define COLON_W        34
#define CLOCK_W        (DIGIT_PAIR_W + COLON_W + DIGIT_PAIR_W)  // 314
#define CLOCK_H        110

typedef struct {
    lv_obj_t *hh;
    lv_obj_t *mm;
    lv_obj_t *colon;
    int8_t    last_hh, last_mm;
} lcd_clock_t;

static lv_obj_t *make_digit_label(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_font_digit_big(), 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_style_text_letter_space(l, -2, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    // 关键：默认 LONG_WRAP 会让 "88" 这种宽数字在 96px 下超过 label 宽被折成
    // 两行；CLIP 保证永远单行、超宽也不换行（我们已经算好总宽 314 是够的）。
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

lv_obj_t *lcd_clock_create(lv_obj_t *parent, int x, int y)
{
    lcd_clock_t *w = (lcd_clock_t *) lv_malloc(sizeof(lcd_clock_t));
    memset(w, 0, sizeof(*w));
    w->last_hh = w->last_mm = -1;

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, CLOCK_W, CLOCK_H);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    // HH:  x=0..DIGIT_PAIR_W
    w->hh    = make_digit_label(c, 0, 0, DIGIT_PAIR_W);
    lv_label_set_text(w->hh, "00");
    // colon: 紧接在 HH 右边
    w->colon = make_digit_label(c, DIGIT_PAIR_W, 0, COLON_W);
    lv_label_set_text(w->colon, ":");
    // MM: colon 之后
    w->mm    = make_digit_label(c, DIGIT_PAIR_W + COLON_W, 0, DIGIT_PAIR_W);
    lv_label_set_text(w->mm, "00");

    lv_obj_set_user_data(c, w);
    return c;
}

int lcd_clock_width(void)  { return CLOCK_W; }
int lcd_clock_height(void) { return CLOCK_H; }

void lcd_clock_set_time(lv_obj_t *widget, int hh, int mm, bool colon_on)
{
    lcd_clock_t *w = (lcd_clock_t *) lv_obj_get_user_data(widget);
    if (!w) return;
    if (hh < 0) hh = 0;
    if (hh > 99) hh = 99;
    if (mm < 0) mm = 0;
    if (mm > 99) mm = 99;

    char buf[4];
    if (w->last_hh != hh) {
        w->last_hh = hh;
        snprintf(buf, sizeof(buf), "%02d", hh);
        lv_label_set_text(w->hh, buf);
    }
    if (w->last_mm != mm) {
        w->last_mm = mm;
        snprintf(buf, sizeof(buf), "%02d", mm);
        lv_label_set_text(w->mm, buf);
    }
    if (colon_on) lv_obj_clear_flag(w->colon, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag  (w->colon, LV_OBJ_FLAG_HIDDEN);
}
