// Gateway ESP-NOW <-> Serial (docs/DESIGN.md §4.3)
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "nvs_flash.h"

#define SERVER_UART UART_NUM_0

static SemaphoreHandle_t serial_mtx;

void serial_out(const char *line)
{
    xSemaphoreTake(serial_mtx, portMAX_DELAY);
    uart_write_bytes(SERVER_UART, line, strlen(line));
    uart_write_bytes(SERVER_UART, "\n", 1);
    xSemaphoreGive(serial_mtx);
}

int zone_from_mac(const uint8_t *mac)
{
    for (int i = 0; i < NUM_ZONES; i++)
        if (memcmp(mac, NODE_MAC[i], 6) == 0) return i + 1;
    return 0;
}

bool gw_send(uint8_t zone, const void *data, size_t len)
{
    if (zone < 1 || zone > NUM_ZONES) return false;
    return esp_now_send(NODE_MAC[zone - 1], data, len) == ESP_OK;
}

static void uart_init(void)
{
    serial_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(uart_driver_install(SERVER_UART, 2048, 2048, 0, NULL, 0));
    uart_vfs_dev_use_driver(SERVER_UART);  // log ESP_LOG cũng đi qua driver, không chen ngang dòng JSON
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());

    for (int i = 0; i < NUM_ZONES; i++) {
        esp_now_peer_info_t peer = {.channel = ESPNOW_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};
        memcpy(peer.peer_addr, NODE_MAC[i], 6);
        if (esp_now_add_peer(&peer) != ESP_OK) serial_out("# them peer loi (MAC trong app_config.h?)");
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uart_init();
    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);

    espnow_init();
    uplink_init();
    cmd_init();
    uplink_start_tasks();
    cmd_start_tasks();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char line[96];
    snprintf(line, sizeof(line), "{\"t\":\"hello\",\"fw\":\"gw-1.0\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    serial_out(line);
}
