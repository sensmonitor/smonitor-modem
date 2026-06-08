idf_component_register(
    SRCS
        "src/smonitor_modem.c"
        "src/boards/lilygo_t_sim7000g.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES driver esp_event esp_netif
)
