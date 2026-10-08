// Node Zone: máy trạng thái bơm + giới hạn an toàn (docs/DESIGN.md §4.2, §11)
#include "pump.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

static SemaphoreHandle_t mtx;
static pump_event_cb_t event_cb = NULL;

static bool running = false;
static pump_source_t run_source = SRC_CMD;
static uint16_t run_cmd_seq = 0;
static uint16_t run_target_s = 0;
static uint32_t run_start_ms = 0;

static bool has_watered = false;  // chưa tưới lần nào thì không có cooldown
static uint32_t last_end_ms = 0;

static void relay_write(bool on)
{
    gpio_set_level(PIN_RELAY, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

static uint32_t cooldown_left(uint32_t now)
{
    if (!has_watered) return 0;
    uint32_t elapsed = now - last_end_ms;
    return elapsed >= PUMP_COOLDOWN_MS ? 0 : PUMP_COOLDOWN_MS - elapsed;
}

static void emit(const pump_event_t *ev)
{
    if (event_cb) event_cb(ev);
}

// Gọi khi đang giữ mtx; event được phát sau khi nhả mutex.
static pump_event_t turn_off_locked(pump_source_t reason, uint32_t now)
{
    relay_write(false);
    running = false;
    has_watered = true;
    last_end_ms = now;
    return (pump_event_t){false, reason, run_cmd_seq, (uint16_t)((now - run_start_ms + 500) / 1000)};
}

static void pump_task(void *arg)
{
    esp_task_wdt_add(NULL);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = now_ms();
        bool fire = false;
        pump_event_t ev;

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

        if (fire) emit(&ev);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PUMP_TASK_PERIOD_MS));
    }
}

void pump_init(void)
{
    gpio_set_level(PIN_RELAY, RELAY_OFF_LEVEL);  // đặt mức OFF trước khi chuyển sang output
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_RELAY,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    relay_write(false);
    mtx = xSemaphoreCreateMutex();
}

void pump_start_task(void)
{
    xTaskCreatePinnedToCore(pump_task, "pump", 3072, NULL, 4, NULL, 1);
}

void pump_set_event_cb(pump_event_cb_t cb)
{
    event_cb = cb;
}

ack_status_t pump_start(uint16_t duration_s, pump_source_t source, uint16_t cmd_seq)
{
    if (duration_s == 0 || duration_s > settings_get().pump_max_s) return ACK_REJ_INVALID;

    uint32_t now = now_ms();
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

    emit(&(pump_event_t){true, source, cmd_seq, 0});
    return ACK_OK;
}

bool pump_stop(void)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (!running) {
        xSemaphoreGive(mtx);
        return false;
    }
    pump_event_t ev = turn_off_locked(run_source, now_ms());
    xSemaphoreGive(mtx);

    emit(&ev);
    return true;
}

pump_status_t pump_status(void)
{
    uint32_t now = now_ms();
    xSemaphoreTake(mtx, portMAX_DELAY);
    pump_status_t s = {running, run_source, run_cmd_seq, run_target_s,
                       running ? now - run_start_ms : 0, cooldown_left(now)};
    xSemaphoreGive(mtx);
    return s;
}

void pump_clear_cooldown(void)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    has_watered = false;
    xSemaphoreGive(mtx);
}
