idf_component_register(
    SRCS "src/net_bsp.c"
         "src/net_wifi.c"
         "src/net_portal.c"
         "src/net_http.c"
         "src/net_weather.c"
         "src/net_uapi.c"
         "src/net_calendar.c"
         "src/net_ota.c"
         "src/net_apistat.c"
         "src/net_xiaozhi.c"
    INCLUDE_DIRS "include"
    # api.tenclass.net 的证书链（GeoTrust G2 TLS CN RSA4096 SHA256 2022 CA1 →
    # DigiCert Global Root G2）在默认证书包里按签发者名匹配不上，xiaozhi 的
    # HTTP/WS 连接改用固定根证书（见 net_xiaozhi.c 的 cert_pem）
    EMBED_TXTFILES "certs/digicert_global_root_g2.pem"
    # user_config.h 目前放在 main/，通过 PRIV_INCLUDE_DIRS 直接引用而不 REQUIRES main
    # ——避免和 port_bsp（后者 PRIV_REQUIRES net_bsp 供长按刷天气用）形成
    # port_bsp → net_bsp → main → port_bsp 的循环依赖。
    PRIV_INCLUDE_DIRS "src" "../../main"
    PRIV_REQUIRES ui app_bsp
        esp_wifi esp_event esp_http_server esp_http_client esp_netif lwip nvs_flash
        esp-tls mbedtls esp_timer cjson esp_ringbuf
        app_update esp_partition esp_app_format
        espressif__zlib esp_websocket_client espressif__cjson
        espressif__esp_audio_codec)
