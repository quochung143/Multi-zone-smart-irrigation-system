// Gateway: JSON từ server -> lệnh ESP-NOW có ACK/retry (docs/DESIGN.md §4.3, §6.2)
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "gw.h"

#define SERVER_UART UART_NUM_0
#define SERIAL_LINE_MAX    256

typedef struct {
    uint8_t  zone;
    uint16_t seq;
    uint8_t  len;
    uint8_t  data[16];
} out_cmd_t;

static QueueHandle_t cmd_queue;

static volatile bool server_seen = false;
static volatile uint32_t last_server_ms = 0;
static volatile uint32_t epoch_base = 0;
static volatile uint32_t epoch_base_ms = 0;

bool server_alive(void)
{
    return server_seen && now_ms() - last_server_ms < SERVER_TIMEOUT_MS;
}

uint32_t server_epoch_now(void)
{
    if (epoch_base == 0) return 0;
    return epoch_base + (now_ms() - epoch_base_ms) / 1000;
}

static const char *ack_name(uint8_t s)
{
    switch (s) {
    case ACK_OK:           return "ok";
    case ACK_REJ_COOLDOWN: return "rej_cooldown";
    case ACK_REJ_BUSY:     return "rej_busy";
    case ACK_REJ_INVALID:  return "rej_invalid";
    default:               return "unknown";
    }
}

static void emit_ack(uint8_t zone, uint16_t seq, const char *status)
{
    char line[80];
    snprintf(line, sizeof(line), "{\"t\":\"ack\",\"zone\":%u,\"seq\":%u,\"status\":\"%s\"}", zone, seq, status);
    serial_out(line);
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static double json_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// Chuyển JSON thành out_cmd_t; false nếu không phải lệnh hợp lệ.
static bool build_command(const cJSON *doc, const char *t, out_cmd_t *out)
{
    int zone = json_int(doc, "zone", 0);
    int seq = json_int(doc, "seq", -1);
    if (zone < 1 || zone > NUM_ZONES || seq < 0 || seq > 0xFFFF) return false;
    out->zone = (uint8_t)zone;
    out->seq = (uint16_t)seq;

    if (strcmp(t, "cmd") == 0) {
        int dur = json_int(doc, "dur", 0);
        if (dur <= 0 || dur > 0xFFFF) return false;
        msg_cmd_water_t m = {{MSG_CMD_WATER, out->zone, out->seq}, (uint16_t)dur};
        memcpy(out->data, &m, sizeof(m));
        out->len = sizeof(m);
    } else if (strcmp(t, "stop") == 0) {
        msg_cmd_stop_t m = {{MSG_CMD_STOP, out->zone, out->seq}};
        memcpy(out->data, &m, sizeof(m));
        out->len = sizeof(m);
    } else if (strcmp(t, "cfg") == 0) {
        double low = json_num(doc, "low", -1);
        int max_s = json_int(doc, "max", 0);
        int fb = json_int(doc, "fb", 0);
        if (low < 0 || low > 100 || max_s <= 0 || fb <= 0) return false;
        msg_config_t m = {{MSG_CONFIG, out->zone, out->seq},
                          (uint16_t)lround(low * 10.0), (uint16_t)max_s, (uint16_t)fb};
        memcpy(out->data, &m, sizeof(m));
        out->len = sizeof(m);
    } else {
        return false;
    }
    return true;
}

static void handle_line(const char *line)
{
    if (line[0] != '{') return;  // bỏ dòng trống / không phải JSON

    cJSON *doc = cJSON_Parse(line);
    if (!doc) {
        serial_out("# json loi");
        return;
    }
    last_server_ms = now_ms();
    server_seen = true;

    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(doc, "t");
    const char *t = cJSON_IsString(jt) ? jt->valuestring : "";
    if (strcmp(t, "time") == 0) {
        double epoch = json_num(doc, "epoch", 0);
        if (epoch > 0) {
            epoch_base = (uint32_t)epoch;
            epoch_base_ms = now_ms();
        }
    } else if (strcmp(t, "ping") != 0) {
        out_cmd_t cmd;
        if (!build_command(doc, t, &cmd)) serial_out("# lenh khong hop le");
        else if (xQueueSend(cmd_queue, &cmd, 0) != pdTRUE) emit_ack(cmd.zone, cmd.seq, "busy");
    }
    cJSON_Delete(doc);
}

static void serial_rx_task(void *arg)
{
    static char line[SERIAL_LINE_MAX];
    size_t len = 0;
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(SERVER_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                handle_line(line);
                len = 0;
            } else if (len < SERIAL_LINE_MAX - 1) {
                line[len++] = c;
            }
        }
    }
}

// Gửi lệnh, chờ ACK đúng seq trong CMD_ACK_TIMEOUT_MS, thử tối đa CMD_RETRY_MAX lần.
static const char *deliver(const out_cmd_t *cmd)
{
    node_ack_t ack;
    while (xQueueReceive(g_ack_queue, &ack, 0) == pdTRUE) {}  // bỏ ACK cũ đến muộn

    for (int attempt = 0; attempt < CMD_RETRY_MAX; attempt++) {
        gw_send(cmd->zone, cmd->data, cmd->len);
        uint32_t deadline = now_ms() + CMD_ACK_TIMEOUT_MS;
        for (;;) {
            int32_t remaining = (int32_t)(deadline - now_ms());
            if (remaining <= 0) break;
            if (xQueueReceive(g_ack_queue, &ack, pdMS_TO_TICKS(remaining)) == pdTRUE &&
                ack.zone == cmd->zone && ack.ack_seq == cmd->seq)
                return ack_name(ack.status);
        }
    }
    return "timeout";
}

static void cmd_task(void *arg)
{
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        out_cmd_t cmd;
        if (xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        emit_ack(cmd.zone, cmd.seq, deliver(&cmd));
    }
}

void cmd_init(void)
{
    cmd_queue = xQueueCreate(8, sizeof(out_cmd_t));
}

void cmd_start_tasks(void)
{
    xTaskCreatePinnedToCore(serial_rx_task, "serial_rx", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(cmd_task, "cmd", 3072, NULL, 4, NULL, 1);
}
