#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t soil_raw;  // ADC sau lọc median + EMA
    float    soil_pct;  // 0..100 theo hiệu chuẩn
    float    temp_c;    // NAN nếu chưa đọc được
    float    hum_pct;   // NAN nếu chưa đọc được
    uint8_t  flags;     // FLAG_SHT_ERR | FLAG_ADC_ERR
    bool     ready;     // đã có ít nhất 1 mẫu độ ẩm đất
} sensor_data_t;

typedef struct {
    uint16_t dry;  // ADC khi để trong không khí (giá trị cao)
    uint16_t wet;  // ADC khi nhúng nước (giá trị thấp)
} soil_cal_t;

void sensor_init(void);
void sensor_start_task(void);
sensor_data_t sensor_get(void);

soil_cal_t sensor_get_cal(void);
// Lấy soil_raw hiện tại làm mốc khô/ướt; false nếu khô - ướt < CAL_MIN_SPAN.
bool sensor_cal_capture_dry(void);
bool sensor_cal_capture_wet(void);
bool sensor_cal_set(uint16_t dry, uint16_t wet);
