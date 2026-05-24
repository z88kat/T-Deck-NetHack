/* tdeck_display.c -- ST7789 driver using esp_lcd. */

#include "tdeck_display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "tdeck_display";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

/* Synchronisation for SPI transactions.  esp_lcd's draw_bitmap is async:
 * the function returns the moment the transaction is queued, but the SPI
 * DMA is still pulling bytes from the caller's buffer.  If the next
 * putchar call reuses the same stack offset before that DMA is done, the
 * old transaction reads garbage and the first character of every batch
 * gets dropped (and the next batch's first character bleeds in where it
 * landed).
 *
 * We register the on_color_trans_done callback (fires from ISR when the
 * SPI is fully drained) to signal a semaphore.  Every draw call below
 * waits on the semaphore before returning, so the caller is free to
 * reuse / pop / overwrite the source buffer immediately. */
static SemaphoreHandle_t s_trans_done_sem = NULL;
static bool IRAM_ATTR
on_trans_done(esp_lcd_panel_io_handle_t io,
              esp_lcd_panel_io_event_data_t *data, void *user_ctx)
{
    (void) io; (void) data; (void) user_ctx;
    BaseType_t hp_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done_sem, &hp_woken);
    return hp_woken == pdTRUE;
}

static inline void
draw_and_wait(int x_start, int y_start, int x_end, int y_end,
              const uint16_t *pixels)
{
    if (!s_panel) return;
    esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, pixels);
    if (s_trans_done_sem) {
        xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
    }
}

/* SPI clock for the panel.  ST7789 specs allow up to ~62.5 MHz; T-Deck
 * traces are short so 40 MHz works reliably.  Drop to 20 MHz if you see
 * stripes. */
#define TDECK_LCD_SPI_HZ   (40 * 1000 * 1000)

/* DMA transfer ceiling: one full 320x240 frame is 153600 bytes.  esp_lcd
 * splits big bitmaps into smaller transfers internally, so we just need
 * the per-call ceiling here.  64 KiB matches the SPI master DMA cap on
 * S3 without burning too much internal RAM. */
#define TDECK_LCD_TRANS_MAX_BYTES   (64 * 1024)

esp_err_t
tdeck_display_init(void)
{
    if (s_panel) {
        return ESP_OK; /* already initialised */
    }

    /* T-Deck peripheral power enable.  Without this the LCD (and the
     * keyboard, SD, trackball) have no power -- the SPI bus will happily
     * push pixels into a dark panel.  Drive HIGH then wait for the rails
     * to settle. */
    if (TDECK_PIN_POWERON >= 0) {
        gpio_config_t pwr_cfg = {
            .pin_bit_mask = 1ULL << TDECK_PIN_POWERON,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
        gpio_set_level(TDECK_PIN_POWERON, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "peripheral power enabled (GPIO %d)", TDECK_PIN_POWERON);
    }

    /* Backlight off while we configure -- avoids a flash of garbage. */
    if (TDECK_LCD_PIN_BL >= 0) {
        gpio_config_t bl_cfg = {
            .pin_bit_mask = 1ULL << TDECK_LCD_PIN_BL,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&bl_cfg));
        gpio_set_level(TDECK_LCD_PIN_BL, 0);
    }

    /* Init the SPI bus.  MISO is unused for the panel, but the bus needs
     * to be told. */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = TDECK_LCD_PIN_SCLK,
        .mosi_io_num = TDECK_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TDECK_LCD_TRANS_MAX_BYTES,
    };
    esp_err_t err = spi_bus_initialize(TDECK_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    /* esp_lcd panel-IO layer.  pclk_hz must be <= SPI host's max. */
    /* trans_queue_depth = 1: we deliberately serialise draws so the
     * per-call stack buffer in tdeck_display_putchar stays valid until
     * the SPI transaction has actually consumed it.  With a deeper
     * queue, up to N transactions point at the same stack offset and
     * the last writer's data clobbers earlier ones in flight -- which
     * manifests as the first character of each rendered line dropping
     * off the screen and the next line's first character bleeding onto
     * the previous line. */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TDECK_LCD_PIN_CS,
        .dc_gpio_num = TDECK_LCD_PIN_DC,
        .pclk_hz = TDECK_LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) TDECK_LCD_HOST,
                                   &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(err));
        return err;
    }

    /* Set up the trans-done semaphore + register the ISR callback that
     * signals it. */
    s_trans_done_sem = xSemaphoreCreateBinary();
    if (!s_trans_done_sem) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        return ESP_ERR_NO_MEM;
    }
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_trans_done,
    };
    esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL);

    /* ST7789 panel driver.  T-Deck's panel is BGR-ordered. */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TDECK_LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789: %s", esp_err_to_name(err));
        return err;
    }

    /* Reset (no-op if pin is -1; relies on power-on reset) and init. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Some ST7789 batches need a settling pause between init and first
     * pixel write; otherwise the first frame is dropped. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Landscape orientation for the LilyGo T-Deck:
     *   - swap_xy(true):       rotate 90 from native 240x320 portrait
     *   - mirror(true, false): X-mirror (rotates the image 180 relative
     *     to mirror(false, true), which had the screen upside down).
     *   - invert_color(true):  ST7789 panels in this family ship with
     *     inversion ON; matches TFT_eSPI's TFT_INVERSION_ON default. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));

    /* Turn the panel on, clear it black, then enable the backlight so
     * the first thing the user sees is a clean black screen. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    tdeck_display_fill(TDECK_COLOR_BLACK);

    if (TDECK_LCD_PIN_BL >= 0) {
        gpio_set_level(TDECK_LCD_PIN_BL, 1);
    }

    ESP_LOGI(TAG, "ST7789 init OK (%dx%d, SPI %d MHz)",
             TDECK_LCD_WIDTH, TDECK_LCD_HEIGHT, TDECK_LCD_SPI_HZ / 1000000);
    return ESP_OK;
}

/* A reusable scratch buffer for tdeck_display_fill / putchar.  Allocated
 * once on first use; lives in internal DRAM for fast DMA. */
