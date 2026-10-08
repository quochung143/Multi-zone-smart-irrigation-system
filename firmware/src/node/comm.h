#pragma once
#include <stdint.h>

#include "protocol.h"

// ESP-NOW phía nút: telemetry định kỳ, nhận lệnh + ACK, gửi PUMP_EVENT, fallback khi mất gateway.
void comm_init();
void comm_start_task();

NodeMode comm_mode();
uint32_t comm_gateway_age_ms();  // thời gian kể từ gói cuối nhận từ gateway

// Đóng gói trạng thái hiện tại thành MsgTelemetry.
void comm_build_telemetry(MsgTelemetry *msg, uint16_t seq);
