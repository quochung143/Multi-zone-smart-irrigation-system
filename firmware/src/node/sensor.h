#pragma once
#include <stdint.h>

struct SensorData {
    uint16_t soil_raw;  // ADC sau lọc median + EMA
    float    soil_pct;  // 0..100 theo hiệu chuẩn
    float    temp_c;    // NAN nếu chưa đọc được
    float    hum_pct;   // NAN nếu chưa đọc được
    uint8_t  flags;     // FLAG_SHT_ERR | FLAG_ADC_ERR
    bool     ready;     // đã có ít nhất 1 mẫu độ ẩm đất
};

struct SoilCal {
    uint16_t dry;  // ADC khi để trong không khí (giá trị cao)
    uint16_t wet;  // ADC khi nhúng nước (giá trị thấp)
};

void sensor_init();
void sensor_start_task();
SensorData sensor_get();

SoilCal sensor_get_cal();
// Lấy giá trị soil_raw hiện tại làm mốc khô/ướt; trả về false nếu lệch nhau < CAL_MIN_SPAN.
bool sensor_cal_capture_dry();
bool sensor_cal_capture_wet();
bool sensor_cal_set(uint16_t dry, uint16_t wet);
