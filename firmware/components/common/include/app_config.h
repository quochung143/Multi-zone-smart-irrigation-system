#pragma once
#include <stdint.h>

#include "esp_timer.h"

// Cấu hình chung node/gateway (docs/DESIGN.md §4.1)

// ===== ESP-NOW =====
#define ESPNOW_CHANNEL        1
#define NUM_ZONES             3

// TODO: điền MAC thật sau khi đọc từ từng ESP32 (lệnh `mac` trên nút, dòng `hello` của gateway)
static const uint8_t GATEWAY_MAC[6]         = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t NODE_MAC[NUM_ZONES][6] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // Zone 1
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // Zone 2
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // Zone 3
};

// ===== Chân (node) =====
#define PIN_SOIL_ADC_CHANNEL  ADC_CHANNEL_6  // GPIO34, ADC1
#define PIN_SDA               21
#define PIN_SCL               22
#define PIN_RELAY             26             // relay active-LOW
#define PIN_LED               2
#define RELAY_ON_LEVEL        0
#define RELAY_OFF_LEVEL       1

// ===== Thời gian =====
#define TELEMETRY_PERIOD_MS   30000
#define TELEMETRY_FIRST_MS    2000    // gói đầu sau boot (+ zone giây để lệch pha)
#define SENSOR_PERIOD_MS      1000
#define SHT_PERIOD_MS         5000
#define NODE_OFFLINE_MS       90000   // gateway: 3 chu kỳ không nhận
#define BEACON_PERIOD_MS      60000
#define FALLBACK_TIMEOUT_MS   300000  // node: 5 phút không nghe gateway
#define SERVER_TIMEOUT_MS     180000  // gateway: không nghe server 3 phút thì ngừng beacon
#define CMD_RETRY_MAX         3
#define CMD_ACK_TIMEOUT_MS    300
#define TX_RETRY_MAX          2       // node: gửi lại khi lỗi lớp MAC
#define TX_WAIT_MS            100     // node: chờ callback gửi

// ===== Cảm biến (node) =====
#define SOIL_OVERSAMPLE       16      // số lần đọc ADC lấy trung bình mỗi mẫu
#define SOIL_MEDIAN_N         5       // cửa sổ median
#define SOIL_EMA_ALPHA        0.2f    // hệ số EMA
#define ADC_DEFAULT_DRY       3000    // giá trị mặc định khi chưa hiệu chuẩn
#define ADC_DEFAULT_WET       1300
#define ADC_VALID_MIN         100     // ngoài khoảng này coi như lỗi cảm biến
#define ADC_VALID_MAX         4000
#define ADC_CAL_MARGIN        300     // cho phép vượt mốc khô/ướt đã hiệu chuẩn
#define CAL_MIN_SPAN          500     // khô - ướt tối thiểu
#define SHT30_ADDR            0x44
#define PUMP_TASK_PERIOD_MS   100

// ===== An toàn bơm =====
#define PUMP_MAX_S            30
#define PUMP_COOLDOWN_MS      600000  // 10 phút
#define FALLBACK_WATER_S      10
#define DEFAULT_SOIL_LOW_X10  350     // 35.0 %

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
