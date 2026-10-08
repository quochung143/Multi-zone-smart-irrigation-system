// Node Zone: ESP-NOW + chế độ fallback (docs/DESIGN.md §4.2, docs/PROTOCOL.md)
#include "comm.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <math.h>

#include "config.h"
#include "pump.h"
#include "sensor.h"
#include "settings.h"

#ifndef ZONE_ID
#error "ZONE_ID phải được định nghĩa qua build_flags (-DZONE_ID=n)"
#endif

struct RxItem {
    uint8_t len;
    uint8_t data[32];
};

static QueueHandle_t rx_queue;
static QueueHandle_t event_queue;
static TaskHandle_t comm_task_handle = nullptr;

static volatile uint32_t last_gw_rx_ms = 0;
static volatile NodeMode mode = MODE_NORMAL;
static uint16_t tx_seq = 0;
static uint32_t led_until_ms = 0;

// Chống thực thi lặp khi gateway gửi lại lệnh
static bool has_last_cmd = false;
static uint16_t last_cmd_seq = 0;
static AckStatus last_ack = ACK_OK;

static const char *source_name(PumpSource s) {
    return s == SRC_CMD ? "cmd" : s == SRC_FALLBACK ? "fallback" : "safety";
}

// ---------- Callback ESP-NOW (chạy trong task Wi-Fi, chỉ copy dữ liệu) ----------

static void on_recv(const uint8_t *mac, const uint8_t *data, int len) {
    if (memcmp(mac, GATEWAY_MAC, 6) != 0) return;
    if (len <= 0 || len > (int)sizeof(RxItem::data) || !msg_validate(data, len)) return;
    RxItem item;
    item.len = (uint8_t)len;
    memcpy(item.data, data, len);
    xQueueSend(rx_queue, &item, 0);
}

static void on_sent(const uint8_t *, esp_now_send_status_t status) {
    if (comm_task_handle)
        xTaskNotify(comm_task_handle, status == ESP_NOW_SEND_SUCCESS ? 1 : 2, eSetValueWithOverwrite);
}

static void on_pump_event(const PumpEvent &ev) {
    if (ev.on) Serial.printf("[pump] ON src=%s seq=%u\n", source_name(ev.source), ev.cmd_seq);
    else Serial.printf("[pump] OFF src=%s seq=%u ran=%us\n", source_name(ev.source), ev.cmd_seq,
                       ev.duration_s);
    xQueueSend(event_queue, &ev, 0);
}

// ---------- Gửi (chỉ gọi từ commTask) ----------

// Gửi và chờ ACK lớp MAC; lỗi thì gửi lại tối đa TX_RETRY_MAX lần.
static bool send_reliable(const void *msg, size_t len) {
    uint32_t result;
    for (int attempt = 0; attempt <= TX_RETRY_MAX; attempt++) {
        xTaskNotifyWait(0, ULONG_MAX, &result, 0);  // xóa thông báo cũ
        if (esp_now_send(GATEWAY_MAC, (const uint8_t *)msg, len) != ESP_OK) continue;
        if (xTaskNotifyWait(0, ULONG_MAX, &result, pdMS_TO_TICKS(TX_WAIT_MS)) == pdTRUE && result == 1)
            return true;
    }
    return false;
}

static void send_telemetry() {
    MsgTelemetry msg;
    comm_build_telemetry(&msg, ++tx_seq);
    if (!send_reliable(&msg, sizeof(msg))) Serial.println("[comm] telemetry gui loi");
}

static void send_ack(uint16_t cmd_seq, AckStatus status) {
    MsgAck msg = {{MSG_ACK, ZONE_ID, ++tx_seq}, cmd_seq, status};
    send_reliable(&msg, sizeof(msg));
}

static void send_pump_event(const PumpEvent &ev) {
    MsgPumpEvent msg = {{MSG_PUMP_EVENT, ZONE_ID, ++tx_seq},
                        (uint8_t)ev.on, ev.source, ev.cmd_seq, ev.duration_s};
    send_reliable(&msg, sizeof(msg));
}

// ---------- Xử lý gói nhận ----------

static AckStatus execute_command(const RxItem &item) {
    switch (item.data[0]) {
        case MSG_CMD_WATER: {
            MsgCmdWater cmd;
            memcpy(&cmd, item.data, sizeof(cmd));
            return pump_start(cmd.duration_s, SRC_CMD, cmd.h.seq);
        }
        case MSG_CMD_STOP:
            pump_stop();
            return ACK_OK;
        case MSG_CONFIG: {
            MsgConfig cfg;
            memcpy(&cfg, item.data, sizeof(cfg));
            NodeSettings s = {cfg.soil_low_x10, cfg.pump_max_s, cfg.fallback_water_s};
            return settings_set(s) ? ACK_OK : ACK_REJ_INVALID;
        }
        default:
            return ACK_REJ_INVALID;
    }
}

