// ui_font —— 3 份由 tools/gen_font.sh 生成的字模的入口
#include "lvgl.h"
#include "ui_font.h"

LV_FONT_DECLARE(ui_font_cjk_16_gen);
LV_FONT_DECLARE(ui_font_digit_big_gen);
LV_FONT_DECLARE(ui_font_digit_mid_gen);

const lv_font_t *ui_font_cjk_16(void)    { return &ui_font_cjk_16_gen;    }
const lv_font_t *ui_font_digit_big(void) { return &ui_font_digit_big_gen; }
const lv_font_t *ui_font_digit_mid(void) { return &ui_font_digit_mid_gen; }
