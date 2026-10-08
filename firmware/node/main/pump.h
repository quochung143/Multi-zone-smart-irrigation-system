#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

typedef struct {
    bool          on;
    pump_source_t source;      // khi tắt: nguồn đã bật bơm, hoặc SRC_SAFETY_STOP
    uint16_t      cmd_seq;
    uint16_t      duration_s;  // thời gian đã chạy (khi on = false)
} pump_event_t;
typedef void (*pump_event_cb_t)(const pump_event_t *ev);

typedef struct {
    bool          on;
    pump_source_t source;
    uint16_t      cmd_seq;
    uint16_t      target_s;
    uint32_t      elapsed_ms;
    uint32_t      cooldown_left_ms;
} pump_status_t;

void pump_init(void);
void pump_start_task(void);
void pump_set_event_cb(pump_event_cb_t cb);  // gọi từ ngữ cảnh task, không gọi trong ISR

// Bật bơm duration_s giây; trả về trạng thái để dùng làm ACK.
ack_status_t pump_start(uint16_t duration_s, pump_source_t source, uint16_t cmd_seq);
bool pump_stop(void);  // false nếu bơm đang không chạy
pump_status_t pump_status(void);
void pump_clear_cooldown(void);  // chỉ dùng khi thử nghiệm
