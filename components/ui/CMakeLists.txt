idf_component_register(
    SRCS       "src/ui_common.c"
               "src/ui_home.c"
               "src/ui_weather.c"
               "src/ui_calendar.c"
               "src/ui_device.c"
               "src/ui_setup.c"
               "src/ui_pages.c"
               "src/lcd_clock.c"
               "src/ui_font.c"
               "src/ui_font_cjk_16_gen.c"
               "src/ui_font_digit_big_gen.c"
               "src/ui_font_digit_mid_gen.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_bsp
    REQUIRES   lvgl__lvgl)

# lv_font_conv 生成的字体文件里有：
#   #ifdef LV_LVGL_H_INCLUDE_SIMPLE
#   #include "lvgl.h"
#   #else
#   #include "lvgl/lvgl.h"       <-- 在 ESP-IDF 里这个路径不存在
#   #endif
# 让整份 ui 组件都走 <lvgl.h> 那条，跟 simulator 侧一致。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
