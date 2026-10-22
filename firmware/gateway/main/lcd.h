#pragma once
#include <stdint.h>

#include "esp_err.h"

// Driver tối giản TFT SPI 240x320 (ILI9341 / ST7789) qua esp_lcd panel IO, vẽ ngang 320x240.
#define LCD_W 320
#define LCD_H 240

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BLACK  RGB565(0, 0, 0)
#define COLOR_WHITE  RGB565(255, 255, 255)
#define COLOR_GREY   RGB565(140, 140, 140)
#define COLOR_DARK   RGB565(60, 60, 60)
#define COLOR_GREEN  RGB565(40, 220, 80)
#define COLOR_RED    RGB565(240, 60, 50)
#define COLOR_YELLOW RGB565(250, 210, 40)
#define COLOR_ORANGE RGB565(255, 140, 0)
#define COLOR_CYAN   RGB565(60, 200, 255)
#define COLOR_BLUE   RGB565(20, 70, 160)

esp_err_t lcd_init(void);
void lcd_fill(int x, int y, int w, int h, uint16_t color);
// Vẽ chuỗi ASCII tại (x, y), phóng to scale lần (1 hoặc 2); đệm bằng màu nền cho đủ width_chars ký tự
// để xoá chữ cũ dài hơn mà không phải xoá cả vùng (không nhấp nháy).
void lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale, int width_chars);
