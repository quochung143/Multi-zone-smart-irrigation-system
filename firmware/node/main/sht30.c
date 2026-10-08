// Node Zone: driver SHT30 (datasheet Sensirion SHT3x-DIS)
#include "sht30.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_TIMEOUT_MS 50

static const char *TAG = "sht30";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;

// CRC-8: đa thức 0x31, giá trị đầu 0xFF
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

esp_err_t sht30_init(int sda, int scl, uint8_t addr)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev), TAG, "device");

    // Thiếu cảm biến thì driver báo NACK mỗi lần đọc; đã có cờ FLAG_SHT_ERR nên tắt log này
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    return ESP_OK;
}

esp_err_t sht30_read(float *temp_c, float *hum_pct)
{
    const uint8_t cmd[2] = {0x24, 0x00};  // single shot, high repeatability, no clock stretching
    ESP_RETURN_ON_ERROR(i2c_master_transmit(dev, cmd, sizeof(cmd), I2C_TIMEOUT_MS), TAG, "cmd");
    vTaskDelay(pdMS_TO_TICKS(20));  // thời gian đo tối đa 15.5 ms

    uint8_t buf[6];
    ESP_RETURN_ON_ERROR(i2c_master_receive(dev, buf, sizeof(buf), I2C_TIMEOUT_MS), TAG, "read");
    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return ESP_ERR_INVALID_CRC;

    uint16_t raw_t = (uint16_t)(buf[0] << 8 | buf[1]);
    uint16_t raw_h = (uint16_t)(buf[3] << 8 | buf[4]);
    *temp_c = -45.0f + 175.0f * raw_t / 65535.0f;
    *hum_pct = 100.0f * raw_h / 65535.0f;
    return ESP_OK;
}
