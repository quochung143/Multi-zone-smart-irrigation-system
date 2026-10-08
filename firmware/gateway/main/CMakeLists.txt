idf_component_register(SRCS "main.c" "uplink.c" "cmd.c"
                       INCLUDE_DIRS "."
                       REQUIRES common esp_wifi esp_netif esp_event nvs_flash esp_driver_uart
                                esp_driver_gpio json esp_timer vfs)
