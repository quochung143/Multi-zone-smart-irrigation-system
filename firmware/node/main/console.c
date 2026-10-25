// Node Zone: console REPL (hiệu chuẩn, test bơm, xem gói telemetry)
#include "console.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "comm.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pump.h"
#include "sensor.h"
#include "settings.h"

#define STREAM_DEFAULT_S 30

// 0 = tắt. Tự tắt để không phải gõ "stream off" trong lúc dữ liệu chen vào dòng nhập.
static volatile uint32_t stream_until_ms = 0;

static const char *ack_name(ack_status_t s)
{
    switch (s) {
    case ACK_OK:           return "OK";
    case ACK_REJ_COOLDOWN: return "REJ_COOLDOWN";
    case ACK_REJ_BUSY:     return "REJ_BUSY";
    case ACK_REJ_INVALID:  return "REJ_INVALID";
    default:               return "?";
    }
}

static void print_sensor_line(void)
{
    sensor_data_t s = sensor_get();
    pump_status_t p = pump_status();
    printf("raw=%u soil=%.1f%% temp=%.2fC hum=%.1f%% pump=%d flags=0x%02X\n",
           s.soil_raw, s.soil_pct, s.temp_c, s.hum_pct, p.on, s.flags);
}

static int cmd_status(int argc, char **argv)
{
    sensor_data_t s = sensor_get();
    pump_status_t p = pump_status();
    soil_cal_t c = sensor_get_cal();
    node_settings_t cfg = settings_get();
    uint8_t zone = settings_get_zone();

    printf("Zone %u%s | uptime %lus\n", zone, zone ? "" : " (CHUA DAT)", (unsigned long)(now_ms() / 1000));
    printf("Soil: raw=%u pct=%.1f%% (cal dry=%u wet=%u)%s\n", s.soil_raw, s.soil_pct, c.dry, c.wet,
           (s.flags & FLAG_ADC_ERR) ? " [ADC ERR]" : "");
    printf("DHT22: temp=%.1fC hum=%.1f%%", s.temp_c, s.hum_pct);
    if (s.flags & FLAG_AIR_ERR) printf(" [DHT ERR: %s]", esp_err_to_name(s.air_err));
    printf("\n");
    printf("Pump: %s", p.on ? "ON" : "OFF");
    if (p.on) printf(" src=%s seq=%u %lu/%us", pump_source_name(p.source), p.cmd_seq,
                     (unsigned long)(p.elapsed_ms / 1000), p.target_s);
    printf(" | cooldown %lus\n", (unsigned long)(p.cooldown_left_ms / 1000));
    printf("Settings: low=%.1f%% max=%us fb=%us\n", cfg.soil_low_x10 / 10.0f, cfg.pump_max_s, cfg.fallback_water_s);
    printf("Comm: mode=%s | kenh %u | gateway cuoi %lus truoc\n",
           comm_mode() == MODE_FALLBACK ? "FALLBACK" : "NORMAL", comm_channel(),
           (unsigned long)(comm_gateway_age_ms() / 1000));

    msg_telemetry_t msg;
    comm_build_telemetry(&msg, zone, 0);
    printf("Telemetry (%u B):", (unsigned)sizeof(msg));
    const uint8_t *b = (const uint8_t *)&msg;
    for (size_t i = 0; i < sizeof(msg); i++) printf(" %02X", b[i]);
    printf("\n");
    return 0;
}

static int cmd_stream(int argc, char **argv)
{
    if (argc != 2) return printf("Cu phap: stream on|off|<giay>\n"), 1;
    if (strcmp(argv[1], "off") == 0) {
        stream_until_ms = 0;
        return 0;
    }
    int secs = strcmp(argv[1], "on") == 0 ? STREAM_DEFAULT_S : atoi(argv[1]);
    if (secs <= 0) return printf("Cu phap: stream on|off|<giay>\n"), 1;
    stream_until_ms = now_ms() + secs * 1000UL;
    printf("In du lieu %d s roi tu tat ('stream off' de dung som)\n", secs);
    return 0;
}

