#pragma once
#include <stddef.h>
#include <stdint.h>

// Khung gói tin ESP-NOW dùng chung node/gateway (docs/PROTOCOL.md)
// Tất cả trường là little-endian, struct packed (không padding).

#pragma pack(push, 1)
enum MsgType : uint8_t {
    MSG_TELEMETRY  = 1,   // node -> gw
    MSG_PUMP_EVENT = 2,   // node -> gw
    MSG_ACK        = 3,   // node -> gw
    MSG_CMD_WATER  = 10,  // gw -> node
    MSG_CMD_STOP   = 11,  // gw -> node
    MSG_CONFIG     = 12,  // gw -> node
    MSG_BEACON     = 13,  // gw -> node
};

enum NodeMode : uint8_t { MODE_NORMAL = 0, MODE_FALLBACK = 1 };
enum PumpSource : uint8_t { SRC_CMD = 0, SRC_FALLBACK = 1, SRC_SAFETY_STOP = 2 };
enum AckStatus : uint8_t { ACK_OK = 0, ACK_REJ_COOLDOWN = 1, ACK_REJ_BUSY = 2, ACK_REJ_INVALID = 3 };

// Bit trong MsgTelemetry.flags
#define FLAG_SHT_ERR   0x01
#define FLAG_ADC_ERR   0x02
#define FLAG_COOLDOWN  0x04

typedef struct {
    uint8_t  type;       // MsgType
    uint8_t  zone_id;    // 1..3
    uint16_t seq;        // tăng dần theo từng chiều gửi
} MsgHeader;

typedef struct {
    MsgHeader h;
    uint16_t soil_raw;   // ADC sau lọc (0..4095)
    uint16_t soil_x10;   // % độ ẩm đất × 10
    int16_t  temp_x100;  // °C × 100
    uint16_t hum_x100;   // %RH × 100
    uint8_t  pump_on;    // 0/1
    uint8_t  node_mode;  // NodeMode
    uint8_t  flags;      // FLAG_*
    uint32_t uptime_s;
} MsgTelemetry;

typedef struct {
    MsgHeader h;
    uint8_t  on;         // 1 = bật, 0 = tắt
    uint8_t  source;     // PumpSource
    uint16_t cmd_seq;    // seq lệnh gây ra (0 nếu fallback)
    uint16_t duration_s; // thời gian đã chạy (khi on = 0)
} MsgPumpEvent;

typedef struct {
    MsgHeader h;
    uint16_t ack_seq;    // seq của lệnh được xác nhận
    uint8_t  status;     // AckStatus
} MsgAck;

typedef struct {
    MsgHeader h;
    uint16_t duration_s; // 1..pump_max_s
} MsgCmdWater;

typedef struct {
    MsgHeader h;
} MsgCmdStop;

typedef struct {
    MsgHeader h;
    uint16_t soil_low_x10;     // ngưỡng tưới fallback
    uint16_t pump_max_s;
    uint16_t fallback_water_s;
} MsgConfig;

typedef struct {
    MsgHeader h;
    uint32_t epoch;      // 0 nếu server chưa đồng bộ giờ
} MsgBeacon;
#pragma pack(pop)

static_assert(sizeof(MsgHeader)    == 4,  "MsgHeader");
static_assert(sizeof(MsgTelemetry) == 19, "MsgTelemetry");
static_assert(sizeof(MsgPumpEvent) == 10, "MsgPumpEvent");
static_assert(sizeof(MsgAck)       == 7,  "MsgAck");
static_assert(sizeof(MsgCmdWater)  == 6,  "MsgCmdWater");
static_assert(sizeof(MsgCmdStop)   == 4,  "MsgCmdStop");
static_assert(sizeof(MsgConfig)    == 10, "MsgConfig");
static_assert(sizeof(MsgBeacon)    == 8,  "MsgBeacon");

// Độ dài hợp lệ của từng loại gói; 0 nếu type không xác định.
static inline size_t msg_expected_len(uint8_t type) {
    switch (type) {
        case MSG_TELEMETRY:  return sizeof(MsgTelemetry);
        case MSG_PUMP_EVENT: return sizeof(MsgPumpEvent);
        case MSG_ACK:        return sizeof(MsgAck);
        case MSG_CMD_WATER:  return sizeof(MsgCmdWater);
        case MSG_CMD_STOP:   return sizeof(MsgCmdStop);
        case MSG_CONFIG:     return sizeof(MsgConfig);
        case MSG_BEACON:     return sizeof(MsgBeacon);
        default:             return 0;
    }
}

// Kiểm tra gói nhận được: đủ header, type biết, độ dài khớp.
static inline bool msg_validate(const uint8_t *data, size_t len) {
    if (len < sizeof(MsgHeader)) return false;
    size_t expected = msg_expected_len(data[0]);
    return expected != 0 && len == expected;
}
