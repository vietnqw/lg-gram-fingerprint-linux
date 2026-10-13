/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * One-time setup:
 *  - extract: carve the sensor configuration and MCU firmware out of the Goodix Windows driver
 *    (3.1.100.320, downloaded from Microsoft Update or LG). Nothing proprietary ships here.
 *  - provision: pair the sensor with Linux. Windows seals a random TLS key into the MCU; we
 *    erase the app, write the well-known all-zero PSK (Goodix white-box format) and reflash
 *    the app firmware. Windows' LG driver re-pairs the sensor automatically if you go back.
 */
#pragma once

#include <gio/gio.h>
#include "sensor.h"

#define GV_FIRMWARE_SIZE 26456

/* Download Goodix's Windows driver package (3.1.100.320, as shipped by LG) from Microsoft
 * Update into a temporary file and verify it; returns its path. */
char    *gv_download_driver (GError **error);

/* Extract config + firmware from the driver: Microsoft's .cab, LG's .zip or Wbdi.dll. */
gboolean gv_extract (const char *driver, const char *out_dir, GError **error);

typedef void (*GvProgressFunc) (const char *step, double fraction, gpointer user_data);

gboolean gv_provision (GvSensor *sensor, const guint8 *firmware, gsize len,
                       GvProgressFunc progress, gpointer user_data, GError **error);
