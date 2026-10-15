idf_component_register(SRCS "main.c" "leds.c" "pd.c" "sensors.c" "mic.c" "fft.c" "ble.c" "net.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_i2s
                                bt nvs_flash esp_timer esp_wifi esp_netif esp_event mbedtls)
