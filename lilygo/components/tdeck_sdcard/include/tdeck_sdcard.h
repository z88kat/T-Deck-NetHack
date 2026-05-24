/* tdeck_sdcard.h -- microSD slot on the LilyGo T-Deck.
 *
 * The SD shares the LCD's SPI bus (SPI2_HOST: SCLK=40, MOSI=41, MISO=38)
 * with its own chip-select on GPIO 39.  Bus must already be initialised
 * by the LCD driver before calling mount.
 *
 * Usage:
 *     if (tdeck_sdcard_mount("/sdcard") == ESP_OK) {
 *         // FAT VFS now available under "/sdcard"
 *     }
 *
 * Path constraint: base_path must be <= ESP_VFS_PATH_MAX (15 chars).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TDECK_SD_PIN_CS
#define TDECK_SD_PIN_CS    39
#endif
#ifndef TDECK_SD_PIN_MISO
#define TDECK_SD_PIN_MISO  38
#endif
#ifndef TDECK_SD_HOST
#define TDECK_SD_HOST      SPI2_HOST
#endif

/* Probe the card, mount the FAT filesystem at base_path, register with
 * VFS.  Returns ESP_OK on success.  Common failures:
 *   ESP_ERR_TIMEOUT      -- no card present
 *   ESP_FAIL             -- card present but FAT not mountable (unformatted)
 *   ESP_ERR_INVALID_STATE -- SPI bus not initialised yet (call LCD init first)
 */
esp_err_t tdeck_sdcard_mount(const char *base_path);

/* Returns the base path passed to the last successful mount, or NULL if
 * the SD has not been mounted. */
const char *tdeck_sdcard_base_path(void);

/* Unmount and free resources. */
void tdeck_sdcard_unmount(void);

/* Recursively delete all files under `dir_path` and any subdirectories,
 * then remove `dir_path` itself (unless it's the SD mount root).  Used to
 * scrub the NetHack save dir between dev iterations.  Returns the number
 * of files removed, or -1 on error. */
int tdeck_sdcard_wipe_dir(const char *dir_path);

#ifdef __cplusplus
}
#endif
