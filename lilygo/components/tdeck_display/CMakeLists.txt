idf_component_register(
    SRCS
        "tdeck_display.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_lcd
        esp_driver_ledc
)
