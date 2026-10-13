/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Platform facts for the supported machine (LG gram 15Z90N, ACPI GXFP5A8B) and the location
 * of the per-machine sensor configuration extracted from LG's Windows driver.
 */
#pragma once

#include <glib.h>

#define GV_ACPI_ID          "GXFP5A8B"
#define GV_GPIOCHIP         "/dev/gpiochip0"   /* INT3455:00 */
#define GV_RESET_LINE       46                 /* ACPI pad GFPS */
#define GV_DATA_DIR         "/var/lib/geneva-fp"
#define GV_CONFIG_PATH      GV_DATA_DIR "/sensor-config.bin"
#define GV_FIRMWARE_PATH    GV_DATA_DIR "/firmware.bin"

/* Find /dev/spidevX.Y bound to the GXFP5A8B SPI device (NULL if spidev is not bound). */
char    *gv_find_spidev (void);

/* Load and validate the 256-byte sensor configuration. */
gboolean gv_config_load (const char *path, guint8 *config, GError **error);
