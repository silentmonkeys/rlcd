idf_component_register(
    SRCS       "src/ui_common.c"
               "src/ui_home.c"
               "src/ui_weather.c"
               "src/ui_calendar.c"
               "src/ui_device.c"
               "src/ui_bot.c"
               "src/bloub/bloub_math.c"
               "src/bloub/bloub_face.c"
               "src/bloub/bloub_states.c"
               "src/bloub/bloub_engine.c"
               "src/bloub/bloub_render.c"
               "src/ui_setup.c"
               "src/ui_pages.c"
               "src/lcd_clock.c"
               "src/ui_font.c"
               "src/ui_weather_icon.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_bsp spiffs
    REQUIRES   lvgl__lvgl)

# 字模已改为运行时从 fonts SPIFFS 分区加载（lv_binfont_create），不再编译内嵌。
# 生成/更新字库见 tools/gen_font.sh。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
