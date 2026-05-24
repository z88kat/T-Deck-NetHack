/* tdeck_keyboard.c -- I2C-polled BlackBerry-style QWERTY for T-Deck. */

#include "tdeck_keyboard.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "tdeck_kbd";

static i2c_master_bus_handle_t  s_bus    = NULL;
static i2c_master_dev_handle_t  s_dev    = NULL;
static QueueHandle_t            s_queue  = NULL;
static TaskHandle_t             s_task   = NULL;
static volatile TickType_t      s_last_key_tick = 0;

/* Poll period.  ATtiny firmware buffers one key at a time; ~20 ms is
 * fast enough for human typing without saturating I2C. */
#define POLL_PERIOD_MS   20

/* Remap raw bytes from the keyboard IC into NetHack-friendly codes.
 * The LilyGo ATtiny firmware sends ASCII for letters/digits/punct,
 * plus a handful of special codes for non-printable keys.  We patch a
 * few that NetHack expects but the T-Deck doesn't have native keys for:
 *
 *   `      -> ESC      (backtick stands in for the missing ESC key)
 *   0x08   -> ESC      (backspace doubles as ESC; the keyboard has no
 *                       dedicated ESC and backspace is rarely used by
 *                       NetHack outside text-entry prompts)
 *
 * Sym-prefixed keys: depending on T-Deck firmware revision, the chip
 * either returns the symbol byte directly (e.g. '#' for Sym+Q) or
 * doesn't map the combo at all.  Watch the INFO log lines below to see
 * what your unit sends, then extend the switch as needed. */
static int
remap_key(uint8_t raw)
{
    switch (raw) {
    case '`':       return 0x1B;    /* ESC */
    case 0x08:      return 0x1B;    /* backspace -> ESC */
    case '~':       return 0x10;    /* Ctrl+P (prevmsg / scrollback);
                                       Shift+backtick on most layouts */
    default:        return raw;
    }
}

static void
poll_task(void *arg)
{
    (void) arg;
    uint8_t byte;
    for (;;) {
        esp_err_t err = i2c_master_receive(s_dev, &byte, 1,
                                           pdMS_TO_TICKS(20));
        if (err == ESP_OK && byte != 0x00) {
            int key = remap_key(byte);
            ESP_LOGI(TAG, "key raw=0x%02x ('%c') -> 0x%02x ('%c')",
                     byte, (byte >= 0x20 && byte < 0x7f) ? byte : '?',
                     key,  (key  >= 0x20 && key  < 0x7f) ? key  : '?');
            xQueueSend(s_queue, &key, 0);  /* drop on full */
            s_last_key_tick = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

esp_err_t
tdeck_keyboard_init(void)
{
    if (s_task) return ESP_OK;   /* already initialised */

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TDECK_KBD_I2C_PORT,
        .sda_io_num = TDECK_KBD_PIN_SDA,
        .scl_io_num = TDECK_KBD_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TDECK_KBD_I2C_ADDR,
        .scl_speed_hz = TDECK_KBD_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    /* Probe so we can log up front whether the keyboard is alive. */
    err = i2c_master_probe(s_bus, TDECK_KBD_I2C_ADDR, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "keyboard not responding at 0x%02x (%s); polling anyway",
                 TDECK_KBD_I2C_ADDR, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "keyboard found at I2C 0x%02x", TDECK_KBD_I2C_ADDR);
    }

    s_queue = xQueueCreate(16, sizeof(int));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(poll_task, "tdeck_kbd",
                                            4096, NULL, 5, &s_task, 0);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int) rc);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int
tdeck_keyboard_getchar(TickType_t timeout_ticks)
{
    if (!s_queue) return -1;
    int key;
    if (xQueueReceive(s_queue, &key, timeout_ticks) == pdTRUE) {
        return key;
    }
    return -1;
}

bool
tdeck_keyboard_peek(int *out)
{
    if (!s_queue || !out) return false;
    int key;
    if (xQueueReceive(s_queue, &key, 0) == pdTRUE) {
        *out = key;
        return true;
    }
    return false;
}

TickType_t
tdeck_keyboard_last_activity(void)
{
    return s_last_key_tick;
}
