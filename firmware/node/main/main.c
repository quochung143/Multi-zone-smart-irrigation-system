// Node Zone (docs/DESIGN.md §4.2)
#include "comm.h"
#include "console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "pump.h"
#include "sensor.h"
#include "settings.h"

static const char *TAG = "node";

void app_main(void)
{
    pump_init();  // relay OFF càng sớm càng tốt

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings_init();
    sensor_init();
    comm_init();

    sensor_start_task();
    pump_start_task();
    comm_start_task();

    ESP_LOGI(TAG, "Node Zone %u san sang, go 'help'", settings_get_zone());
    console_start();
}
