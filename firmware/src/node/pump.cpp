// Node Zone: máy trạng thái bơm + giới hạn an toàn (docs/DESIGN.md §4.2, §11)
#include "pump.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "settings.h"

static SemaphoreHandle_t mtx;
static PumpEventCb event_cb = nullptr;

static bool running = false;
static PumpSource run_source = SRC_CMD;
static uint16_t run_cmd_seq = 0;
static uint16_t run_target_s = 0;
static uint32_t run_start_ms = 0;

static bool has_watered = false;  // chưa tưới lần nào thì không có cooldown
static uint32_t last_end_ms = 0;

static void relay_write(bool on) {
    digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
}

static uint32_t cooldown_left(uint32_t now) {
    if (!has_watered) return 0;
    uint32_t elapsed = now - last_end_ms;
    return elapsed >= PUMP_COOLDOWN_MS ? 0 : PUMP_COOLDOWN_MS - elapsed;
}

static void emit(const PumpEvent &ev) {
    if (event_cb) event_cb(ev);
}

// Gọi khi đang giữ mtx; event được phát sau khi nhả mutex.
static PumpEvent turn_off_locked(PumpSource reason, uint32_t now) {
    relay_write(false);
    running = false;
    has_watered = true;
    last_end_ms = now;
    return {false, reason, run_cmd_seq, (uint16_t)((now - run_start_ms + 500) / 1000)};
}

static void pump_task(void *) {
    esp_task_wdt_add(NULL);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = millis();
        bool fire = false;
        PumpEvent ev;

        xSemaphoreTake(mtx, portMAX_DELAY);
        if (running) {
            uint32_t elapsed = now - run_start_ms;
            if (elapsed >= PUMP_MAX_S * 1000UL) {
                ev = turn_off_locked(SRC_SAFETY_STOP, now);
                fire = true;
            } else if (elapsed >= run_target_s * 1000UL) {
                ev = turn_off_locked(run_source, now);
                fire = true;
            }
        } else {
            relay_write(false);  // luôn ép relay OFF khi không tưới
        }
        xSemaphoreGive(mtx);

        if (fire) emit(ev);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PUMP_TASK_PERIOD_MS));
    }
}

void pump_init() {
    digitalWrite(PIN_RELAY, RELAY_OFF);  // đặt mức OFF trước khi chuyển sang output
    pinMode(PIN_RELAY, OUTPUT);
    relay_write(false);
    mtx = xSemaphoreCreateMutex();
}

void pump_start_task() {
    xTaskCreatePinnedToCore(pump_task, "pump", 3072, NULL, 3, NULL, 1);
}

void pump_set_event_cb(PumpEventCb cb) {
    event_cb = cb;
}

AckStatus pump_start(uint16_t duration_s, PumpSource source, uint16_t cmd_seq) {
    if (duration_s == 0 || duration_s > settings_get().pump_max_s) return ACK_REJ_INVALID;

    uint32_t now = millis();
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (running) {
        xSemaphoreGive(mtx);
        return ACK_REJ_BUSY;
    }
    if (cooldown_left(now) > 0) {
        xSemaphoreGive(mtx);
        return ACK_REJ_COOLDOWN;
    }
    running = true;
    run_source = source;
    run_cmd_seq = cmd_seq;
    run_target_s = duration_s;
    run_start_ms = now;
    relay_write(true);
    xSemaphoreGive(mtx);

    emit({true, source, cmd_seq, 0});
    return ACK_OK;
}

bool pump_stop() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (!running) {
        xSemaphoreGive(mtx);
        return false;
    }
    PumpEvent ev = turn_off_locked(run_source, millis());
    xSemaphoreGive(mtx);

    emit(ev);
    return true;
}

PumpStatus pump_status() {
    uint32_t now = millis();
    xSemaphoreTake(mtx, portMAX_DELAY);
    PumpStatus s = {running, run_source, run_cmd_seq, run_target_s,
                    running ? now - run_start_ms : 0, cooldown_left(now)};
    xSemaphoreGive(mtx);
    return s;
}

void pump_clear_cooldown() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    has_watered = false;
    xSemaphoreGive(mtx);
}
