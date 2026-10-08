#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Khung gói tin ESP-NOW dùng chung node/gateway (docs/PROTOCOL.md)
// Tất cả trường là little-endian, struct packed (không padding).

// MsgHeader.type
enum {
    MSG_TELEMETRY  = 1,   // node -> gw
    MSG_PUMP_EVENT = 2,   // node -> gw
    MSG_ACK        = 3,   // node -> gw
    MSG_CMD_WATER  = 10,  // gw -> node
    MSG_CMD_STOP   = 11,  // gw -> node
    MSG_CONFIG     = 12,  // gw -> node
    MSG_BEACON     = 13,  // gw -> node
};

typedef enum { MODE_NORMAL = 0, MODE_FALLBACK = 1 } node_mode_t;
typedef enum { SRC_CMD = 0, SRC_FALLBACK = 1, SRC_SAFETY_STOP = 2 } pump_source_t;
typedef enum { ACK_OK = 0, ACK_REJ_COOLDOWN = 1, ACK_REJ_BUSY = 2, ACK_REJ_INVALID = 3 } ack_status_t;

// Bit trong msg_telemetry_t.flags
#define FLAG_SHT_ERR   0x01
#define FLAG_ADC_ERR   0x02
#define FLAG_COOLDOWN  0x04

typedef struct __attribute__((packed)) {
    uint8_t  type;       // MSG_*
    uint8_t  zone_id;    // 1..3
    uint16_t seq;        // tăng dần theo từng chiều gửi
} msg_header_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
    uint16_t soil_raw;   // ADC sau lọc (0..4095)
    uint16_t soil_x10;   // % độ ẩm đất × 10
    int16_t  temp_x100;  // °C × 100
    uint16_t hum_x100;   // %RH × 100
    uint8_t  pump_on;    // 0/1
    uint8_t  node_mode;  // node_mode_t
    uint8_t  flags;      // FLAG_*
    uint32_t uptime_s;
} msg_telemetry_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
    uint8_t  on;         // 1 = bật, 0 = tắt
    uint8_t  source;     // pump_source_t
    uint16_t cmd_seq;    // seq lệnh gây ra (0 nếu fallback)
    uint16_t duration_s; // thời gian đã chạy (khi on = 0)
} msg_pump_event_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
    uint16_t ack_seq;    // seq của lệnh được xác nhận
    uint8_t  status;     // ack_status_t
} msg_ack_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
    uint16_t duration_s; // 1..pump_max_s
} msg_cmd_water_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
} msg_cmd_stop_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
    uint16_t soil_low_x10;     // ngưỡng tưới fallback
    uint16_t pump_max_s;
    uint16_t fallback_water_s;
} msg_config_t;

typedef struct __attribute__((packed)) {
    msg_header_t h;
    uint32_t epoch;      // 0 nếu server chưa đồng bộ giờ
} msg_beacon_t;

static_assert(sizeof(msg_header_t)     == 4,  "msg_header_t");
static_assert(sizeof(msg_telemetry_t)  == 19, "msg_telemetry_t");
static_assert(sizeof(msg_pump_event_t) == 10, "msg_pump_event_t");
static_assert(sizeof(msg_ack_t)        == 7,  "msg_ack_t");
static_assert(sizeof(msg_cmd_water_t)  == 6,  "msg_cmd_water_t");
static_assert(sizeof(msg_cmd_stop_t)   == 4,  "msg_cmd_stop_t");
static_assert(sizeof(msg_config_t)     == 10, "msg_config_t");
static_assert(sizeof(msg_beacon_t)     == 8,  "msg_beacon_t");

#define MSG_MAX_LEN 32

// Độ dài hợp lệ của từng loại gói; 0 nếu type không xác định.
static inline size_t msg_expected_len(uint8_t type)
{
    switch (type) {
    case MSG_TELEMETRY:  return sizeof(msg_telemetry_t);
    case MSG_PUMP_EVENT: return sizeof(msg_pump_event_t);
    case MSG_ACK:        return sizeof(msg_ack_t);
    case MSG_CMD_WATER:  return sizeof(msg_cmd_water_t);
    case MSG_CMD_STOP:   return sizeof(msg_cmd_stop_t);
    case MSG_CONFIG:     return sizeof(msg_config_t);
    case MSG_BEACON:     return sizeof(msg_beacon_t);
    default:             return 0;
    }
}

// Kiểm tra gói nhận được: đủ header, type biết, độ dài khớp.
static inline bool msg_validate(const uint8_t *data, size_t len)
{
    if (len < sizeof(msg_header_t)) return false;
    size_t expected = msg_expected_len(data[0]);
    return expected != 0 && len == expected;
}

static inline const char *pump_source_name(uint8_t s)
{
    return s == SRC_CMD ? "cmd" : s == SRC_FALLBACK ? "fallback" : s == SRC_SAFETY_STOP ? "safety" : "?";
}
