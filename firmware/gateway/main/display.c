// Gateway: màn hình TFT 320x240 hiển thị trạng thái 3 zone, server và lệnh gần nhất
//
//  ┌──────────────────────────────────────┐
//  │ IRRIGATION GW                 MQTT OK │  thanh tiêu đề: trạng thái broker
//  │                             SERVER OK │                 và server
//  │ 12:43:08 MyHotspot 192.168.43.12 c6   │  giờ, hotspot, IP gateway, kênh Wi-Fi/ESP-NOW
//  ├──────────────────────────────────────┤
//  │ Z1 ONLINE           -49dBm   12s      │  3 khối zone, mỗi khối 3 dòng
//  │ Dat 43.2%  T 28.4C  H 71.2%           │
//  │ Bom OFF  NORMAL   NGHI                │
//  ├──────────────────────────────────────┤
//  │ Lenh: Z1 #47792 -> ok (5s)            │  kết quả lệnh gần nhất
//  └──────────────────────────────────────┘
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw.h"
#include "lcd.h"

#define TZ_OFFSET_S  (7 * 3600)  // giờ Việt Nam (UTC+7)
#define HEADER_H     32
#define ZONE_Y0      58
#define ZONE_H       54
#define FOOTER_Y     (LCD_H - FONT_ROW)
#define FONT_ROW     16
#define CHAR_W       8
#define MAX_SEGS     32
#define SEG_LINK     (4 + NUM_ZONES * 8)  // sau ô footer

static const char *TAG = "display";

// Ô chữ đã vẽ: chỉ vẽ lại khi nội dung hoặc màu đổi
typedef struct {
    char     text[44];
    uint16_t fg;
    bool     drawn;
} seg_cache_t;
static seg_cache_t cache[MAX_SEGS];

static void seg(int id, int col, int y, const char *text, uint16_t fg, uint16_t bg, int scale, int width)
{
    seg_cache_t *c = &cache[id];
    if (c->drawn && c->fg == fg && strcmp(c->text, text) == 0) return;
    lcd_text(col * CHAR_W, y, text, fg, bg, scale, width);
    strlcpy(c->text, text, sizeof(c->text));
    c->fg = fg;
    c->drawn = true;
}

static void fmt_age(char *out, size_t n, uint32_t s)
{
    if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, n, "%lup", (unsigned long)(s / 60));
    else snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

static void draw_static(void)
{
    lcd_fill(0, 0, LCD_W, HEADER_H, COLOR_BLUE);
    lcd_text(8, 0, "IRRIGATION GW", COLOR_WHITE, COLOR_BLUE, 2, 0);
    for (int i = 0; i <= NUM_ZONES; i++) {
        int y = (i == NUM_ZONES) ? FOOTER_Y - 3 : ZONE_Y0 - 4 + i * ZONE_H;
        lcd_fill(0, y, LCD_W, 1, COLOR_DARK);
    }
}

