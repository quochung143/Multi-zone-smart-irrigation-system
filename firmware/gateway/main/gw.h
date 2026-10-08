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

// Serial tới server (in cả dòng một lần, an toàn đa task)
void serial_out(const char *line);

// ESP-NOW
int zone_from_mac(const uint8_t *mac);  // 1..NUM_ZONES, 0 nếu không thuộc bảng
bool gw_send(uint8_t zone, const void *data, size_t len);

// uplink.c: nhận gói từ nút, phát JSON, theo dõi online, beacon
void uplink_init(void);
void uplink_start_tasks(void);

// cmd.c: đọc JSON từ server, gửi lệnh có retry/ACK
void cmd_init(void);
void cmd_start_tasks(void);
bool server_alive(void);
uint32_t server_epoch_now(void);  // 0 nếu chưa đồng bộ
