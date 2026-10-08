idf_component_register(SRCS "main.c" "settings.c" "sht30.c" "sensor.c" "pump.c" "comm.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES common esp_adc esp_driver_i2c esp_driver_gpio esp_wifi esp_netif
                                esp_event nvs_flash console esp_timer)
