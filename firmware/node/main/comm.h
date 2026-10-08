#pragma once
#include <stdint.h>

#include "protocol.h"

// ESP-NOW phía nút: telemetry định kỳ, nhận lệnh + ACK, gửi PUMP_EVENT, fallback khi mất gateway.
void comm_init(void);
void comm_start_task(void);

node_mode_t comm_mode(void);
uint32_t comm_gateway_age_ms(void);  // thời gian kể từ gói cuối nhận từ gateway

// Đóng gói trạng thái hiện tại thành msg_telemetry_t.
void comm_build_telemetry(msg_telemetry_t *msg, uint8_t zone, uint16_t seq);
