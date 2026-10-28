#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"

// ACK từ nút, uplink chuyển cho cmd_task đối chiếu
typedef struct {
    uint8_t  zone;
    uint16_t ack_seq;
    uint8_t  status;
} node_ack_t;
extern QueueHandle_t g_ack_queue;

// MQTT tới server (docs/SERVER.md §3): publish irr/z/{zone}/{kind}, payload JSON giữ nguyên như bản Serial.
// Chỉ gửi khi server còn sống: không đệm dữ liệu lúc mất server (DESIGN Q10).
bool gw_publish(uint8_t zone, const char *kind, const char *json, int qos, bool retain);

// ESP-NOW
int zone_from_mac(const uint8_t *mac);  // 1..NUM_ZONES, 0 nếu không thuộc bảng
bool gw_send(uint8_t zone, const void *data, size_t len);

// uplink.c: nhận gói từ nút, phát JSON, theo dõi online, beacon
void uplink_init(void);
void uplink_start_tasks(void);

// Ảnh chụp trạng thái một zone cho màn hình
typedef struct {
    bool            configured;  // MAC zone đã điền trong app_config.h
    bool            has_tel;     // đã nhận ít nhất 1 telemetry
    bool            online;
    uint32_t        age_s;       // giây kể từ gói cuối
    msg_telemetry_t tel;         // telemetry gần nhất
    int8_t          rssi;
    bool            pump_on;     // cập nhật cả từ PUMP_EVENT, nhanh hơn chờ telemetry
    uint32_t        rx, lost;
} zone_view_t;
void uplink_zone_view(int zone, zone_view_t *out);

// cmd.c: lệnh JSON từ server (irr/z/{zone}/cmd) -> ESP-NOW có retry/ACK; heartbeat server
void cmd_init(void);
void cmd_start_tasks(void);
void cmd_handle_json(const char *json, int len);
void cmd_on_server_status(bool online);  // irr/srv/status (retain + LWT của server)
void cmd_on_time(uint32_t epoch);        // irr/srv/time mỗi 60 s
// Server còn sống = MQTT đang nối + srv/status online + có time trong SERVER_TIMEOUT_MS (SERVER.md §3.2)
bool server_alive(void);
uint32_t server_epoch_now(void);  // 0 nếu chưa đồng bộ

typedef struct {
    bool     valid;
    uint8_t  zone;
    uint16_t seq;
    char     status[16];
    uint32_t at_ms;
} last_ack_t;
void cmd_last_ack(last_ack_t *out);

// net.c: Wi-Fi STA vào hotspot + client MQTT; cấu hình lưu NVS, sửa qua console
typedef struct {
    char     ssid[33];
    char     pass[65];
    char     host[64];   // "<ten>.local" (mDNS), IP, hoặc URL đầy đủ "wss://mqtt.example.com" (qua Internet)
    char     ip[16];     // IP dự phòng khi mDNS không phân giải được
    uint16_t port;
    char     user[33];
    char     mpass[65];
} net_config_t;
net_config_t net_config_get(void);
bool net_config_set(const net_config_t *c);  // lưu NVS; có hiệu lực sau khi khởi động lại
bool net_host_is_url(const char *host);      // "scheme://..." thì nối thẳng URL, không mDNS/IP dự phòng

typedef struct {
    bool    wifi;         // đã có IP
    bool    mqtt;         // đã nối broker
    uint8_t channel;      // kênh Wi-Fi hiện tại = kênh ESP-NOW
    char    ssid[33];
    char    ip[16];
    char    broker[64];   // IP:port hoặc URL broker đang dùng
} net_status_t;
void net_init(void);      // gọi sau esp_wifi_init
void net_start(void);
void net_get_status(net_status_t *out);
bool net_mqtt_connected(void);

// console.c: REPL trên USB (UART0) để cấu hình Wi-Fi/broker
void console_start(void);

// display.c: màn hình TFT (bỏ qua nếu LCD_ENABLED = 0)
void display_start(void);
