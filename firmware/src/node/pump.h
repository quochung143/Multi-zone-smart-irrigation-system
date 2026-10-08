#pragma once
#include <stdint.h>

#include "protocol.h"

struct PumpEvent {
    bool       on;
    PumpSource source;      // khi tắt: nguồn đã bật bơm, hoặc SRC_SAFETY_STOP
    uint16_t   cmd_seq;
    uint16_t   duration_s;  // thời gian đã chạy (khi on = false)
};
typedef void (*PumpEventCb)(const PumpEvent &ev);

struct PumpStatus {
    bool       on;
    PumpSource source;
    uint16_t   cmd_seq;
    uint16_t   target_s;
    uint32_t   elapsed_ms;
    uint32_t   cooldown_left_ms;
};

void pump_init();
void pump_start_task();
void pump_set_event_cb(PumpEventCb cb);  // gọi từ ngữ cảnh task, không gọi trong ISR

// Bật bơm duration_s giây; trả về trạng thái để dùng làm ACK.
AckStatus pump_start(uint16_t duration_s, PumpSource source, uint16_t cmd_seq);
bool pump_stop();  // false nếu bơm đang không chạy
PumpStatus pump_status();
void pump_clear_cooldown();  // chỉ dùng khi thử nghiệm
