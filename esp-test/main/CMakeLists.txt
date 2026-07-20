idf_component_register(SRCS "dns_server.c" "main.c" "oled.c" "anim.c" "ble_scan.c" "ble_display.c"
                            "scan_store.c" "sensor_packet.c" "nvs_config.c"
                            "wifi.c" "uploader.c" "provision.c"
                       PRIV_REQUIRES bt nvs_flash esp_coex esp_timer esp_wifi esp_netif esp_event
                                    esp_http_client esp_http_server mbedtls lwip
                                    esp_driver_i2c esp_driver_gpio
                       INCLUDE_DIRS ".")
