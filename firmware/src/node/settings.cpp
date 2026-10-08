// Node Zone: cấu hình lưu NVS (docs/DESIGN.md §4.2)
#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static NodeSettings current = {DEFAULT_SOIL_LOW_X10, PUMP_MAX_S, FALLBACK_WATER_S};

static bool valid(const NodeSettings &s) {
    return s.soil_low_x10 <= 1000 &&
           s.pump_max_s >= 1 && s.pump_max_s <= PUMP_MAX_S &&
           s.fallback_water_s >= 1 && s.fallback_water_s <= s.pump_max_s;
}

void settings_init() {
    Preferences prefs;
    prefs.begin("cfg", true);
    NodeSettings s = {
        prefs.getUShort("low", DEFAULT_SOIL_LOW_X10),
        prefs.getUShort("max", PUMP_MAX_S),
        prefs.getUShort("fb", FALLBACK_WATER_S),
    };
    prefs.end();
    if (valid(s)) current = s;
}

NodeSettings settings_get() {
    portENTER_CRITICAL(&mux);
    NodeSettings s = current;
    portEXIT_CRITICAL(&mux);
    return s;
}

bool settings_set(const NodeSettings &s) {
    if (!valid(s)) return false;
    portENTER_CRITICAL(&mux);
    current = s;
    portEXIT_CRITICAL(&mux);

    Preferences prefs;
    prefs.begin("cfg", false);
    prefs.putUShort("low", s.soil_low_x10);
    prefs.putUShort("max", s.pump_max_s);
    prefs.putUShort("fb", s.fallback_water_s);
    prefs.end();
    return true;
}
