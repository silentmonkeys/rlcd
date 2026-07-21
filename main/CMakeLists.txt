idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "./"
    PRIV_REQUIRES port_bsp app_bsp ui net_bsp user_app nvs_flash spiffs)

# 把 partitions/fonts/ 目录打包成 SPIFFS 镜像，烧到 partitions.csv 里名叫
# "fonts" 的分区。FLASH_IN_PROJECT → 跟随 idf.py flash 一起烧。
# 单独更新字库：idf.py -p /dev/ttyUSB0 partition-table && idf.py flash-fonts。
spiffs_create_partition_image(fonts
    ${CMAKE_SOURCE_DIR}/partitions/fonts
    FLASH_IN_PROJECT)
