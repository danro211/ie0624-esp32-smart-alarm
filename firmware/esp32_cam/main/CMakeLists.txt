idf_component_register(
    SRCS "esp32_cam.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32-camera
        esp_driver_ledc
)
