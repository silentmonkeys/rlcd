idf_component_register(
    SRCS "i2c_bsp.c" "display_bsp.cpp"
         "button_bsp.c"
         "sdcard_bsp.c"
         "adc_bsp.c"
         "audio_bsp.c"
         "multi_button/multi_button.c"
    INCLUDE_DIRS "./" "multi_button"
    # audio_bsp.c 引 user_config.h（音频引脚）——只借 include 路径不 REQUIRES main，
    # 避免 main → port_bsp（main 用 display_bsp）与 port_bsp → net_bsp 的环。
    PRIV_INCLUDE_DIRS "../../main"
    # display_bsp.h 里直接 #include <driver/gpio.h>、<driver/spi_master.h>、
    # <esp_lcd_*.h>，属于头文件公开依赖，必须写成 REQUIRES 才能传播给
    # main / net_bsp 等消费者。
    #
    # 在 ESP-IDF v6 里：
    #   driver/gpio.h        <- esp_driver_gpio 组件
    #   driver/spi_master.h  <- esp_driver_spi  组件
    #   esp_lcd_*.h          <- esp_lcd 组件
    #   driver/i2c_master.h  <- esp_driver_i2c 组件
    #   esp_vfs_fat.h / sdmmc_host.h <- fatfs / sdmmc / esp_driver_sdmmc
    REQUIRES esp_driver_gpio esp_driver_spi esp_lcd esp_driver_i2c
             esp_driver_sdmmc sdmmc fatfs vfs esp_adc
    # i2s_std.h（I2S std 通道）来自 esp_driver_i2s；esp_codec_dev 提供 codec 驱动
    PRIV_REQUIRES esp_timer ui net_bsp esp_driver_i2s espressif__esp_codec_dev)
