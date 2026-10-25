#pragma once
#include <stdbool.h>
#include <stdint.h>

// Cấu hình nút lưu trong NVS, server cập nhật qua MSG_CONFIG
typedef struct {
    uint16_t soil_low_x10;     // ngưỡng tưới fallback (% × 10)
    uint16_t pump_max_s;       // ≤ PUMP_MAX_S
    uint16_t fallback_water_s; // ≤ pump_max_s
} node_settings_t;

void settings_init(void);
node_settings_t settings_get(void);
bool settings_set(const node_settings_t *s);  // false nếu giá trị không hợp lệ

// Zone ID của nút (1..NUM_ZONES), 0 = chưa đặt. Cả 3 nút dùng chung một firmware.
uint8_t settings_get_zone(void);
bool settings_set_zone(uint8_t zone);

// Kênh ESP-NOW tìm được lần cuối (1..CHANNEL_MAX), mặc định ESPNOW_CHANNEL
uint8_t settings_get_channel(void);
void settings_set_channel(uint8_t ch);
