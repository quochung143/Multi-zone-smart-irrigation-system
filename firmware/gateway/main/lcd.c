// Gateway: driver TFT SPI 240x320 tối giản (ILI9341 / ST7789) dùng esp_lcd panel IO
#include "lcd.h"

#include <string.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "font8x16.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_HOST   SPI3_HOST          // VSPI: chân gốc SCLK 18 / MOSI 23
#define BUF_LINES  (FONT_H * 2)       // đủ một dòng chữ phóng to 2 lần
#define BUF_PX     (LCD_W * BUF_LINES)

// MADCTL: MY 0x80, MX 0x40, MV 0x20 (đổi hàng/cột = xoay ngang), BGR 0x08
#if LCD_CONTROLLER == LCD_CTRL_ST7789
#define MADCTL_BASE 0x60  // MX | MV, thứ tự RGB
#define INVERT      1     // ST7789 cần bật đảo màu
#else
#define MADCTL_BASE 0x28  // MV | BGR
#define INVERT      0
#endif

static const char *TAG = "lcd";
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t done_sem;
static uint16_t *buf;  // RGB565 big-endian (byte cao gửi trước)

static bool on_color_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(done_sem, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, const uint8_t *data, size_t len)
{
    esp_lcd_panel_io_tx_param(io, c, data, len);
}

static inline uint16_t be(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

// Gửi px điểm ảnh đầu của buf vào cửa sổ (x, y, w, h), chờ DMA xong để dùng lại buf.
static void push(int x, int y, int w, int h)
{
    uint8_t ca[4] = {x >> 8, x & 0xFF, (x + w - 1) >> 8, (x + w - 1) & 0xFF};
    uint8_t ra[4] = {y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF};
    cmd(0x2A, ca, 4);  // CASET
    cmd(0x2B, ra, 4);  // RASET
    esp_lcd_panel_io_tx_color(io, 0x2C, buf, (size_t)w * h * 2);  // RAMWR
    xSemaphoreTake(done_sem, portMAX_DELAY);
}

esp_err_t lcd_init(void)
{
    done_sem = xSemaphoreCreateBinary();
    buf = heap_caps_malloc(BUF_PX * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buf");

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUF_PX * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io), TAG, "io");

    gpio_config_t out = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << LCD_PIN_RST};
    if (LCD_PIN_BL >= 0) out.pin_bit_mask |= 1ULL << LCD_PIN_BL;
    gpio_config(&out);
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    cmd(0x01, NULL, 0);  // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11, NULL, 0);  // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x3A, (uint8_t[]){0x55}, 1);  // COLMOD: 16 bit/điểm
    uint8_t madctl = MADCTL_BASE ^ (LCD_SWAP_RB ? 0x08 : 0) ^ (LCD_MIRROR ? 0x40 : 0) ^ (LCD_FLIP ? 0xC0 : 0);
    cmd(0x36, &madctl, 1);
    cmd((INVERT ^ LCD_INVERT) ? 0x21 : 0x20, NULL, 0);  // INVON / INVOFF (một số tấm IPS cần đảo)
    cmd(0x29, NULL, 0);  // DISPON
    vTaskDelay(pdMS_TO_TICKS(20));

    lcd_fill(0, 0, LCD_W, LCD_H, COLOR_BLACK);
    if (LCD_PIN_BL >= 0) gpio_set_level(LCD_PIN_BL, 1);
    return ESP_OK;
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    int lines = BUF_PX / w;
    if (lines > h) lines = h;
    uint16_t c = be(color);
    for (int i = 0; i < w * lines; i++) buf[i] = c;
    for (int row = y; row < y + h; row += lines) {
        int n = (y + h - row) < lines ? (y + h - row) : lines;
        push(x, row, w, n);
    }
}

void lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale, int width_chars)
{
    int cw = FONT_W * scale, ch = FONT_H * scale;
    int len = (int)strlen(s);
    int n = width_chars > len ? width_chars : len;
    if (n > (LCD_W - x) / cw) n = (LCD_W - x) / cw;
    if (n <= 0) return;

    int w = n * cw;
    uint16_t f = be(fg), b = be(bg);
    for (int py = 0; py < ch; py++) {
        uint16_t *row = &buf[py * w];
        for (int i = 0; i < n; i++) {
            char c = i < len ? s[i] : ' ';
            uint8_t bits = (c >= FONT_FIRST && c <= FONT_LAST) ? FONT8X16[c - FONT_FIRST][py / scale] : 0;
            for (int px = 0; px < cw; px++)
                row[i * cw + px] = (bits & (0x80 >> (px / scale))) ? f : b;
        }
    }
    push(x, y, w, ch);
}
