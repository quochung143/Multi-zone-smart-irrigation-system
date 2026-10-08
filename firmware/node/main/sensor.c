// Node Zone: đọc + lọc độ ẩm đất, hiệu chuẩn khô/ướt, đọc SHT30 (docs/DESIGN.md §4.2)
#include "sensor.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "protocol.h"
#include "sht30.h"

static const char *TAG = "sensor";

static SemaphoreHandle_t mtx;
static sensor_data_t data;
static soil_cal_t cal = {ADC_DEFAULT_DRY, ADC_DEFAULT_WET};

static adc_oneshot_unit_handle_t adc;
static bool sht_ready = false;

static uint16_t median_buf[SOIL_MEDIAN_N];
static uint8_t median_count = 0;
static uint8_t median_idx = 0;
static float ema_raw = -1.0f;

// Trung bình SOIL_OVERSAMPLE lần đọc để giảm nhiễu lượng tử/nhiễu nguồn.
static uint16_t read_oversampled(void)
{
    uint32_t sum = 0;
    int ok = 0;
    for (int i = 0; i < SOIL_OVERSAMPLE; i++) {
        int v;
        if (adc_oneshot_read(adc, PIN_SOIL_ADC_CHANNEL, &v) == ESP_OK) {
            sum += v;
            ok++;
        }
        esp_rom_delay_us(100);
    }
    return ok ? (uint16_t)(sum / ok) : 0;
}

// Đưa mẫu vào cửa sổ trượt và trả về median, loại bỏ gai nhiễu đơn lẻ.
static uint16_t median_push(uint16_t v)
{
    median_buf[median_idx] = v;
    median_idx = (median_idx + 1) % SOIL_MEDIAN_N;
    if (median_count < SOIL_MEDIAN_N) median_count++;

    uint16_t tmp[SOIL_MEDIAN_N];
    memcpy(tmp, median_buf, median_count * sizeof(uint16_t));
    for (int i = 1; i < median_count; i++) {
        uint16_t key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    return tmp[median_count / 2];
}

static float raw_to_pct(float raw, soil_cal_t c)
{
    float pct = (c.dry - raw) * 100.0f / (c.dry - c.wet);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// Rút dây hoặc chập cảm biến cho giá trị sát 0 / 4095 hoặc lệch xa vùng hiệu chuẩn.
static bool raw_valid(uint16_t raw, soil_cal_t c)
{
    return raw >= ADC_VALID_MIN && raw <= ADC_VALID_MAX &&
           raw <= c.dry + ADC_CAL_MARGIN && raw + ADC_CAL_MARGIN >= c.wet;
}

static bool cal_valid(uint16_t dry, uint16_t wet)
{
    return dry <= ADC_VALID_MAX && wet >= ADC_VALID_MIN && dry >= wet + CAL_MIN_SPAN;
}

static void cal_save(soil_cal_t c)
{
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "dry", c.dry);
    nvs_set_u16(h, "wet", c.wet);
    nvs_commit(h);
    nvs_close(h);
}

static void sensor_task(void *arg)
{
    esp_task_wdt_add(NULL);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_sht_ms = 0;
    bool first = true;

    for (;;) {
        esp_task_wdt_reset();
        uint16_t med = median_push(read_oversampled());
        ema_raw = (ema_raw < 0) ? med : SOIL_EMA_ALPHA * med + (1.0f - SOIL_EMA_ALPHA) * ema_raw;

        bool sht_read = false, sht_ok = false;
        float t = NAN, h = NAN;
        if (first || now_ms() - last_sht_ms >= SHT_PERIOD_MS) {
            first = false;
            last_sht_ms = now_ms();
            sht_read = true;
            sht_ok = sht_ready && sht30_read(&t, &h) == ESP_OK;
        }

        xSemaphoreTake(mtx, portMAX_DELAY);
        data.soil_raw = (uint16_t)(ema_raw + 0.5f);
        data.soil_pct = raw_to_pct(ema_raw, cal);
        data.ready = true;
        if (raw_valid(med, cal)) data.flags &= ~FLAG_ADC_ERR;
        else data.flags |= FLAG_ADC_ERR;
        if (sht_read) {
            if (sht_ok) {
                data.temp_c = t;
                data.hum_pct = h;
                data.flags &= ~FLAG_SHT_ERR;
            } else {
                data.flags |= FLAG_SHT_ERR;
            }
        }
        xSemaphoreGive(mtx);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

void sensor_init(void)
{
    mtx = xSemaphoreCreateMutex();
    data = (sensor_data_t){0, 0.0f, NAN, NAN, 0, false};

    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc));
    adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};  // ~0..3.1 V
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, PIN_SOIL_ADC_CHANNEL, &chan_cfg));

    sht_ready = sht30_init(PIN_SDA, PIN_SCL, SHT30_ADDR) == ESP_OK;
    if (!sht_ready) ESP_LOGE(TAG, "khoi tao I2C loi");

    nvs_handle_t h;
    if (nvs_open("cal", NVS_READONLY, &h) == ESP_OK) {
        uint16_t dry = ADC_DEFAULT_DRY, wet = ADC_DEFAULT_WET;
        nvs_get_u16(h, "dry", &dry);
        nvs_get_u16(h, "wet", &wet);
        nvs_close(h);
        if (cal_valid(dry, wet)) cal = (soil_cal_t){dry, wet};
    }
}

void sensor_start_task(void)
{
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 2, NULL, 1);
}

sensor_data_t sensor_get(void)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    sensor_data_t d = data;
    xSemaphoreGive(mtx);
    return d;
}

soil_cal_t sensor_get_cal(void)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    soil_cal_t c = cal;
    xSemaphoreGive(mtx);
    return c;
}

bool sensor_cal_set(uint16_t dry, uint16_t wet)
{
    if (!cal_valid(dry, wet)) return false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    cal = (soil_cal_t){dry, wet};
    xSemaphoreGive(mtx);
    cal_save((soil_cal_t){dry, wet});
    return true;
}

bool sensor_cal_capture_dry(void)
{
    sensor_data_t d = sensor_get();
    return d.ready && sensor_cal_set(d.soil_raw, sensor_get_cal().wet);
}

bool sensor_cal_capture_wet(void)
{
    sensor_data_t d = sensor_get();
    return d.ready && sensor_cal_set(sensor_get_cal().dry, d.soil_raw);
}
