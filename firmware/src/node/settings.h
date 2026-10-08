#pragma once
#include <stdint.h>

// Cấu hình nút lưu trong NVS, server cập nhật qua MSG_CONFIG (tuần 11)
struct NodeSettings {
    uint16_t soil_low_x10;     // ngưỡng tưới fallback (% × 10)
    uint16_t pump_max_s;       // ≤ PUMP_MAX_S
    uint16_t fallback_water_s; // ≤ pump_max_s
};

void settings_init();
NodeSettings settings_get();
bool settings_set(const NodeSettings &s);  // false nếu giá trị không hợp lệ
