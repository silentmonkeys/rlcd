idf_component_register(
    SRCS "src/net_bsp.c"
         "src/net_wifi.c"
         "src/net_portal.c"
         "src/net_weather.c"
         "src/net_calendar.c"
    INCLUDE_DIRS "include"
    # user_config.h 目前放在 main/，通过 PRIV_INCLUDE_DIRS 直接引用而不 REQUIRES main
    # ——避免和 port_bsp（后者 PRIV_REQUIRES net_bsp 供长按刷天气用）形成
    # port_bsp → net_bsp → main → port_bsp 的循环依赖。
    PRIV_INCLUDE_DIRS "src" "../../main"
    PRIV_REQUIRES ui app_bsp
        esp_wifi esp_event esp_http_server esp_http_client esp_netif lwip nvs_flash
        esp-tls mbedtls esp_timer
        espressif__zlib)
