// Node Zone: ESP-NOW + chế độ fallback (docs/DESIGN.md §4.2, docs/PROTOCOL.md)
#include "comm.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pump.h"
#include "sensor.h"
#include "settings.h"

static const char *TAG = "comm";

typedef struct {
    uint8_t len;
    uint8_t data[MSG_MAX_LEN];
} rx_item_t;

static QueueHandle_t rx_queue;
static QueueHandle_t event_queue;
static TaskHandle_t comm_task_handle = NULL;

static volatile uint32_t last_gw_rx_ms = 0;
static volatile node_mode_t mode = MODE_NORMAL;
static uint16_t tx_seq = 0;
static uint32_t led_until_ms = 0;

// Chống thực thi lặp khi gateway gửi lại lệnh
static bool has_last_cmd = false;
static uint16_t last_cmd_seq = 0;
static ack_status_t last_ack = ACK_OK;

// ---------- Callback ESP-NOW (chạy trong task Wi-Fi, chỉ copy dữ liệu) ----------

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, GATEWAY_MAC, 6) != 0) return;
    if (len <= 0 || len > MSG_MAX_LEN || !msg_validate(data, len)) return;
    rx_item_t item = {.len = (uint8_t)len};
    memcpy(item.data, data, len);
    xQueueSend(rx_queue, &item, 0);
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (comm_task_handle)
        xTaskNotify(comm_task_handle, status == ESP_NOW_SEND_SUCCESS ? 1 : 2, eSetValueWithOverwrite);
}

static void on_pump_event(const pump_event_t *ev)
{
    if (ev->on) ESP_LOGI(TAG, "bom ON src=%s seq=%u", pump_source_name(ev->source), ev->cmd_seq);
    else ESP_LOGI(TAG, "bom OFF src=%s seq=%u chay=%us", pump_source_name(ev->source), ev->cmd_seq,
                  ev->duration_s);
    xQueueSend(event_queue, ev, 0);
}

// ---------- Gửi (chỉ gọi từ commTask) ----------

// Gửi và chờ ACK lớp MAC; lỗi thì gửi lại tối đa TX_RETRY_MAX lần.
static bool send_reliable(const void *msg, size_t len)
{
    uint32_t result;
    for (int attempt = 0; attempt <= TX_RETRY_MAX; attempt++) {
        xTaskNotifyWait(0, UINT32_MAX, &result, 0);  // xóa thông báo cũ
        if (esp_now_send(GATEWAY_MAC, msg, len) != ESP_OK) continue;
        if (xTaskNotifyWait(0, UINT32_MAX, &result, pdMS_TO_TICKS(TX_WAIT_MS)) == pdTRUE && result == 1)
            return true;
    }
    return false;
}

static void send_telemetry(uint8_t zone)
{
    msg_telemetry_t msg;
    comm_build_telemetry(&msg, zone, ++tx_seq);
    if (!send_reliable(&msg, sizeof(msg))) ESP_LOGW(TAG, "gui telemetry loi");
}

static void send_ack(uint8_t zone, uint16_t cmd_seq, ack_status_t status)
{
    msg_ack_t msg = {{MSG_ACK, zone, ++tx_seq}, cmd_seq, status};
    send_reliable(&msg, sizeof(msg));
}

static void send_pump_event(uint8_t zone, const pump_event_t *ev)
{
    msg_pump_event_t msg = {{MSG_PUMP_EVENT, zone, ++tx_seq},
                            ev->on ? 1 : 0, ev->source, ev->cmd_seq, ev->duration_s};
    send_reliable(&msg, sizeof(msg));
}

// ---------- Xử lý gói nhận ----------

static ack_status_t execute_command(const rx_item_t *item)
{
    switch (item->data[0]) {
    case MSG_CMD_WATER: {
        msg_cmd_water_t cmd;
        memcpy(&cmd, item->data, sizeof(cmd));
        return pump_start(cmd.duration_s, SRC_CMD, cmd.h.seq);
    }
    case MSG_CMD_STOP:
        pump_stop();
        return ACK_OK;
    case MSG_CONFIG: {
        msg_config_t cfg;
        memcpy(&cfg, item->data, sizeof(cfg));
        node_settings_t s = {cfg.soil_low_x10, cfg.pump_max_s, cfg.fallback_water_s};
        return settings_set(&s) ? ACK_OK : ACK_REJ_INVALID;
    }
    default:
        return ACK_REJ_INVALID;
    }
}

