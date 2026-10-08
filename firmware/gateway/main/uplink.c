// Gateway: gói từ nút -> JSON, theo dõi online/offline, beacon (docs/DESIGN.md §4.3, §6.1)
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_now.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "gw.h"

QueueHandle_t g_ack_queue;

typedef struct {
    uint8_t zone;
    int8_t  rssi;
    uint8_t len;
    uint8_t data[MSG_MAX_LEN];
} rx_packet_t;

static QueueHandle_t rx_queue;
static volatile uint32_t last_seen_ms[NUM_ZONES];
static volatile bool seen[NUM_ZONES];
static uint16_t beacon_seq = 0;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    int zone = zone_from_mac(info->src_addr);
    if (!zone || len <= 0 || len > MSG_MAX_LEN) return;
    rx_packet_t pkt = {.zone = (uint8_t)zone, .rssi = (int8_t)info->rx_ctrl->rssi, .len = (uint8_t)len};
    memcpy(pkt.data, data, len);
    xQueueSend(rx_queue, &pkt, 0);
}

static void emit_telemetry(uint8_t zone, const msg_telemetry_t *m, int8_t rssi)
{
    char line[256];
    snprintf(line, sizeof(line),
             "{\"t\":\"tel\",\"zone\":%u,\"seq\":%u,\"soil\":%.1f,\"soil_raw\":%u,\"temp\":%.2f,"
             "\"hum\":%.2f,\"pump\":%u,\"mode\":\"%s\",\"flags\":%u,\"up\":%lu,\"rssi\":%d}",
             zone, m->h.seq, m->soil_x10 / 10.0f, m->soil_raw, m->temp_x100 / 100.0f,
             m->hum_x100 / 100.0f, m->pump_on, m->node_mode == MODE_FALLBACK ? "fallback" : "normal",
             m->flags, (unsigned long)m->uptime_s, rssi);
    serial_out(line);
}

static void emit_pump_event(uint8_t zone, const msg_pump_event_t *m)
{
    char line[128];
    snprintf(line, sizeof(line),
             "{\"t\":\"pump\",\"zone\":%u,\"on\":%u,\"src\":\"%s\",\"cmd_seq\":%u,\"dur\":%u}",
             zone, m->on, pump_source_name(m->source), m->cmd_seq, m->duration_s);
    serial_out(line);
}

static void handle_packet(const rx_packet_t *pkt)
{
    if (!msg_validate(pkt->data, pkt->len)) {
        serial_out("# goi sai dinh dang");
        return;
    }
    msg_header_t h;
    memcpy(&h, pkt->data, sizeof(h));
    if (h.zone_id != pkt->zone) {
        serial_out("# zone_id khong khop MAC");
        return;
    }
    last_seen_ms[pkt->zone - 1] = now_ms();
    seen[pkt->zone - 1] = true;

    switch (h.type) {
    case MSG_TELEMETRY: {
        msg_telemetry_t m;
        memcpy(&m, pkt->data, sizeof(m));
        emit_telemetry(pkt->zone, &m, pkt->rssi);
        break;
    }
    case MSG_PUMP_EVENT: {
        msg_pump_event_t m;
        memcpy(&m, pkt->data, sizeof(m));
        emit_pump_event(pkt->zone, &m);
        break;
    }
    case MSG_ACK: {
        msg_ack_t m;
        memcpy(&m, pkt->data, sizeof(m));
        node_ack_t ack = {pkt->zone, m.ack_seq, m.status};
        xQueueSend(g_ack_queue, &ack, 0);  // cmd_task đối chiếu và báo server
        break;
    }
    default:
        break;
    }
}

static void uplink_task(void *arg)
{
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        rx_packet_t pkt;
        if (xQueueReceive(rx_queue, &pkt, pdMS_TO_TICKS(1000)) == pdTRUE) handle_packet(&pkt);
    }
}

static void send_beacons(void)
{
    for (uint8_t zone = 1; zone <= NUM_ZONES; zone++) {
        msg_beacon_t b = {{MSG_BEACON, zone, ++beacon_seq}, server_epoch_now()};
        gw_send(zone, &b, sizeof(b));
    }
}

// Mỗi giây: kiểm tra online/offline; mỗi BEACON_PERIOD_MS: beacon nếu server còn sống.
static void liveness_task(void *arg)
{
    esp_task_wdt_add(NULL);
    bool reported_online[NUM_ZONES] = {false};
    uint32_t last_beacon_ms = 0;

    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = now_ms();

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
        gpio_set_level(PIN_LED, alive);
        if (alive && now - last_beacon_ms >= BEACON_PERIOD_MS) {
            last_beacon_ms = now;
            send_beacons();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void uplink_init(void)
{
    rx_queue = xQueueCreate(16, sizeof(rx_packet_t));
    g_ack_queue = xQueueCreate(8, sizeof(node_ack_t));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
}

void uplink_start_tasks(void)
{
    xTaskCreatePinnedToCore(uplink_task, "uplink", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(liveness_task, "liveness", 3072, NULL, 2, NULL, 1);
}
