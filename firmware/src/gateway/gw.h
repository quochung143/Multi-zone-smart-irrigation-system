#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

// ACK từ nút, uplink chuyển cho cmdTask đối chiếu
struct NodeAck {
    uint8_t  zone;
    uint16_t ack_seq;
    uint8_t  status;
};
extern QueueHandle_t g_ack_queue;

// Serial (in cả dòng một lần, an toàn đa task)
void serial_out_init();
void serial_out(const char *line);

// ESP-NOW
int zone_from_mac(const uint8_t *mac);  // 1..NUM_ZONES, 0 nếu không thuộc bảng
bool gw_send(uint8_t zone, const void *data, size_t len);

// uplink.cpp: nhận gói từ nút, phát JSON, theo dõi online, beacon
void uplink_init();
void uplink_start_tasks();

// cmd.cpp: đọc JSON từ server, gửi lệnh có retry/ACK
void cmd_init();
void cmd_start_tasks();
bool server_alive();
uint32_t server_epoch_now();  // 0 nếu chưa đồng bộ
