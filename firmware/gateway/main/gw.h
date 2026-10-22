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

// Serial tới server (in cả dòng một lần, an toàn đa task).
// serial_out chỉ in khi server còn sống: không ai mở cổng thì chip USB-UART không giữ dữ liệu cũ,
// tránh driver CP210x phát lại hàng loạt khi cổng được mở (đã gây BSOD 0xB8 trên Windows).
void serial_out(const char *line);
void serial_out_always(const char *line);  // hello, lỗi khởi động, trả lời trực tiếp dòng server vừa gửi

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

// cmd.c: đọc JSON từ server, gửi lệnh có retry/ACK
void cmd_init(void);
void cmd_start_tasks(void);
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

// display.c: màn hình TFT (bỏ qua nếu LCD_ENABLED = 0)
void display_start(void);
