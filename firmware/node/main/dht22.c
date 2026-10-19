// Node Zone: driver DHT22/AM2302 qua RMT RX (datasheet Aosong AM2302)
//
// Khung truyền:  host kéo LOW ≥1 ms rồi nhả → DHT trả LOW 80 µs, HIGH 80 µs
//                → 40 bit, mỗi bit = LOW 50 µs + HIGH 26–28 µs (bit 0) hoặc 70 µs (bit 1)
//                → 5 byte: độ ẩm (2), nhiệt độ (2, bit 15 = dấu âm), checksum
#include "dht22.h"

#include <string.h>

#include "driver/rmt_rx.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define RMT_RESOLUTION_HZ 1000000  // 1 tick = 1 µs
#define RMT_SYMBOLS       64
#define START_LOW_MS      3        // host giữ LOW (datasheet: ≥ 1 ms)
#define BIT_ONE_MIN_US    48       // HIGH dài hơn ngưỡng này là bit 1
#define READ_TIMEOUT_MS   50

static const char *TAG = "dht22";

static gpio_num_t dht_pin;
static rmt_channel_handle_t rx_chan;
static QueueHandle_t done_queue;
static rmt_symbol_word_t symbols[RMT_SYMBOLS];

static const rmt_receive_config_t rx_cfg = {
    .signal_range_min_ns = 1000,    // bỏ gai nhiễu < 1 µs
    .signal_range_max_ns = 200000,  // mức giữ nguyên > 200 µs = DHT đã truyền xong
};

static bool on_rx_done(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(done_queue, edata, &woken);
    return woken == pdTRUE;
}

esp_err_t dht22_init(gpio_num_t pin)
{
    dht_pin = pin;
    done_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));

    rmt_rx_channel_config_t chan_cfg = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = RMT_SYMBOLS,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&chan_cfg, &rx_chan), TAG, "rx channel");
    rmt_rx_event_callbacks_t cbs = {.on_recv_done = on_rx_done};
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL), TAG, "callback");
    ESP_RETURN_ON_ERROR(rmt_enable(rx_chan), TAG, "enable");

    // Cấu hình sau rmt_new_rx_channel: open-drain để host kéo LOW được, RMT vẫn đọc cùng chân
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    gpio_set_level(pin, 1);  // nhả dây ở trạng thái nghỉ
    return ESP_OK;
}

// Dàn các symbol RMT thành dãy xung, lấy 40 xung HIGH cuối cùng làm 40 bit dữ liệu.
static esp_err_t decode(const rmt_symbol_word_t *sym, size_t n, uint8_t out[5])
{
    uint16_t highs[2 * RMT_SYMBOLS];
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (sym[i].level0 == 1 && sym[i].duration0 > 0) highs[count++] = sym[i].duration0;
        if (sym[i].level1 == 1 && sym[i].duration1 > 0) highs[count++] = sym[i].duration1;
    }
    if (count < 40) return ESP_ERR_INVALID_RESPONSE;

    memset(out, 0, 5);
    const uint16_t *bits = &highs[count - 40];
    for (int i = 0; i < 40; i++)
        if (bits[i] > BIT_ONE_MIN_US) out[i / 8] |= (uint8_t)(0x80 >> (i % 8));

    uint8_t sum = (uint8_t)(out[0] + out[1] + out[2] + out[3]);
    return sum == out[4] ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t dht22_read(float *temp_c, float *hum_pct)
{
    rmt_rx_done_event_data_t done;
    xQueueReset(done_queue);

    // Tín hiệu bắt đầu: kéo LOW, bật RMT ngay trước khi nhả để không đo nhầm đoạn LOW dài này
    gpio_set_level(dht_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(START_LOW_MS));
    esp_err_t err = rmt_receive(rx_chan, symbols, sizeof(symbols), &rx_cfg);
    gpio_set_level(dht_pin, 1);
    if (err != ESP_OK) return err;

    if (xQueueReceive(done_queue, &done, pdMS_TO_TICKS(READ_TIMEOUT_MS)) != pdTRUE) {
        // Không có phản hồi: khởi động lại kênh để lần sau rmt_receive không báo bận
        rmt_disable(rx_chan);
        rmt_enable(rx_chan);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t b[5];
    err = decode(done.received_symbols, done.num_symbols, b);
    if (err != ESP_OK) return err;  // không log: sensor đã bật cờ lỗi, tránh in mỗi 5 s

    *hum_pct = ((b[0] << 8) | b[1]) / 10.0f;
    float t = (((b[2] & 0x7F) << 8) | b[3]) / 10.0f;
    *temp_c = (b[2] & 0x80) ? -t : t;
    return ESP_OK;
}