static int cmd_mac(int argc, char **argv)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("app_config.h: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int cmd_zone(int argc, char **argv)
{
    if (argc == 2) {
        if (!settings_set_zone((uint8_t)atoi(argv[1]))) return printf("Zone phai la 1..%d\n", NUM_ZONES), 1;
    }
    printf("Zone: %u\n", settings_get_zone());
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "dry") == 0) {
        printf(sensor_cal_capture_dry() ? "Da luu moc KHO\n" : "Loi: kho phai > uot + %d\n", CAL_MIN_SPAN);
    } else if (argc >= 2 && strcmp(argv[1], "wet") == 0) {
        printf(sensor_cal_capture_wet() ? "Da luu moc UOT\n" : "Loi: uot phai < kho - %d\n", CAL_MIN_SPAN);
    } else if (argc == 4 && strcmp(argv[1], "set") == 0) {
        printf(sensor_cal_set((uint16_t)atoi(argv[2]), (uint16_t)atoi(argv[3])) ? "OK\n" : "Gia tri khong hop le\n");
    } else {
        soil_cal_t c = sensor_get_cal();
        printf("cal dry=%u wet=%u\n", c.dry, c.wet);
    }
    return 0;
}

static int cmd_water(int argc, char **argv)
{
    bool force = argc == 3 && strcmp(argv[2], "force") == 0;
    if (argc != 2 && !force) return printf("Cu phap: water <giay> [force]\n"), 1;
    printf("water: %s\n", ack_name(pump_start((uint16_t)atoi(argv[1]), force ? SRC_FORCE : SRC_CMD, 0)));
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    printf(pump_stop() ? "Da tat bom\n" : "Bom dang tat\n");
    return 0;
}

static int cmd_cooldown(int argc, char **argv)
{
    pump_clear_cooldown();
    printf("Da xoa cooldown\n");
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc != 3) return printf("Cu phap: set low|max|fb <gia tri>\n"), 1;
    node_settings_t s = settings_get();
    float value = strtof(argv[2], NULL);
    if (strcmp(argv[1], "low") == 0) s.soil_low_x10 = (uint16_t)lroundf(value * 10.0f);
    else if (strcmp(argv[1], "max") == 0) s.pump_max_s = (uint16_t)value;
    else if (strcmp(argv[1], "fb") == 0) s.fallback_water_s = (uint16_t)value;
    else return printf("Khoa khong hop le (low|max|fb)\n"), 1;
    printf(settings_set(&s) ? "OK\n" : "Gia tri khong hop le\n");
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    comm_request_scan();
    printf("Do kenh bang goi telemetry ke tiep (kenh hien tai %u), xem ket qua bang 'status'\n", comm_channel());
    return 0;
}

static void stream_task(void *arg)
{
    for (;;) {
        uint32_t until = stream_until_ms;
        if (until && (int32_t)(until - now_ms()) > 0) {
            print_sensor_line();
        } else if (until) {
            stream_until_ms = 0;
            printf("stream: da tat\n");
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

static void register_cmd(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {.command = name, .help = help, .func = func};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "node>";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    register_cmd("status", "Trang thai cam bien, bom, goi telemetry (hex)", cmd_status);
    register_cmd("stream", "stream on|off|<giay>: in du lieu cam bien moi giay, tu tat sau 30 s", cmd_stream);
    register_cmd("mac", "Dia chi MAC (dien vao app_config.h)", cmd_mac);
    register_cmd("zone", "zone [1-3]: xem/dat Zone ID (luu NVS)", cmd_zone);
    register_cmd("cal", "cal [dry|wet|set <dry> <wet>]: hieu chuan do am dat", cmd_cal);
    register_cmd("water", "water <giay> [force]: bat bom (force = bo qua thoi gian nghi)", cmd_water);
    register_cmd("stop", "Tat bom", cmd_stop);
    register_cmd("cooldown_clear", "Xoa cooldown (chi de thu nghiem)", cmd_cooldown);
    register_cmd("set", "set low|max|fb <gia tri>", cmd_set);
    register_cmd("scan", "Do lai kenh ESP-NOW cua gateway (kenh theo hotspot)", cmd_scan);

    xTaskCreatePinnedToCore(stream_task, "stream", 3072, NULL, 1, NULL, 1);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
