// Node Zone: đọc + lọc độ ẩm đất, hiệu chuẩn khô/ướt, đọc SHT30 (docs/DESIGN.md §4.2)
#include "sensor.h"

#include <Adafruit_SHT31.h>
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "protocol.h"

static SemaphoreHandle_t mtx;
static SensorData data;
static SoilCal cal = {ADC_DEFAULT_DRY, ADC_DEFAULT_WET};

static Adafruit_SHT31 sht;
static bool sht_ok = false;

static uint16_t median_buf[SOIL_MEDIAN_N];
static uint8_t median_count = 0;
static uint8_t median_idx = 0;
static float ema_raw = -1.0f;

// Trung bình SOIL_OVERSAMPLE lần đọc để giảm nhiễu lượng tử/nhiễu nguồn.
static uint16_t read_oversampled() {
    uint32_t sum = 0;
    for (int i = 0; i < SOIL_OVERSAMPLE; i++) {
        sum += analogRead(PIN_SOIL);
        delayMicroseconds(100);
    }
    return sum / SOIL_OVERSAMPLE;
}

// Đưa mẫu vào cửa sổ trượt và trả về median, loại bỏ gai nhiễu đơn lẻ.
static uint16_t median_push(uint16_t v) {
    median_buf[median_idx] = v;
    median_idx = (median_idx + 1) % SOIL_MEDIAN_N;
    if (median_count < SOIL_MEDIAN_N) median_count++;

    uint16_t tmp[SOIL_MEDIAN_N];
    memcpy(tmp, median_buf, median_count * sizeof(uint16_t));
    for (uint8_t i = 1; i < median_count; i++) {
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

static float raw_to_pct(float raw, const SoilCal &c) {
    float pct = (c.dry - raw) * 100.0f / (c.dry - c.wet);
    return constrain(pct, 0.0f, 100.0f);
}

// Rút dây hoặc chập cảm biến cho giá trị sát 0 / 4095 hoặc lệch xa vùng hiệu chuẩn.
static bool raw_valid(uint16_t raw, const SoilCal &c) {
    return raw >= ADC_VALID_MIN && raw <= ADC_VALID_MAX &&
           raw <= c.dry + ADC_CAL_MARGIN && raw + ADC_CAL_MARGIN >= c.wet;
}

static bool cal_valid(uint16_t dry, uint16_t wet) {
    return dry <= ADC_VALID_MAX && wet >= ADC_VALID_MIN && dry >= wet + CAL_MIN_SPAN;
}

static void cal_save(const SoilCal &c) {
    Preferences prefs;
    prefs.begin("cal", false);
    prefs.putUShort("dry", c.dry);
    prefs.putUShort("wet", c.wet);
    prefs.end();
}

static void sensor_task(void *) {
    esp_task_wdt_add(NULL);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_sht_ms = 0;
    bool first = true;

    for (;;) {
        esp_task_wdt_reset();
        uint16_t med = median_push(read_oversampled());
        ema_raw = (ema_raw < 0) ? med : SOIL_EMA_ALPHA * med + (1.0f - SOIL_EMA_ALPHA) * ema_raw;

        bool sht_read = false;
        float t = NAN, h = NAN;
        if (first || millis() - last_sht_ms >= SHT_PERIOD_MS) {
            first = false;
            last_sht_ms = millis();
            sht_read = true;
            if (!sht_ok) sht_ok = sht.begin(SHT30_ADDR);  // thử kết nối lại nếu lỗi
            if (sht_ok && !sht.readBoth(&t, &h)) sht_ok = false;
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

void sensor_init() {
    mtx = xSemaphoreCreateMutex();
    data = {0, 0.0f, NAN, NAN, 0, false};

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_SOIL, ADC_11db);  // dải ~0..3.1 V
    Wire.begin(PIN_SDA, PIN_SCL);

    Preferences prefs;
    prefs.begin("cal", true);
    uint16_t dry = prefs.getUShort("dry", ADC_DEFAULT_DRY);
    uint16_t wet = prefs.getUShort("wet", ADC_DEFAULT_WET);
    prefs.end();
    if (cal_valid(dry, wet)) cal = {dry, wet};
}

void sensor_start_task() {
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 2, NULL, 1);
}

SensorData sensor_get() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    SensorData d = data;
    xSemaphoreGive(mtx);
    return d;
}

SoilCal sensor_get_cal() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    SoilCal c = cal;
    xSemaphoreGive(mtx);
    return c;
}

bool sensor_cal_set(uint16_t dry, uint16_t wet) {
    if (!cal_valid(dry, wet)) return false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    cal = {dry, wet};
    xSemaphoreGive(mtx);
    cal_save({dry, wet});
    return true;
}

bool sensor_cal_capture_dry() {
    SensorData d = sensor_get();
    return d.ready && sensor_cal_set(d.soil_raw, sensor_get_cal().wet);
}

bool sensor_cal_capture_wet() {
    SensorData d = sensor_get();
    return d.ready && sensor_cal_set(sensor_get_cal().dry, d.soil_raw);
}
