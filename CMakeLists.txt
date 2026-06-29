idf_component_register(
    SRCS
        "src/smonitor_modem.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_event esp_netif espressif__esp_modem
)
