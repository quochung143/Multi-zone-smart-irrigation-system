// Gateway: gói từ nút -> JSON, theo dõi online/offline, beacon (docs/DESIGN.md §4.3, §6.1)
#include <Arduino.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#include "config.h"
#include "gw.h"

QueueHandle_t g_ack_queue;

struct RxPacket {
    uint8_t zone;
    int8_t  rssi;
    uint8_t len;
    uint8_t data[32];
};

static QueueHandle_t rx_queue;
static volatile int8_t last_rssi[NUM_ZONES] = {0};
static volatile uint32_t last_seen_ms[NUM_ZONES] = {0};
static volatile bool seen[NUM_ZONES] = {false};
static uint16_t beacon_seq = 0;

static const char *source_name(uint8_t s) {
    return s == SRC_CMD ? "cmd" : s == SRC_FALLBACK ? "fallback" : "safety";
}

// Promiscuous chỉ để lấy RSSI của frame ESP-NOW (action frame, địa chỉ nguồn ở offset 10).
static void on_promisc(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int zone = zone_from_mac(pkt->payload + 10);
    if (zone) last_rssi[zone - 1] = pkt->rx_ctrl.rssi;
}

static void on_recv(const uint8_t *mac, const uint8_t *data, int len) {
    int zone = zone_from_mac(mac);
    if (!zone || len <= 0 || len > (int)sizeof(RxPacket::data)) return;
    RxPacket pkt;
    pkt.zone = (uint8_t)zone;
    pkt.rssi = last_rssi[zone - 1];
    pkt.len = (uint8_t)len;
    memcpy(pkt.data, data, len);
    xQueueSend(rx_queue, &pkt, 0);
}

static void emit_telemetry(uint8_t zone, const MsgTelemetry &m, int8_t rssi) {
    char line[256];
    snprintf(line, sizeof(line),
             "{\"t\":\"tel\",\"zone\":%u,\"seq\":%u,\"soil\":%.1f,\"soil_raw\":%u,\"temp\":%.2f,"
             "\"hum\":%.2f,\"pump\":%u,\"mode\":\"%s\",\"flags\":%u,\"up\":%lu,\"rssi\":%d}",
             zone, m.h.seq, m.soil_x10 / 10.0f, m.soil_raw, m.temp_x100 / 100.0f,
             m.hum_x100 / 100.0f, m.pump_on, m.node_mode == MODE_FALLBACK ? "fallback" : "normal",
             m.flags, (unsigned long)m.uptime_s, rssi);
    serial_out(line);
}

static void emit_pump_event(uint8_t zone, const MsgPumpEvent &m) {
    char line[128];
    snprintf(line, sizeof(line),
             "{\"t\":\"pump\",\"zone\":%u,\"on\":%u,\"src\":\"%s\",\"cmd_seq\":%u,\"dur\":%u}",
             zone, m.on, source_name(m.source), m.cmd_seq, m.duration_s);
    serial_out(line);
}

static void handle_packet(const RxPacket &pkt) {
    if (!msg_validate(pkt.data, pkt.len)) {
        serial_out("# goi sai dinh dang");
        return;
    }
    const MsgHeader *h = (const MsgHeader *)pkt.data;
    if (h->zone_id != pkt.zone) {
        serial_out("# zone_id khong khop MAC");
        return;
    }
    last_seen_ms[pkt.zone - 1] = millis();
    seen[pkt.zone - 1] = true;

    switch (h->type) {
        case MSG_TELEMETRY: {
            MsgTelemetry m;
            memcpy(&m, pkt.data, sizeof(m));
            emit_telemetry(pkt.zone, m, pkt.rssi);
            break;
        }
        case MSG_PUMP_EVENT: {
            MsgPumpEvent m;
            memcpy(&m, pkt.data, sizeof(m));
            emit_pump_event(pkt.zone, m);
            break;
        }
        case MSG_ACK: {
            MsgAck m;
            memcpy(&m, pkt.data, sizeof(m));
            NodeAck ack = {pkt.zone, m.ack_seq, m.status};
            xQueueSend(g_ack_queue, &ack, 0);  // cmdTask đối chiếu và báo server
            break;
        }
        default:
            break;
    }
}

static void uplink_task(void *) {
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        RxPacket pkt;
        if (xQueueReceive(rx_queue, &pkt, pdMS_TO_TICKS(1000)) == pdTRUE) handle_packet(pkt);
    }
}

static void send_beacons() {
    for (uint8_t zone = 1; zone <= NUM_ZONES; zone++) {
        MsgBeacon b = {{MSG_BEACON, zone, ++beacon_seq}, server_epoch_now()};
        gw_send(zone, &b, sizeof(b));
    }
}

// Mỗi giây: kiểm tra online/offline; mỗi BEACON_PERIOD_MS: beacon nếu server còn sống.
static void liveness_task(void *) {
    esp_task_wdt_add(NULL);
    bool reported_online[NUM_ZONES] = {false};
    uint32_t last_beacon_ms = 0;

    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = millis();

        for (int i = 0; i < NUM_ZONES; i++) {
            bool online = seen[i] && (now - last_seen_ms[i] < NODE_OFFLINE_MS);
            if (online != reported_online[i]) {
                reported_online[i] = online;
                char line[48];
                snprintf(line, sizeof(line), "{\"t\":\"node\",\"zone\":%d,\"online\":%s}", i + 1,
                         online ? "true" : "false");
                serial_out(line);
            }
        }

        // Chỉ beacon khi server còn sống: laptop tắt thì nút tự chuyển sang FALLBACK
        bool alive = server_alive();
        digitalWrite(PIN_LED, alive ? HIGH : LOW);
        if (alive && now - last_beacon_ms >= BEACON_PERIOD_MS) {
            last_beacon_ms = now;
            send_beacons();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void uplink_init() {
    rx_queue = xQueueCreate(16, sizeof(RxPacket));
    g_ack_queue = xQueueCreate(8, sizeof(NodeAck));

    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(on_promisc);
    esp_wifi_set_promiscuous(true);

    esp_now_register_recv_cb(on_recv);
}

void uplink_start_tasks() {
    xTaskCreatePinnedToCore(uplink_task, "uplink", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(liveness_task, "liveness", 3072, NULL, 1, NULL, 1);
}
