// Gateway: JSON từ server -> lệnh ESP-NOW có ACK/retry (docs/DESIGN.md §4.3, §6.2)
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <math.h>

#include "config.h"
#include "gw.h"

struct OutCmd {
    uint8_t  zone;
    uint16_t seq;
    uint8_t  len;
    uint8_t  data[16];
};

static QueueHandle_t cmd_queue;

static volatile bool server_seen = false;
static volatile uint32_t last_server_ms = 0;
static volatile uint32_t epoch_base = 0;
static volatile uint32_t epoch_base_ms = 0;

bool server_alive() {
    return server_seen && millis() - last_server_ms < SERVER_TIMEOUT_MS;
}

uint32_t server_epoch_now() {
    if (epoch_base == 0) return 0;
    return epoch_base + (millis() - epoch_base_ms) / 1000;
}

static const char *ack_name(uint8_t s) {
    switch (s) {
        case ACK_OK:           return "ok";
        case ACK_REJ_COOLDOWN: return "rej_cooldown";
        case ACK_REJ_BUSY:     return "rej_busy";
        case ACK_REJ_INVALID:  return "rej_invalid";
        default:               return "unknown";
    }
}

static void emit_ack(uint8_t zone, uint16_t seq, const char *status) {
    char line[80];
    snprintf(line, sizeof(line), "{\"t\":\"ack\",\"zone\":%u,\"seq\":%u,\"status\":\"%s\"}", zone,
             seq, status);
    serial_out(line);
}

// Chuyển 1 dòng JSON thành OutCmd; false nếu không phải lệnh hợp lệ.
static bool build_command(JsonDocument &doc, const char *t, OutCmd &out) {
    int zone = doc["zone"] | 0;
    int seq = doc["seq"] | -1;
    if (zone < 1 || zone > NUM_ZONES || seq < 0 || seq > 0xFFFF) return false;
    out.zone = (uint8_t)zone;
    out.seq = (uint16_t)seq;

    if (!strcmp(t, "cmd")) {
        int dur = doc["dur"] | 0;
        if (dur <= 0 || dur > 0xFFFF) return false;
        MsgCmdWater m = {{MSG_CMD_WATER, out.zone, out.seq}, (uint16_t)dur};
        memcpy(out.data, &m, sizeof(m));
        out.len = sizeof(m);
    } else if (!strcmp(t, "stop")) {
        MsgCmdStop m = {{MSG_CMD_STOP, out.zone, out.seq}};
        memcpy(out.data, &m, sizeof(m));
        out.len = sizeof(m);
    } else if (!strcmp(t, "cfg")) {
        float low = doc["low"] | -1.0f;
        int max_s = doc["max"] | 0;
        int fb = doc["fb"] | 0;
        if (low < 0 || low > 100 || max_s <= 0 || fb <= 0) return false;
        MsgConfig m = {{MSG_CONFIG, out.zone, out.seq},
                       (uint16_t)lroundf(low * 10.0f), (uint16_t)max_s, (uint16_t)fb};
        memcpy(out.data, &m, sizeof(m));
        out.len = sizeof(m);
    } else {
        return false;
    }
    return true;
}

static void handle_line(const char *line) {
    if (line[0] == '\0' || line[0] == '#') return;

    JsonDocument doc;
    if (deserializeJson(doc, line)) {
        serial_out("# json loi");
        return;
    }
    last_server_ms = millis();
    server_seen = true;

    const char *t = doc["t"] | "";
    if (!strcmp(t, "time")) {
        uint32_t epoch = doc["epoch"] | 0;
        if (epoch) {
            epoch_base = epoch;
            epoch_base_ms = millis();
        }
    } else if (!strcmp(t, "ping")) {
        // chỉ để báo server còn sống
    } else {
        OutCmd cmd;
        if (!build_command(doc, t, cmd)) {
            serial_out("# lenh khong hop le");
            return;
        }
        if (xQueueSend(cmd_queue, &cmd, 0) != pdTRUE) emit_ack(cmd.zone, cmd.seq, "busy");
    }
}

static void serial_rx_task(void *) {
    char line[256];
    size_t len = 0;
    for (;;) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                handle_line(line);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Gửi lệnh, chờ ACK đúng seq trong CMD_ACK_TIMEOUT_MS, thử tối đa CMD_RETRY_MAX lần.
static const char *deliver(const OutCmd &cmd) {
    NodeAck ack;
    while (xQueueReceive(g_ack_queue, &ack, 0) == pdTRUE) {}  // bỏ ACK cũ đến muộn

    for (int attempt = 0; attempt < CMD_RETRY_MAX; attempt++) {
        gw_send(cmd.zone, cmd.data, cmd.len);
        uint32_t deadline = millis() + CMD_ACK_TIMEOUT_MS;
        for (;;) {
            int32_t remaining = (int32_t)(deadline - millis());
            if (remaining <= 0) break;
            if (xQueueReceive(g_ack_queue, &ack, pdMS_TO_TICKS(remaining)) == pdTRUE &&
                ack.zone == cmd.zone && ack.ack_seq == cmd.seq)
                return ack_name(ack.status);
        }
    }
    return "timeout";
}

static void cmd_task(void *) {
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        OutCmd cmd;
        if (xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        emit_ack(cmd.zone, cmd.seq, deliver(cmd));
    }
}

void cmd_init() {
    cmd_queue = xQueueCreate(8, sizeof(OutCmd));
}

void cmd_start_tasks() {
    xTaskCreatePinnedToCore(serial_rx_task, "serial_rx", 6144, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(cmd_task, "cmd", 3072, NULL, 3, NULL, 1);
}
