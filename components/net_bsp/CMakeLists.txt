idf_component_register(
    SRCS "src/net_bsp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES ui app_bsp main
        esp_wifi esp_event esp_http_server esp_http_client esp_netif lwip nvs_flash
        esp-tls mbedtls
        espressif__zlib)