static void handle_rx(const RxItem &item) {
    const MsgHeader *h = (const MsgHeader *)item.data;
    if (h->zone_id != ZONE_ID) return;

    last_gw_rx_ms = millis();
    led_until_ms = last_gw_rx_ms + 50;

    switch (h->type) {
        case MSG_BEACON:
            break;  // chỉ dùng làm heartbeat
        case MSG_CMD_WATER:
        case MSG_CMD_STOP:
        case MSG_CONFIG: {
            uint16_t seq = h->seq;
            if (has_last_cmd && seq == last_cmd_seq) {
                send_ack(seq, last_ack);  // lệnh gửi lại: chỉ xác nhận lại, không thực thi
                return;
            }
            AckStatus status = execute_command(item);
            has_last_cmd = true;
            last_cmd_seq = seq;
            last_ack = status;
            Serial.printf("[comm] cmd type=%u seq=%u -> ack %u\n", h->type, seq, status);
            send_ack(seq, status);
            break;
        }
        default:
            break;  // gói uplink không dành cho nút
    }
}

// ---------- Fallback ----------

static void fallback_check() {
    SensorData s = sensor_get();
    if (!s.ready || (s.flags & FLAG_ADC_ERR)) return;  // cảm biến lỗi thì không tự tưới
    NodeSettings cfg = settings_get();
    if (s.soil_pct * 10.0f >= cfg.soil_low_x10) return;
    PumpStatus p = pump_status();
    if (p.on || p.cooldown_left_ms > 0) return;
    pump_start(cfg.fallback_water_s, SRC_FALLBACK, 0);
}

static void supervise(uint32_t now) {
    NodeMode m = (now - last_gw_rx_ms > FALLBACK_TIMEOUT_MS) ? MODE_FALLBACK : MODE_NORMAL;
    if (m != mode) {
        mode = m;
        Serial.printf("[comm] mode -> %s\n", m == MODE_FALLBACK ? "FALLBACK" : "NORMAL");
    }
    if (mode == MODE_FALLBACK) fallback_check();
}

static void comm_task(void *) {
    esp_task_wdt_add(NULL);
    uint32_t next_tel_ms = millis() + TELEMETRY_FIRST_MS + ZONE_ID * 1000UL;
    uint32_t last_sup_ms = 0;

    for (;;) {
        esp_task_wdt_reset();

        RxItem item;
        if (xQueueReceive(rx_queue, &item, pdMS_TO_TICKS(50)) == pdTRUE) handle_rx(item);

        PumpEvent ev;
        while (xQueueReceive(event_queue, &ev, 0) == pdTRUE) send_pump_event(ev);

        uint32_t now = millis();
        if ((int32_t)(now - next_tel_ms) >= 0) {
            send_telemetry();
            next_tel_ms = now + TELEMETRY_PERIOD_MS;
        }
        if (now - last_sup_ms >= 1000) {
            last_sup_ms = now;
            supervise(now);
        }
        digitalWrite(PIN_LED, (mode == MODE_FALLBACK || (int32_t)(led_until_ms - now) > 0) ? HIGH : LOW);
    }
}

// ---------- API ----------

void comm_init() {
    pinMode(PIN_LED, OUTPUT);
    rx_queue = xQueueCreate(8, sizeof(RxItem));
    event_queue = xQueueCreate(8, sizeof(PumpEvent));
    last_gw_rx_ms = millis();
    pump_set_event_cb(on_pump_event);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[comm] ESP-NOW init loi, khoi dong lai");
        delay(1000);
        ESP.restart();
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, GATEWAY_MAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("[comm] add peer gateway loi");
}

void comm_start_task() {
    xTaskCreatePinnedToCore(comm_task, "comm", 4096, NULL, 2, &comm_task_handle, 1);
}

NodeMode comm_mode() {
    return mode;
}

uint32_t comm_gateway_age_ms() {
    return millis() - last_gw_rx_ms;
}

void comm_build_telemetry(MsgTelemetry *msg, uint16_t seq) {
    SensorData s = sensor_get();
    PumpStatus p = pump_status();

    msg->h = {MSG_TELEMETRY, ZONE_ID, seq};
    msg->soil_raw = s.soil_raw;
    msg->soil_x10 = (uint16_t)lroundf(s.soil_pct * 10.0f);
    msg->temp_x100 = isnan(s.temp_c) ? 0 : (int16_t)lroundf(s.temp_c * 100.0f);
    msg->hum_x100 = isnan(s.hum_pct) ? 0 : (uint16_t)lroundf(s.hum_pct * 100.0f);
    msg->pump_on = p.on ? 1 : 0;
    msg->node_mode = mode;
    msg->flags = s.flags | (p.cooldown_left_ms > 0 ? FLAG_COOLDOWN : 0);
    msg->uptime_s = millis() / 1000;
}
