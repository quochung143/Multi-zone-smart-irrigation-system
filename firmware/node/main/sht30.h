#pragma once
#include "esp_err.h"

// Driver SHT30 tối giản trên i2c_master (đo đơn, độ lặp cao, không clock stretching)
esp_err_t sht30_init(int sda, int scl, uint8_t addr);
esp_err_t sht30_read(float *temp_c, float *hum_pct);  // ESP_ERR_INVALID_CRC nếu sai CRC
