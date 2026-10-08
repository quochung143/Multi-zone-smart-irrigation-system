// Node Zone: cấu hình lưu NVS (docs/DESIGN.md §4.2)
#include "settings.h"

#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static node_settings_t current = {DEFAULT_SOIL_LOW_X10, PUMP_MAX_S, FALLBACK_WATER_S};
static uint8_t zone_id = 0;

static bool valid(const node_settings_t *s)
{
    return s->soil_low_x10 <= 1000 &&
           s->pump_max_s >= 1 && s->pump_max_s <= PUMP_MAX_S &&
           s->fallback_water_s >= 1 && s->fallback_water_s <= s->pump_max_s;
}

void settings_init(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;  // chưa có gì: dùng mặc định
    node_settings_t s = current;
    nvs_get_u16(h, "low", &s.soil_low_x10);
    nvs_get_u16(h, "max", &s.pump_max_s);
    nvs_get_u16(h, "fb", &s.fallback_water_s);
    uint8_t z = 0;
    if (nvs_get_u8(h, "zone", &z) == ESP_OK && z >= 1 && z <= NUM_ZONES) zone_id = z;
    nvs_close(h);
    if (valid(&s)) current = s;
}

node_settings_t settings_get(void)
{
    taskENTER_CRITICAL(&mux);
    node_settings_t s = current;
    taskEXIT_CRITICAL(&mux);
    return s;
}

bool settings_set(const node_settings_t *s)
{
    if (!valid(s)) return false;
    taskENTER_CRITICAL(&mux);
    current = *s;
    taskEXIT_CRITICAL(&mux);

    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "low", s->soil_low_x10);
        nvs_set_u16(h, "max", s->pump_max_s);
        nvs_set_u16(h, "fb", s->fallback_water_s);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

uint8_t settings_get_zone(void)
{
    return zone_id;
}

bool settings_set_zone(uint8_t zone)
{
    if (zone < 1 || zone > NUM_ZONES) return false;
    zone_id = zone;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "zone", zone);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}
