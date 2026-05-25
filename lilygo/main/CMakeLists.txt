idf_component_register(
    SRCS
        "app_main.c"
        "shim_callback.c"
    INCLUDE_DIRS "."
    REQUIRES
        nethack
        esp_psram
        # esp_rom supplies __wrap_longjmp (xtensa patches/esp_rom_longjmp.S),
        # which the ESP-IDF link injects via `-Wl,--wrap=longjmp` -- libnh.a
        # and lua548.a both call longjmp.
        esp_rom
        # spiffs provides esp_vfs_spiffs_register for mounting the nhdat
        # partition at /littlefs/nethack.
        spiffs
        # esp_partition for partition enumeration diagnostics
        esp_partition
        # tdeck_display drives the ST7789 over SPI
        tdeck_display
        # tdeck_keyboard polls the BlackBerry-style QWERTY over I2C
        tdeck_keyboard
        # tdeck_sdcard mounts the microSD over the shared LCD SPI bus
        tdeck_sdcard
        # esp_pm provides esp_pm_configure() for DFS + light sleep
        esp_pm
)
