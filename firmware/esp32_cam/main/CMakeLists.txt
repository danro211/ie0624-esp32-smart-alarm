idf_component_register(
    SRCS "esp32_cam.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp32-camera
        esp_driver_ledc
)
