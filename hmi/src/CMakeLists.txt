idf_component_register(
    SRCS "main.c" "board.c" "plat_esp.c" "net.c" "sdlog.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_wifi esp_event esp_netif esp_timer nvs_flash
             esp_http_server app_update esp_rom
             fatfs sdmmc esp_driver_sdspi esp_driver_spi)
