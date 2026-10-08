// Node Zone: console Serial (hiệu chuẩn, test bơm, xem gói telemetry)
#include "console.h"

#include <Arduino.h>
#include <WiFi.h>

#include "comm.h"
#include "config.h"
#include "pump.h"
#include "sensor.h"
#include "settings.h"

static bool streaming = false;

static const char *source_name(PumpSource s) {
    switch (s) {
        case SRC_CMD:         return "cmd";
        case SRC_FALLBACK:    return "fallback";
        case SRC_SAFETY_STOP: return "safety";
        default:              return "?";
    }
}

static const char *ack_name(AckStatus s) {
    switch (s) {
        case ACK_OK:           return "OK";
        case ACK_REJ_COOLDOWN: return "REJ_COOLDOWN";
        case ACK_REJ_BUSY:     return "REJ_BUSY";
        case ACK_REJ_INVALID:  return "REJ_INVALID";
        default:               return "?";
    }
}

static void print_help() {
    Serial.println(
        "Lenh:\n"
        "  status            trang thai cam bien, bom, goi telemetry (hex)\n"
        "  stream on|off     in du lieu cam bien moi giay\n"
        "  mac               dia chi MAC (dien vao config.h)\n"
        "  cal show          xem moc hieu chuan\n"
        "  cal dry           lay gia tri hien tai lam moc KHO (cam bien de ngoai khong khi)\n"
        "  cal wet           lay gia tri hien tai lam moc UOT (nhung nuoc toi vach)\n"
        "  cal set <dry> <wet>\n"
        "  water <s>         bat bom s giay\n"
        "  stop              tat bom\n"
        "  cooldown clear    xoa cooldown (chi de thu nghiem)\n"
        "  set low <pct> | set max <s> | set fb <s>");
}

static void print_sensor_line() {
    SensorData s = sensor_get();
    PumpStatus p = pump_status();
    Serial.printf("raw=%u soil=%.1f%% temp=%.2fC hum=%.1f%% pump=%d flags=0x%02X\n",
                  s.soil_raw, s.soil_pct, s.temp_c, s.hum_pct, p.on, s.flags);
}

static void print_status() {
    SensorData s = sensor_get();
    PumpStatus p = pump_status();
    SoilCal c = sensor_get_cal();
    NodeSettings cfg = settings_get();

    Serial.printf("Zone %d | uptime %lus\n", ZONE_ID, millis() / 1000);
    Serial.printf("Soil: raw=%u pct=%.1f%% (cal dry=%u wet=%u)%s\n", s.soil_raw, s.soil_pct,
                  c.dry, c.wet, (s.flags & FLAG_ADC_ERR) ? " [ADC ERR]" : "");
    Serial.printf("SHT30: temp=%.2fC hum=%.1f%%%s\n", s.temp_c, s.hum_pct,
                  (s.flags & FLAG_SHT_ERR) ? " [SHT ERR]" : "");
    Serial.printf("Pump: %s", p.on ? "ON" : "OFF");
    if (p.on) Serial.printf(" src=%s seq=%u %lu/%us", source_name(p.source), p.cmd_seq,
                            p.elapsed_ms / 1000, p.target_s);
    Serial.printf(" | cooldown %lus\n", p.cooldown_left_ms / 1000);
    Serial.printf("Settings: low=%.1f%% max=%us fb=%us\n", cfg.soil_low_x10 / 10.0f,
                  cfg.pump_max_s, cfg.fallback_water_s);
    Serial.printf("Comm: mode=%s | gateway cuoi %lus truoc\n",
                  comm_mode() == MODE_FALLBACK ? "FALLBACK" : "NORMAL", comm_gateway_age_ms() / 1000);

    MsgTelemetry msg;
    comm_build_telemetry(&msg, 0);
    Serial.printf("Telemetry (%u B):", (unsigned)sizeof(msg));
    const uint8_t *b = (const uint8_t *)&msg;
    for (size_t i = 0; i < sizeof(msg); i++) Serial.printf(" %02X", b[i]);
    Serial.println();
}

static void update_setting(const char *key, float value) {
    NodeSettings s = settings_get();
    if (!strcmp(key, "low")) s.soil_low_x10 = (uint16_t)lroundf(value * 10.0f);
    else if (!strcmp(key, "max")) s.pump_max_s = (uint16_t)value;
    else if (!strcmp(key, "fb")) s.fallback_water_s = (uint16_t)value;
    else {
        Serial.println("Khoa khong hop le (low|max|fb)");
        return;
    }
    Serial.println(settings_set(s) ? "OK" : "Gia tri khong hop le");
}

static void handle_line(char *line) {
    char cmd[16] = {0}, arg1[16] = {0};
    unsigned a = 0, b = 0;
    int n = sscanf(line, "%15s %15s", cmd, arg1);
    if (n <= 0) return;

    if (!strcmp(cmd, "help")) {
        print_help();
    } else if (!strcmp(cmd, "status")) {
        print_status();
    } else if (!strcmp(cmd, "stream")) {
        streaming = !strcmp(arg1, "on");
    } else if (!strcmp(cmd, "mac")) {
        Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
    } else if (!strcmp(cmd, "cal")) {
        if (!strcmp(arg1, "dry")) {
            Serial.println(sensor_cal_capture_dry() ? "Da luu moc KHO" : "Loi: kho phai > uot + 500");
        } else if (!strcmp(arg1, "wet")) {
            Serial.println(sensor_cal_capture_wet() ? "Da luu moc UOT" : "Loi: uot phai < kho - 500");
        } else if (!strcmp(arg1, "set") && sscanf(line, "%*s %*s %u %u", &a, &b) == 2) {
            Serial.println(sensor_cal_set(a, b) ? "OK" : "Gia tri khong hop le");
        } else {
            SoilCal c = sensor_get_cal();
            Serial.printf("cal dry=%u wet=%u\n", c.dry, c.wet);
        }
    } else if (!strcmp(cmd, "water") && n == 2) {
        AckStatus st = pump_start((uint16_t)atoi(arg1), SRC_CMD, 0);
        Serial.printf("water: %s\n", ack_name(st));
    } else if (!strcmp(cmd, "stop")) {
        Serial.println(pump_stop() ? "Da tat bom" : "Bom dang tat");
    } else if (!strcmp(cmd, "cooldown") && !strcmp(arg1, "clear")) {
        pump_clear_cooldown();
        Serial.println("Da xoa cooldown");
    } else if (!strcmp(cmd, "set")) {
        char key[8] = {0};
        float value = 0;
        if (sscanf(line, "%*s %7s %f", key, &value) == 2) update_setting(key, value);
        else Serial.println("Cu phap: set low|max|fb <gia tri>");
    } else {
        Serial.println("Lenh khong hop le, go 'help'");
    }
}

static void console_task(void *) {
    char line[64];
    size_t len = 0;
    uint32_t last_stream_ms = 0;

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
        if (streaming && millis() - last_stream_ms >= SENSOR_PERIOD_MS) {
            last_stream_ms = millis();
            print_sensor_line();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void console_start_task() {
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 1, NULL, 1);
}
