idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "./"
    PRIV_REQUIRES port_bsp app_bsp ui net_bsp user_app nvs_flash)