static uint16_t *s_line_buf = NULL;
static size_t    s_line_buf_pixels = 0;

static void
ensure_line_buf(size_t pixels)
{
    if (s_line_buf && s_line_buf_pixels >= pixels) return;
    free(s_line_buf);
    s_line_buf = (uint16_t *) heap_caps_malloc(pixels * sizeof(uint16_t),
                                               MALLOC_CAP_DMA);
    s_line_buf_pixels = s_line_buf ? pixels : 0;
}

void
tdeck_display_fill(uint16_t color)
{
    if (!s_panel) return;
    ensure_line_buf(TDECK_LCD_WIDTH);
    if (!s_line_buf) return;

    for (int i = 0; i < TDECK_LCD_WIDTH; i++) {
        s_line_buf[i] = color;
    }
    /* One row at a time so the scratch buffer stays small. */
    for (int y = 0; y < TDECK_LCD_HEIGHT; y++) {
        draw_and_wait(0, y, TDECK_LCD_WIDTH, y + 1, s_line_buf);
    }
}

void
tdeck_display_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_panel || !pixels) return;
    if (w <= 0 || h <= 0) return;
    draw_and_wait(x, y, x + w, y + h, pixels);
}

/* --- Minimal 8x8 font (printable ASCII 0x20..0x7E only) ------------ */

/* This is the classic IBM VGA 8x8 font, public-domain version.  Each
 * character is 8 bytes; each byte is one row, MSB-left. */
static const uint8_t font_8x8[96][8] = {
    {0,0,0,0,0,0,0,0},                              /* 0x20 ' ' */
    {0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x00},      /* '!' */
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},      /* '"' */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},      /* '#' */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},      /* '$' */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},      /* '%' */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},      /* '&' */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},      /* "'" */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},      /* '(' */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},      /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},      /* '*' */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},      /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},      /* ',' */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},      /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00},      /* '.' */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},      /* '/' */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},      /* '0' */
    {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00},      /* '1' */
    {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00},      /* '2' */
    {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00},      /* '3' */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},      /* '4' */
    {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00},      /* '5' */
    {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00},      /* '6' */
    {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00},      /* '7' */
    {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00},      /* '8' */
    {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00},      /* '9' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},      /* ':' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},      /* ';' */
    {0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x00},      /* '<' */
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},      /* '=' */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},      /* '>' */
    {0x78,0xCC,0x0C,0x18,0x30,0x00,0x30,0x00},      /* '?' */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},      /* '@' */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00},      /* 'A' */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},      /* 'B' */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},      /* 'C' */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},      /* 'D' */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},      /* 'E' */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},      /* 'F' */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},      /* 'G' */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00},      /* 'H' */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00},      /* 'I' */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},      /* 'J' */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},      /* 'K' */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},      /* 'L' */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},      /* 'M' */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},      /* 'N' */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00},      /* 'O' */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},      /* 'P' */
    {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00},      /* 'Q' */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},      /* 'R' */
    {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00},      /* 'S' */
    {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00},      /* 'T' */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00},      /* 'U' */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00},      /* 'V' */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},      /* 'W' */
    {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00},      /* 'X' */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00},      /* 'Y' */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},      /* 'Z' */
    {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00},      /* '[' */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},      /* '\' */
    {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00},      /* ']' */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},      /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},      /* '_' */
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00},      /* '`' */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},      /* 'a' */
    {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00},      /* 'b' */
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00},      /* 'c' */
    {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00},      /* 'd' */
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00},      /* 'e' */
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00},      /* 'f' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},      /* 'g' */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},      /* 'h' */
    {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00},      /* 'i' */
    {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78},      /* 'j' */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},      /* 'k' */
    {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00},      /* 'l' */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00},      /* 'm' */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00},      /* 'n' */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00},      /* 'o' */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},      /* 'p' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},      /* 'q' */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00},      /* 'r' */
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00},      /* 's' */
    {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00},      /* 't' */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},      /* 'u' */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00},      /* 'v' */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00},      /* 'w' */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},      /* 'x' */
    {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8},      /* 'y' */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00},      /* 'z' */
    {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00},      /* '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},      /* '|' */
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00},      /* '}' */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},      /* '~' */
    {0,0,0,0,0,0,0,0},                              /* 0x7F filler */
};

void
tdeck_display_putchar(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (!s_panel) return;
    if (x < 0 || y < 0 || x + 8 > TDECK_LCD_WIDTH || y + 8 > TDECK_LCD_HEIGHT)
        return;

    const uint8_t *glyph;
    if ((unsigned char) c < 0x20 || (unsigned char) c >= 0x80) {
        static const uint8_t block[8] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        glyph = block;
    } else {
        glyph = font_8x8[(unsigned char) c - 0x20];
    }

    /* Static buffer (not stack) so the SPI DMA's source memory has a
     * stable address even after putchar returns.  Combined with the
     * trans-done semaphore inside draw_and_wait, the next caller can't
     * overwrite the buffer until the SPI is done with it. */
    static uint16_t buf[64];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            buf[row * 8 + col] = (bits & (0x80 >> col)) ? fg : bg;
        }
    }
    draw_and_wait(x, y, x + 8, y + 8, buf);
}

void
tdeck_display_print(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (s && *s) {
        tdeck_display_putchar(x, y, *s, fg, bg);
        x += 8;
        if (x + 8 > TDECK_LCD_WIDTH) break;
        s++;
    }
}
