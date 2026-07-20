idf_component_register(
    SRCS "i2c_bsp.c" "display_bsp.cpp"
         "button_bsp.c"
         "multi_button/multi_button.c"
    INCLUDE_DIRS "./" "multi_button"
    # display_bsp.h 里直接 #include <driver/gpio.h>、<driver/spi_master.h>、
    # <esp_lcd_*.h>，属于头文件公开依赖，必须写成 REQUIRES 才能传播给
    # main / net_bsp 等消费者。
    #
    # 在 ESP-IDF v6 里：
    #   driver/gpio.h        <- esp_driver_gpio 组件
    #   driver/spi_master.h  <- esp_driver_spi  组件
    #   esp_lcd_*.h          <- esp_lcd 组件
    #   driver/i2c_master.h  <- esp_driver_i2c 组件
    REQUIRES esp_driver_gpio esp_driver_spi esp_lcd esp_driver_i2c
    PRIV_REQUIRES esp_timer ui)
