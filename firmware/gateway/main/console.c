// Gateway: console REPL trên USB (UART0) để cấu hình Wi-Fi hotspot / broker MQTT và xem trạng thái
// Chuỗi có dấu cách thì đặt trong ngoặc kép: wifi "Hotspot Cua Toi" matkhau123
// Lệnh *_json và wifi_scan cho phần mềm cấu hình (server/tools/gw_config.py): in đúng 1 dòng
// "@<json>" để tách khỏi log ESP_LOG chạy xen trên cùng UART.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw.h"

static int saved(const net_config_t *c)
{
    if (!net_config_set(c)) return printf("Gia tri khong hop le\n"), 1;
    printf("Da luu. Go 'restart' de ap dung.\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    net_status_t n;
    net_get_status(&n);
    net_config_t c = net_config_get();
    printf("Wi-Fi: %s", c.ssid[0] ? c.ssid : "(chua cau hinh)");
    if (n.wifi) printf(" | IP %s | kenh %u", n.ip, n.channel);
    else if (c.ssid[0]) printf(" | dang ket noi...");
    printf("\nBroker: %s | MQTT %s | server %s\n", n.broker[0] ? n.broker : "-", n.mqtt ? "OK" : "--",
           server_alive() ? "OK" : "--");
    for (int z = 1; z <= NUM_ZONES; z++) {
        zone_view_t v;
        uplink_zone_view(z, &v);
        if (!v.configured) {
            printf("Z%d: chua dien MAC\n", z);
            continue;
        }
        printf("Z%d: %s", z, v.online ? "ONLINE" : "offline");
        if (v.has_tel) printf(" | dat %.1f%% | %ddBm | %lus truoc", v.tel.soil_x10 / 10.0f, v.rssi,
                              (unsigned long)v.age_s);
        printf(" | rx=%lu lost=%lu\n", (unsigned long)v.rx, (unsigned long)v.lost);
    }
    return 0;
}

static int cmd_net(int argc, char **argv)
{
    net_config_t c = net_config_get();
    printf("ssid=%s pass=%s\n", c.ssid, c.pass[0] ? "***" : "(trong)");
    printf("broker=%s port=%u ip_du_phong=%s\n", c.host[0] ? c.host : "(trong)", c.port, c.ip[0] ? c.ip : "(trong)");
    printf("mqtt_user=%s mqtt_pass=%s\n", c.user, c.mpass[0] ? "***" : "(trong)");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) return printf("Cu phap: wifi <ssid> [mat khau]\n"), 1;
    net_config_t c = net_config_get();
    strlcpy(c.ssid, argv[1], sizeof(c.ssid));
    strlcpy(c.pass, argc == 3 ? argv[2] : "", sizeof(c.pass));
    return saved(&c);
}

static int cmd_broker(int argc, char **argv)
{
    if (argc < 2 || argc > 3) return printf("Cu phap: broker <ten-laptop>.local|<ip> [port] | broker wss://<ten-mien>\n"), 1;
    net_config_t c = net_config_get();
    strlcpy(c.host, argv[1], sizeof(c.host));
    if (argc == 3) c.port = (uint16_t)atoi(argv[2]);
    return saved(&c);
}

static int cmd_broker_ip(int argc, char **argv)
{
    if (argc != 2) return printf("Cu phap: broker_ip <ip> (dung khi mDNS khong tim thay laptop; '-' de xoa)\n"), 1;
    net_config_t c = net_config_get();
    strlcpy(c.ip, strcmp(argv[1], "-") == 0 ? "" : argv[1], sizeof(c.ip));
    return saved(&c);
}

static int cmd_mqtt_user(int argc, char **argv)
{
    if (argc != 3) return printf("Cu phap: mqtt_user <user> <mat khau>\n"), 1;
    net_config_t c = net_config_get();
    strlcpy(c.user, argv[1], sizeof(c.user));
    strlcpy(c.mpass, argv[2], sizeof(c.mpass));
    return saved(&c);
}

