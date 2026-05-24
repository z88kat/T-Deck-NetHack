idf_component_register(
    SRCS
        "tdeck_sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        sdmmc
        fatfs
        esp_driver_spi
        esp_driver_sdspi
)
