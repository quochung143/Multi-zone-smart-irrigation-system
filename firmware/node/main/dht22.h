#pragma once
#include "esp_err.h"
#include "driver/gpio.h"

// Driver DHT22 (AM2302) đọc bằng RMT RX: phần cứng đo độ rộng xung nên không bị ngắt Wi-Fi làm sai.
// Khoảng cách giữa 2 lần đọc phải ≥ 2 s.
esp_err_t dht22_init(gpio_num_t pin);
// ESP_ERR_TIMEOUT nếu cảm biến không trả lời, ESP_ERR_INVALID_CRC nếu sai checksum.
esp_err_t dht22_read(float *temp_c, float *hum_pct);
