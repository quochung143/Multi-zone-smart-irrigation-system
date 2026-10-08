// Node Zone (docs/DESIGN.md §4.2)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "comm.h"
#include "config.h"
#include "console.h"
#include "pump.h"
#include "sensor.h"
#include "settings.h"

void setup() {
    pump_init();  // relay OFF càng sớm càng tốt
    Serial.begin(115200);

    esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);
    settings_init();
    sensor_init();
    comm_init();

    sensor_start_task();
    pump_start_task();
    comm_start_task();
    console_start_task();

    Serial.printf("\nNode Zone %d | MAC %s | go 'help'\n", ZONE_ID, WiFi.macAddress().c_str());
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
