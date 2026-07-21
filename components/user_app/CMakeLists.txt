idf_component_register(
    SRCS "user_app.c"
    INCLUDE_DIRS "./"
    PRIV_REQUIRES ui app_bsp main port_bsp net_bsp
        spi_flash esp_hw_support esp_app_format app_update esp_timer esp_partition
    REQUIRES lvgl__lvgl)
