idf_component_register(SRCS "main.c" "uplink.c" "cmd.c" "lcd.c" "display.c"
                       INCLUDE_DIRS "."
                       REQUIRES common esp_wifi esp_netif esp_event nvs_flash esp_driver_uart
                                esp_driver_gpio esp_driver_spi esp_lcd json esp_timer vfs)