static int cmd_mac(int argc, char **argv)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("app_config.h GATEWAY_MAC: {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

// ---------- lệnh cho phần mềm cấu hình: 1 dòng "@<json>" ----------
// Dựng cả dòng trong bộ đệm rồi in một lần: log của task khác không chen được vào giữa JSON.

#define JSON_BUF 2048
static char jbuf[JSON_BUF];
static size_t jlen;

static void jp(const char *fmt, ...)
{
    if (jlen >= JSON_BUF) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(jbuf + jlen, JSON_BUF - jlen, fmt, ap);
    va_end(ap);
    if (n > 0) jlen += (size_t)n;
}

static void jstr(const char *s)
{
    jp("\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') jp("\\%c", ch);
        else if (ch < 0x20) jp("\\u%04x", ch);
        else jp("%c", ch);  // UTF-8 giữ nguyên
    }
    jp("\"");
}

static void jkv(const char *key, const char *val)
{
    jp("\"%s\":", key);
    jstr(val);
    jp(",");
}

static int jflush(void)
{
    bool full = jlen >= JSON_BUF - 2;
    printf("%s\n", full ? "@{\"error\":\"overflow\"}" : jbuf);
    fflush(stdout);
    jlen = 0;
    return full;
}

// Mật khẩu không bao giờ in ra: chỉ báo đã đặt hay chưa
static int cmd_net_json(int argc, char **argv)
{
    net_config_t c = net_config_get();
    jp("@{");
    jkv("ssid", c.ssid);
    jp("\"has_pass\":%s,", c.pass[0] ? "true" : "false");
    jkv("host", c.host);
    jp("\"port\":%u,", c.port);
    jkv("ip", c.ip);
    jkv("user", c.user);
    jp("\"has_mpass\":%s}", c.mpass[0] ? "true" : "false");
    return jflush();
}

static int cmd_status_json(int argc, char **argv)
{
    net_status_t n;
    net_get_status(&n);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_s[18];
    snprintf(mac_s, sizeof(mac_s), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    jp("@{\"wifi\":%s,", n.wifi ? "true" : "false");
    jkv("ssid", net_config_get().ssid);
    jkv("ip", n.ip);
    jp("\"channel\":%u,\"mqtt\":%s,\"server\":%s,", n.channel, n.mqtt ? "true" : "false",
       server_alive() ? "true" : "false");
    jkv("broker", n.broker);
    jkv("mac", mac_s);
    jp("\"zones\":[");
    for (int z = 1; z <= NUM_ZONES; z++) {
        zone_view_t v;
        uplink_zone_view(z, &v);
        jp("%s{\"zone\":%d,\"configured\":%s,\"online\":%s", z > 1 ? "," : "", z, v.configured ? "true" : "false",
           v.online ? "true" : "false");
        if (v.has_tel) jp(",\"soil\":%.1f,\"rssi\":%d,\"age_s\":%lu", v.tel.soil_x10 / 10.0f, v.rssi,
                          (unsigned long)v.age_s);
        jp(",\"pump\":%s}", v.pump_on ? "true" : "false");
    }
    jp("]}");
    return jflush();
}

// Quét Wi-Fi quanh gateway (chặn ~2-3 s). Chưa nối AP thì trả lại kênh ESP-NOW sau khi quét.
static wifi_ap_record_t scan_aps[20];  // tĩnh: ~1.6 KB, không đặt trên stack task console

static int cmd_wifi_scan(int argc, char **argv)
{
    net_status_t n;
    net_get_status(&n);
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        jp("@{\"error\":\"%s\"}", esp_err_to_name(err));  // vd. đang nối Wi-Fi: thử lại sau vài giây
        jflush();
        return 1;
    }
    uint16_t count = sizeof(scan_aps) / sizeof(scan_aps[0]);
    esp_wifi_scan_get_ap_records(&count, scan_aps);
    if (!n.wifi) esp_wifi_set_channel(n.channel, WIFI_SECOND_CHAN_NONE);
    jp("@[");
    for (int i = 0; i < count; i++) {
        jp("%s{", i ? "," : "");
        jkv("ssid", (const char *)scan_aps[i].ssid);
        jp("\"rssi\":%d,\"ch\":%u,\"open\":%s}", scan_aps[i].rssi, scan_aps[i].primary,
           scan_aps[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    jp("]");
    return jflush();
}

static int cmd_restart(int argc, char **argv)
{
    printf("Khoi dong lai...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
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
    repl_cfg.prompt = "gw>";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    register_cmd("status", "Wi-Fi, MQTT, server va 3 zone", cmd_status);
    register_cmd("net", "Xem cau hinh mang da luu", cmd_net);
    register_cmd("wifi", "wifi <ssid> [mat khau]: hotspot dien thoai (luu NVS)", cmd_wifi);
    register_cmd("broker", "broker <ten-laptop>.local|<ip> [port] | broker wss://<ten-mien>: dia chi Mosquitto", cmd_broker);
    register_cmd("broker_ip", "broker_ip <ip>|-: IP du phong khi mDNS loi", cmd_broker_ip);
    register_cmd("mqtt_user", "mqtt_user <user> <mat khau>: tai khoan broker (mac dinh user 'gateway')", cmd_mqtt_user);
    register_cmd("mac", "Dia chi MAC cua gateway", cmd_mac);
    register_cmd("restart", "Khoi dong lai (ap dung cau hinh moi)", cmd_restart);
    register_cmd("net_json", "Cau hinh mang dang JSON (cho phan mem cau hinh)", cmd_net_json);
    register_cmd("status_json", "Trang thai dang JSON (cho phan mem cau hinh)", cmd_status_json);
    register_cmd("wifi_scan", "Quet Wi-Fi xung quanh, in JSON", cmd_wifi_scan);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
