idf_component_register(SRCS "main.c" "uplink.c" "cmd.c" "net.c" "console.c" "lcd.c" "display.c"
                       INCLUDE_DIRS "."
                       REQUIRES common esp_wifi esp_netif esp_event nvs_flash mqtt console
                                esp_driver_gpio esp_driver_spi esp_lcd json esp_timer)
