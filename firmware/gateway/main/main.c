// Gateway ESP-NOW <-> MQTT qua Wi-Fi hotspot (docs/DESIGN.md §4.3, docs/SERVER.md §3)
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "gw.h"
#include "nvs_flash.h"

static const char *TAG = "gw";

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

static void radio_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    net_init();  // nạp cấu hình Wi-Fi/broker từ NVS, đăng ký sự kiện
    ESP_ERROR_CHECK(esp_wifi_start());
    // Chưa cấu hình Wi-Fi: đứng yên ở ESPNOW_CHANNEL. Có Wi-Fi thì kênh theo hotspot.
    if (!net_config_get().ssid[0]) ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());

    static const uint8_t unset[6] = {0};
    for (int i = 0; i < NUM_ZONES; i++) {
        if (memcmp(NODE_MAC[i], unset, 6) == 0) continue;  // zone chưa điền MAC (đang thử ít nút)
        // Kênh 0 = kênh hiện tại của giao diện STA, tự theo kênh hotspot
        esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
        memcpy(peer.peer_addr, NODE_MAC[i], 6);
        if (esp_now_add_peer(&peer) != ESP_OK) ESP_LOGE(TAG, "Them peer Z%d loi (MAC trong app_config.h?)", i + 1);
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

    // Log mặc định WARN (log Wi-Fi rất nhiều); module của gateway in cả INFO để chẩn đoán trên console
    static const char *const tags[] = {"gw", "net", "cmd", "uplink"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) esp_log_level_set(tags[i], ESP_LOG_INFO);

    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);

    radio_init();
    uplink_init();
    cmd_init();
    uplink_start_tasks();
    cmd_start_tasks();
    net_start();
    display_start();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "gw-2.0 MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    console_start();
}