static void draw_header(void)
{
    net_status_t n;
    net_get_status(&n);
    bool alive = server_alive();
    const char *link = n.mqtt ? "MQTT OK" : n.wifi ? "MQTT --" : "WIFI --";
    seg(SEG_LINK, 30, 0, link, n.mqtt ? COLOR_GREEN : COLOR_RED, COLOR_BLUE, 1, 10);
    seg(0, 30, 16, alive ? "SERVER OK" : "SERVER --", alive ? COLOR_GREEN : COLOR_RED, COLOR_BLUE, 1, 10);

    char line[64];
    uint32_t epoch = server_epoch_now();
    if (epoch) {
        time_t t = (time_t)epoch + TZ_OFFSET_S;
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(line, sizeof(line), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(line, sizeof(line), "--:--:--");
    }
    seg(1, 0, 36, line, COLOR_WHITE, COLOR_BLACK, 1, 9);

    uint16_t color = COLOR_GREY;
    if (!n.ssid[0]) {
        snprintf(line, sizeof(line), "WiFi chua cau hinh (console)");
        color = COLOR_ORANGE;
    } else if (n.wifi) {
        snprintf(line, sizeof(line), "%.10s %s c%u", n.ssid, n.ip, n.channel);
    } else {
        snprintf(line, sizeof(line), "Dang noi %.20s...", n.ssid);
        color = COLOR_YELLOW;
    }
    seg(2, 9, 36, line, color, COLOR_BLACK, 1, 31);
}

static void draw_zone(int zone)
{
    zone_view_t v;
    uplink_zone_view(zone, &v);
    int y = ZONE_Y0 + (zone - 1) * ZONE_H;
    int id = 3 + (zone - 1) * 8;
    char s[64], a[12];

    snprintf(s, sizeof(s), "Z%d", zone);
    seg(id, 0, y, s, COLOR_CYAN, COLOR_BLACK, 1, 3);

    const char *status;
    uint16_t color;
    if (!v.configured) status = "CHUA CAU HINH MAC", color = COLOR_DARK;
    else if (!v.has_tel && !v.online) status = "CHO DU LIEU...", color = COLOR_YELLOW;
    else if (v.online) status = "ONLINE", color = COLOR_GREEN;
    else status = "OFFLINE", color = COLOR_RED;
    seg(id + 1, 3, y, status, color, COLOR_BLACK, 1, 17);

    if (!v.configured || !v.has_tel) {
        seg(id + 2, 20, y, "", COLOR_WHITE, COLOR_BLACK, 1, 20);
        seg(id + 3, 0, y + FONT_ROW, "", COLOR_WHITE, COLOR_BLACK, 1, 40);
        seg(id + 4, 0, y + 2 * FONT_ROW, "", COLOR_WHITE, COLOR_BLACK, 1, 8);
        seg(id + 5, 8, y + 2 * FONT_ROW, "", COLOR_WHITE, COLOR_BLACK, 1, 9);
        seg(id + 6, 17, y + 2 * FONT_ROW, "", COLOR_WHITE, COLOR_BLACK, 1, 5);
        return;
    }

    const msg_telemetry_t *t = &v.tel;
    fmt_age(a, sizeof(a), v.age_s);
    snprintf(s, sizeof(s), "%4ddBm  %s truoc", v.rssi, a);
    seg(id + 2, 20, y, s, COLOR_GREY, COLOR_BLACK, 1, 20);

    char soil[16], air[24];
    if (t->flags & FLAG_ADC_ERR) snprintf(soil, sizeof(soil), "Dat LOI  ");
    else snprintf(soil, sizeof(soil), "Dat %4.1f%%", t->soil_x10 / 10.0f);
    if (t->flags & FLAG_AIR_ERR) snprintf(air, sizeof(air), "DHT22 LOI");
    else snprintf(air, sizeof(air), "T %4.1fC  H %4.1f%%", t->temp_x100 / 100.0f, t->hum_x100 / 100.0f);
    snprintf(s, sizeof(s), "%s  %s", soil, air);
    seg(id + 3, 0, y + FONT_ROW, s, (t->flags & (FLAG_ADC_ERR | FLAG_AIR_ERR)) ? COLOR_ORANGE : COLOR_WHITE,
        COLOR_BLACK, 1, 40);

    seg(id + 4, 0, y + 2 * FONT_ROW, v.pump_on ? "Bom ON" : "Bom OFF", v.pump_on ? COLOR_GREEN : COLOR_GREY,
        COLOR_BLACK, 1, 8);
    bool fb = t->node_mode == MODE_FALLBACK;
    seg(id + 5, 8, y + 2 * FONT_ROW, fb ? "FALLBACK" : "NORMAL", fb ? COLOR_ORANGE : COLOR_WHITE, COLOR_BLACK, 1, 9);
    seg(id + 6, 17, y + 2 * FONT_ROW, (t->flags & FLAG_COOLDOWN) ? "NGHI" : "", COLOR_YELLOW, COLOR_BLACK, 1, 5);
}

static void draw_footer(void)
{
    last_ack_t a;
    cmd_last_ack(&a);
    char s[64], age[12];
    uint16_t color = COLOR_GREY;
    if (a.valid) {
        fmt_age(age, sizeof(age), (now_ms() - a.at_ms) / 1000);
        snprintf(s, sizeof(s), "Lenh: Z%u #%u -> %s (%s)", a.zone, a.seq, a.status, age);
        color = strcmp(a.status, "ok") == 0 ? COLOR_GREEN : COLOR_ORANGE;
    } else {
        snprintf(s, sizeof(s), "Lenh: chua co");
    }
    seg(3 + NUM_ZONES * 8, 0, FOOTER_Y, s, color, COLOR_BLACK, 1, 40);
}

static void display_task(void *arg)
{
    if (lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "khoi tao man hinh loi");
        vTaskDelete(NULL);
    }
    draw_static();
    for (;;) {
        draw_header();
        for (int z = 1; z <= NUM_ZONES; z++) draw_zone(z);
        draw_footer();
        vTaskDelay(pdMS_TO_TICKS(LCD_REFRESH_MS));
    }
}

void display_start(void)
{
#if LCD_ENABLED
    _Static_assert(SEG_LINK < MAX_SEGS, "MAX_SEGS");
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 1, NULL, 0);
#endif
}
