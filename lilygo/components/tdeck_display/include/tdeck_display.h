/* tdeck_display.h -- ST7789 LCD driver for the LilyGo T-Deck.
 *
 * 320x240 colour TFT over SPI.  Pins are hard-coded for the stock T-Deck
 * schematic; override the macros below if you have a variant.
 *
 * Usage:
 *     tdeck_display_init();
 *     tdeck_display_fill(TDECK_COLOR_RGB565(0, 0, 255));      // blue
 *     tdeck_display_blit(0, 0, w, h, pixel_buffer);
 *
 * Pixels are RGB565 (16 bpp), little-endian in the buffer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* T-Deck pin map.  Override at compile time if your hardware differs. */
#ifndef TDECK_LCD_HOST
#define TDECK_LCD_HOST       SPI2_HOST
#endif
#ifndef TDECK_LCD_PIN_SCLK
#define TDECK_LCD_PIN_SCLK   40
#endif
#ifndef TDECK_LCD_PIN_MOSI
#define TDECK_LCD_PIN_MOSI   41
#endif
#ifndef TDECK_LCD_PIN_CS
#define TDECK_LCD_PIN_CS     12
#endif
#ifndef TDECK_LCD_PIN_DC
#define TDECK_LCD_PIN_DC     11
#endif
#ifndef TDECK_LCD_PIN_RST
#define TDECK_LCD_PIN_RST    (-1)   /* T-Deck routes RST through the IO
                                       expander; power-on reset is enough
                                       for v0. */
#endif
#ifndef TDECK_LCD_PIN_BL
#define TDECK_LCD_PIN_BL     42     /* backlight enable, active high */
#endif
#ifndef TDECK_PIN_POWERON
#define TDECK_PIN_POWERON    10     /* T-Deck peripheral power enable.
                                       Must be driven HIGH or the LCD,
                                       keyboard, SD, and trackball are
                                       all unpowered.  -1 to skip. */
#endif

/* Panel geometry (rotated to landscape). */
#define TDECK_LCD_WIDTH      320
#define TDECK_LCD_HEIGHT     240

/* RGB565 colour helper -- r/g/b are 0..255.
 *
 * The result is *byte-swapped* so that when stored as a uint16_t in
 * little-endian memory (Xtensa is LE), the bytes are emitted MSB-first
 * over SPI which is what ST7789 expects.  Without this swap, green
 * (0x07E0) gets read by the panel as 0xE007 ~= red. */
#define TDECK_COLOR_RGB565(r, g, b)                                  \
    ((uint16_t) __builtin_bswap16((uint16_t)                         \
        ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))))

#define TDECK_COLOR_BLACK   TDECK_COLOR_RGB565(0,   0,   0)
#define TDECK_COLOR_WHITE   TDECK_COLOR_RGB565(255, 255, 255)
#define TDECK_COLOR_RED     TDECK_COLOR_RGB565(255, 0,   0)
#define TDECK_COLOR_GREEN   TDECK_COLOR_RGB565(0,   255, 0)
#define TDECK_COLOR_BLUE    TDECK_COLOR_RGB565(0,   0,   255)

/* Initialise the SPI bus, the ST7789 panel, and the backlight.  Safe to
 * call once at boot.  Returns ESP_OK on success. */
esp_err_t tdeck_display_init(void);

/* Fill the entire screen with a single colour. */
void tdeck_display_fill(uint16_t color);

/* Push a rectangle of RGB565 pixels to the panel at (x, y) of size w x h.
 * `pixels` is row-major, length w*h. */
void tdeck_display_blit(int x, int y, int w, int h, const uint16_t *pixels);

/* Draw a single 8x8 ASCII character at pixel (x, y) using the built-in
 * font.  Out-of-range characters render as a filled block. */
void tdeck_display_putchar(int x, int y, char c, uint16_t fg, uint16_t bg);

/* Draw a NUL-terminated ASCII string at pixel (x, y); advances 8 pixels
 * per character.  No wrapping. */
void tdeck_display_print(int x, int y, const char *s, uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif
