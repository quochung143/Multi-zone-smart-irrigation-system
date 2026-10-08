// Gateway ESP-NOW <-> Serial (docs/DESIGN.md §4.3)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#include "config.h"
#include "gw.h"

static SemaphoreHandle_t serial_mtx;

void serial_out_init() {
    serial_mtx = xSemaphoreCreateMutex();
}

void serial_out(const char *line) {
    xSemaphoreTake(serial_mtx, portMAX_DELAY);
    Serial.print(line);
    Serial.print('\n');
    xSemaphoreGive(serial_mtx);
}

int zone_from_mac(const uint8_t *mac) {
    for (int i = 0; i < NUM_ZONES; i++)
        if (memcmp(mac, NODE_MAC[i], 6) == 0) return i + 1;
    return 0;
}

bool gw_send(uint8_t zone, const void *data, size_t len) {
    if (zone < 1 || zone > NUM_ZONES) return false;
    return esp_now_send(NODE_MAC[zone - 1], (const uint8_t *)data, len) == ESP_OK;
}

static void espnow_init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        serial_out("# ESP-NOW init loi, khoi dong lai");
        delay(1000);
        ESP.restart();
    }
    for (int i = 0; i < NUM_ZONES; i++) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, NODE_MAC[i], 6);
        peer.channel = ESPNOW_CHANNEL;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) serial_out("# add peer loi");
    }
}

void setup() {
    Serial.begin(115200);
    serial_out_init();
    pinMode(PIN_LED, OUTPUT);
    esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);

    espnow_init();
    uplink_init();
    cmd_init();
    uplink_start_tasks();
    cmd_start_tasks();

    char line[96];
    snprintf(line, sizeof(line), "{\"t\":\"hello\",\"fw\":\"gw-1.0\",\"mac\":\"%s\"}",
             WiFi.macAddress().c_str());
    serial_out(line);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
