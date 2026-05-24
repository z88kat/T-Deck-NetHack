/* tdeck_sdcard.c -- mount the T-Deck microSD over the shared LCD SPI bus.
 *
 * SD card is on SPI2_HOST (same as LCD).  CS=39, MISO=38, MOSI=41,
 * SCK=40.  The LCD driver initialises the bus with MISO=38 so we can
 * attach as a second device here.
 *
 * FAT filesystem is mounted at the caller-supplied base_path (e.g.
 * "/sdcard").  Path must be <= 15 chars (ESP_VFS_PATH_MAX).
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tdeck_sdcard.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

static const char *TAG = "tdeck_sdcard";

static sdmmc_card_t *s_card  = NULL;
static char          s_base_path[16] = "";

esp_err_t
tdeck_sdcard_mount(const char *base_path)
{
    if (!base_path || !*base_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = TDECK_SD_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = TDECK_SD_PIN_CS;
    slot.host_id = TDECK_SD_HOST;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD at %s (CS=%d)", base_path, TDECK_SD_PIN_CS);
    esp_err_t err = esp_vfs_fat_sdspi_mount(base_path, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s (0x%x)",
                 esp_err_to_name(err), err);
        s_card = NULL;
        return err;
    }

    strncpy(s_base_path, base_path, sizeof(s_base_path) - 1);
    s_base_path[sizeof(s_base_path) - 1] = '\0';

    ESP_LOGI(TAG, "SD card mounted: name=%s, size=%llu MB",
             s_card->cid.name,
             ((uint64_t) s_card->csd.capacity) * s_card->csd.sector_size
                 / (1024 * 1024));
    return ESP_OK;
}

const char *
tdeck_sdcard_base_path(void)
{
    return s_card ? s_base_path : NULL;
}

void
tdeck_sdcard_unmount(void)
{
    if (!s_card) return;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(s_base_path, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(err));
    }
    s_card = NULL;
    s_base_path[0] = '\0';
}

int
tdeck_sdcard_wipe_dir(const char *dir_path)
{
    if (!dir_path || !*dir_path) return -1;
    DIR *d = opendir(dir_path);
    if (!d) {
        ESP_LOGW(TAG, "wipe: opendir(%s) failed", dir_path);
        return -1;
    }

    int removed = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        /* FATFS only does 8.3 short names plus a long-name extension, but
         * newlib's dirent reserves NAME_MAX (256) for d_name, which trips
         * gcc's -Wformat-truncation if `path` is sized only for the
         * realistic case.  Size for the worst case. */
        char path[320];
        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
        if (n < 0 || n >= (int) sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "wipe: stat(%s) failed", path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            int sub = tdeck_sdcard_wipe_dir(path);
            if (sub >= 0) removed += sub;
            if (rmdir(path) != 0) {
                ESP_LOGW(TAG, "wipe: rmdir(%s) failed", path);
            }
        } else {
            if (unlink(path) == 0) {
                removed++;
            } else {
                ESP_LOGW(TAG, "wipe: unlink(%s) failed", path);
            }
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "wipe: %s -> %d files removed", dir_path, removed);
    return removed;
}
