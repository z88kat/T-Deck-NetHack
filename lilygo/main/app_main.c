/* app_main.c -- ESP-IDF entrypoint for tdeck-nethack.
 *
 * Phase 2: just spawn a FreeRTOS task pinned to core 1 that registers the
 * spec-discovery shim callback and calls nhmain().  No display, no
 * keyboard, no filesystem yet -- the goal is to see what windowport
 * functions NetHack invokes before it gets stuck waiting on input.
 *
 * Stack budget: NetHack's startup pulls in Lua which has fairly deep
 * function call chains.  32 KB is comfortable; we can shave once we've
 * profiled.
 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nethack.h"
#include "tdeck_display.h"
#include "tdeck_keyboard.h"
#include "tdeck_sdcard.h"

static const char *TAG = "app_main";

/* Mount the `nhdat` partition (a SPIFFS image built from project data/
 * dir, see top-level CMakeLists.txt) at /nethack -- matches HACKDIR
 * baked into libnh.a.  Path must be <= ESP_VFS_PATH_MAX = 15 chars. */
static void
mount_nhdat(void)
{
    /* Mount at /nethack (matches HACKDIR baked into libnh.a).  The path
     * must be <= ESP_VFS_PATH_MAX = 15 chars, so the longer /littlefs/...
     * path we'd originally planned is not usable. */
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/nethack",
        .partition_label = "nhdat",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nhdat mount failed: %s (0x%x)",
                 esp_err_to_name(err), err);
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("nhdat", &total, &used);
    ESP_LOGI(TAG, "nhdat mounted at /nethack (%u/%u bytes used)",
             (unsigned) used, (unsigned) total);
}

extern void nh_shim_callback(const char *name, void *ret_ptr,
                             const char *fmt, ...);

static void
nethack_task(void *arg)
{
    (void) arg;
    ESP_LOGI(TAG, "registering shim windowport callback");
    shim_graphics_set_callback(nh_shim_callback);

    ESP_LOGI(TAG, "entering nhmain()");
    /* nhmain dereferences argv[0] unconditionally to set gh.hname (see
     * sys/libnh/libnhmain.c:84), so we must pass at least one arg. */
    static char *argv[] = { (char *) "nethack", NULL };
    (void) nhmain(1, argv);

    /* nhmain() will block in the moveloop forever in steady state; if it
     * ever returns, something has gone wrong. */
    ESP_LOGW(TAG, "nhmain returned -- this should not happen");
    vTaskDelete(NULL);
}

void
app_main(void)
{
    ESP_LOGI(TAG, "tdeck-nethack boot");

    /* Quick environment dump so we can spot misconfigured PSRAM. */
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "chip: %s rev %d, %d core(s)",
             info.model == CHIP_ESP32S3 ? "ESP32-S3" : "unknown",
             info.revision, info.cores);
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "psram: %u bytes", (unsigned) esp_psram_get_size());
    } else {
        ESP_LOGW(TAG, "psram: not initialised");
    }
    ESP_LOGI(TAG, "free internal heap: %u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Phase 3a diagnostic: cycle through pure colours so we can tell
     * exactly what's working.  Expected sequence: RED, GREEN, BLUE,
     * WHITE, then the boot text on blue.  If a colour comes out wrong
     * (e.g. red appears as blue), the rgb_endian or invert_color setting
     * in tdeck_display.c needs flipping. */
    if (tdeck_display_init() == ESP_OK) {
        ESP_LOGI(TAG, "lcd test: RED");
        tdeck_display_fill(TDECK_COLOR_RED);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "lcd test: GREEN");
        tdeck_display_fill(TDECK_COLOR_GREEN);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "lcd test: BLUE");
        tdeck_display_fill(TDECK_COLOR_BLUE);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "lcd test: WHITE");
        tdeck_display_fill(TDECK_COLOR_WHITE);
        vTaskDelay(pdMS_TO_TICKS(500));
        tdeck_display_fill(TDECK_COLOR_BLUE);
        tdeck_display_print(8, 8, "T-Deck NetHack 5.0",
                            TDECK_COLOR_WHITE, TDECK_COLOR_BLUE);
        tdeck_display_print(8, 24, "Phase 3a: display alive.",
                            TDECK_COLOR_WHITE, TDECK_COLOR_BLUE);
        tdeck_display_print(8, 40, "booting...",
                            TDECK_COLOR_GREEN, TDECK_COLOR_BLUE);
        vTaskDelay(pdMS_TO_TICKS(1500));
        tdeck_display_fill(TDECK_COLOR_BLACK);

        /* Phase 3c diagnostic for the "first column clipped" bug.
         * If the leading 'A' below is missing, column 0 of the panel is
         * being hidden -- bezel, off-screen, or a column-address-offset
         * issue in the panel init.  If 'A' is visible, the wrap routine
         * in shim_callback's draw_status_message is to blame. */
        tdeck_display_print(0, 100,
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345678901",
                            TDECK_COLOR_WHITE, TDECK_COLOR_BLACK);
        tdeck_display_print(0, 112,
                            "if 'A' missing -> column 0 clipped",
                            TDECK_COLOR_GREEN, TDECK_COLOR_BLACK);
        vTaskDelay(pdMS_TO_TICKS(2500));
        tdeck_display_fill(TDECK_COLOR_BLACK);
    }

    /* Phase 4: bring the BlackBerry-style QWERTY up.  Polls I2C 0x55 in
     * a background task and queues keypresses for the shim to consume. */
    if (tdeck_keyboard_init() != ESP_OK) {
        ESP_LOGE(TAG, "keyboard init failed; nethack input will not work");
    }

    mount_nhdat();

    /* Phase 5: try to mount the SD card.  If a card is present and
     * FAT-formatted, point NetHack's writable prefixes there so saves
     * survive reboots.  If no card / unformatted / unreadable, log a
     * warning and continue -- the game will still play, just without
     * persistence. */
    if (tdeck_sdcard_mount("/sdcard") == ESP_OK) {
        /* NetHack's set_savefile_name() on UNIX prefixes the save filename
         * with "save/" (see src/files.c:1051), so create that subdir on
         * the card; otherwise create_savefile() fails with ENOENT and the
         * user sees "Cannot open save file." */
        if (mkdir("/sdcard/save", 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir /sdcard/save failed: %s", strerror(errno));
        }
        nh_set_savedir("/sdcard");
        ESP_LOGI(TAG, "saves will persist to /sdcard");
    } else {
        ESP_LOGW(TAG, "no SD card -- saves will not persist across reboots");
    }

    /* Pin NetHack to core 1; core 0 stays free for display / radio later. */
    BaseType_t rc = xTaskCreatePinnedToCore(
        nethack_task,
        "nethack",
        32 * 1024, /* stack bytes */
        NULL,
        5,         /* priority */
        NULL,
        1          /* core 1 */
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int) rc);
    }
}
