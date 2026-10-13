/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * One-time setup:
 *  - extract: carve the sensor configuration and MCU firmware out of LG's Windows driver
 *    (GD_FP_W106 3.1.100.320, which users download from LG). Nothing proprietary ships here.
 *  - provision: pair the sensor with Linux. Windows seals a random TLS key into the MCU; we
 *    erase the app, write the well-known all-zero PSK (Goodix white-box format) and reflash
 *    the app firmware. Windows' LG driver re-pairs the sensor automatically if you go back.
 */
#pragma once

#include <gio/gio.h>
#include "sensor.h"

#define GV_FIRMWARE_SIZE 26456

gboolean gv_extract (const char *lg_driver, const char *out_dir, GError **error);

typedef void (*GvProgressFunc) (const char *step, double fraction, gpointer user_data);

gboolean gv_provision (GvSensor *sensor, const guint8 *firmware, gsize len,
                       GvProgressFunc progress, gpointer user_data, GError **error);
