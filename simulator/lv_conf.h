/**
 * lv_conf.h —— 模拟器专用的 LVGL 配置
 * 尽量贴近目标板：RGB565 buffer + 常用字体 + 中文字体。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH        16      // RGB565，与真机 flush 一致
#define LV_COLOR_16_SWAP      0

#define LV_USE_LOG            1
#define LV_LOG_PRINTF         1
#define LV_LOG_LEVEL          LV_LOG_LEVEL_WARN

#define LV_MEM_SIZE           (512U * 1024U)

// —— 字体 —— 主界面用到的都要打开
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

// 中文字体走 components/ui/src/ui_font_cjk_16_gen.c（由 tools/gen_font.sh
// 从系统 TTF 裁剪生成，只含项目实际用到的字符），因此不需要 LVGL 自带的
// simsun_16_cjk。
#define LV_FONT_SIMSUN_16_CJK 0

#define LV_USE_ANIMIMG        0
#define LV_USE_CANVAS         0
#define LV_USE_QRCODE         0
#define LV_USE_SYSMON         0
#define LV_USE_PERF_MONITOR   0
#define LV_USE_MEM_MONITOR    0

#define LV_USE_FONT_COMPRESSED  0

// tick 通过我们在 main.c 里调用 lv_tick_inc 手工推进
#define LV_TICK_CUSTOM        0

// 让内建 draw 走软件 rasterizer
#define LV_DRAW_SW_COMPLEX    1

#endif /* LV_CONF_H */
