idf_component_register(
    SRCS "esp32_principal.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        lwip
)
