idf_component_register(SRCS "espnow_rx.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_driver_i2s esp_driver_gpio esp_ringbuf
                    INCLUDE_DIRS ".")