static void handle_rx(uint8_t zone, const rx_item_t *item)
{
    msg_header_t h;
    memcpy(&h, item->data, sizeof(h));
    if (h.zone_id != zone) return;

    last_gw_rx_ms = now_ms();
    led_until_ms = last_gw_rx_ms + 50;

    switch (h.type) {
    case MSG_BEACON:
        break;  // chỉ dùng làm heartbeat
    case MSG_CMD_WATER:
    case MSG_CMD_STOP:
    case MSG_CONFIG: {
        if (has_last_cmd && h.seq == last_cmd_seq) {
            send_ack(zone, h.seq, last_ack);  // lệnh gửi lại: chỉ xác nhận lại, không thực thi
            return;
        }
        ack_status_t status = execute_command(item);
        has_last_cmd = true;
        last_cmd_seq = h.seq;
        last_ack = status;
        ESP_LOGI(TAG, "lenh type=%u seq=%u -> ack %d", h.type, h.seq, status);
        send_ack(zone, h.seq, status);
        break;
    }
    default:
        break;  // gói uplink không dành cho nút
    }
}

// ---------- Fallback ----------

static void fallback_check(void)
{
    sensor_data_t s = sensor_get();
    if (!s.ready || (s.flags & FLAG_ADC_ERR)) return;  // cảm biến lỗi thì không tự tưới
    node_settings_t cfg = settings_get();
    if (s.soil_pct * 10.0f >= cfg.soil_low_x10) return;
    pump_status_t p = pump_status();
    if (p.on || p.cooldown_left_ms > 0) return;
    pump_start(cfg.fallback_water_s, SRC_FALLBACK, 0);
}

static void supervise(uint32_t now)
{
    node_mode_t m = (now - last_gw_rx_ms > FALLBACK_TIMEOUT_MS) ? MODE_FALLBACK : MODE_NORMAL;
    if (m != mode) {
        mode = m;
        ESP_LOGW(TAG, "che do -> %s", m == MODE_FALLBACK ? "FALLBACK" : "NORMAL");
    }
    if (mode == MODE_FALLBACK) fallback_check();
}

static void comm_task(void *arg)
{
    esp_task_wdt_add(NULL);
    uint32_t next_tel_ms = now_ms() + TELEMETRY_FIRST_MS + settings_get_zone() * 1000UL;
    uint32_t last_sup_ms = 0;

    for (;;) {
        esp_task_wdt_reset();
        uint8_t zone = settings_get_zone();  // 0 = chưa đặt: không gửi/nhận

        rx_item_t item;
        if (xQueueReceive(rx_queue, &item, pdMS_TO_TICKS(50)) == pdTRUE && zone) handle_rx(zone, &item);

        pump_event_t ev;
        while (xQueueReceive(event_queue, &ev, 0) == pdTRUE)
            if (zone) send_pump_event(zone, &ev);

        uint32_t now = now_ms();
        if ((int32_t)(now - next_tel_ms) >= 0) {
            if (zone) send_telemetry(zone);
            else ESP_LOGW(TAG, "chua dat zone, go 'zone <1-3>'");
            next_tel_ms = now + TELEMETRY_PERIOD_MS;
        }
        if (now - last_sup_ms >= 1000) {
            last_sup_ms = now;
            supervise(now);
        }
        gpio_set_level(PIN_LED, mode == MODE_FALLBACK || (int32_t)(led_until_ms - now) > 0);
    }
}

// ---------- API ----------

void comm_init(void)
{
    gpio_config_t led = {.pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);

    rx_queue = xQueueCreate(8, sizeof(rx_item_t));
    event_queue = xQueueCreate(8, sizeof(pump_event_t));
    last_gw_rx_ms = now_ms();
    pump_set_event_cb(on_pump_event);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));

    esp_now_peer_info_t peer = {.channel = ESPNOW_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, GATEWAY_MAC, 6);
    if (esp_now_add_peer(&peer) != ESP_OK) ESP_LOGE(TAG, "them peer gateway loi (MAC trong app_config.h?)");
}

void comm_start_task(void)
{
    xTaskCreatePinnedToCore(comm_task, "comm", 4096, NULL, 3, &comm_task_handle, 1);
}

node_mode_t comm_mode(void)
{
    return mode;
}

uint32_t comm_gateway_age_ms(void)
{
    return now_ms() - last_gw_rx_ms;
}

void comm_build_telemetry(msg_telemetry_t *msg, uint8_t zone, uint16_t seq)
{
    sensor_data_t s = sensor_get();
    pump_status_t p = pump_status();

    msg->h = (msg_header_t){MSG_TELEMETRY, zone, seq};
    msg->soil_raw = s.soil_raw;
    msg->soil_x10 = (uint16_t)lroundf(s.soil_pct * 10.0f);
    msg->temp_x100 = isnan(s.temp_c) ? 0 : (int16_t)lroundf(s.temp_c * 100.0f);
    msg->hum_x100 = isnan(s.hum_pct) ? 0 : (uint16_t)lroundf(s.hum_pct * 100.0f);
    msg->pump_on = p.on ? 1 : 0;
    msg->node_mode = mode;
    msg->flags = s.flags | (p.cooldown_left_ms > 0 ? FLAG_COOLDOWN : 0);
    msg->uptime_s = now_ms() / 1000;
}
