/* tdeck_keyboard.h -- BlackBerry-style QWERTY keyboard on the LilyGo T-Deck.
 *
 * The keyboard is an ATtiny running custom firmware that lives on the
 * I2C bus at address 0x55.  Reading 1 byte returns the latest keypress
 * (ASCII), or 0x00 if no key has been pressed since the last read.
 *
 * Usage:
 *     tdeck_keyboard_init();
 *     int c = tdeck_keyboard_getchar(portMAX_DELAY);  // blocking
 *
 * Special-key conventions exposed to callers:
 *     0x1B (ESC)   - mapped from backtick `
 *     0x0D (CR)    - the enter key
 *     0x08 (BS)    - the backspace key
 *     0xE0..0xE3   - reserved for future arrow / trackball events
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* T-Deck pin map.  Override at compile time if your hardware differs. */
#ifndef TDECK_KBD_I2C_PORT
#define TDECK_KBD_I2C_PORT   0
#endif
#ifndef TDECK_KBD_PIN_SDA
#define TDECK_KBD_PIN_SDA    18
#endif
#ifndef TDECK_KBD_PIN_SCL
#define TDECK_KBD_PIN_SCL    8
#endif
#ifndef TDECK_KBD_PIN_INT
#define TDECK_KBD_PIN_INT    46  /* active-low keypress interrupt; we
                                    currently poll instead of using it */
#endif
#ifndef TDECK_KBD_I2C_ADDR
#define TDECK_KBD_I2C_ADDR   0x55
#endif
#ifndef TDECK_KBD_I2C_FREQ_HZ
#define TDECK_KBD_I2C_FREQ_HZ  100000
#endif

/* Initialise I2C and start the polling task.  Safe to call once at boot. */
esp_err_t tdeck_keyboard_init(void);

/* Block (up to timeout_ticks) for the next keypress.  Returns the
 * single-byte key code or -1 on timeout.  Pass portMAX_DELAY to wait
 * indefinitely. */
int tdeck_keyboard_getchar(TickType_t timeout_ticks);

/* Non-blocking peek: returns true and stores the next key in *out, or
 * false if the queue is empty. */
bool tdeck_keyboard_peek(int *out);

#ifdef __cplusplus
}
#endif
