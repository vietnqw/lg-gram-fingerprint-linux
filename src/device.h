/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Raw access to the sensor: spidev transfers and the reset GPIO. Blocking calls.
 *   write : F0 3D 00 <len BE16> <pack> (padded to an even length; the MCU drops the last byte
 *           of odd-length writes; the pad is counted in the SPI length, not the pack length)
 *   read  : F0 3B 00, then F1 + clock out the mailbox
 */
#pragma once

#include <glib.h>

#define GV_SPI_SPEED_HZ 4000000

typedef struct _GvDevice GvDevice;

GvDevice *gv_device_open (const char *spidev, const char *gpiochip, guint reset_line, GError **error);
void      gv_device_close (GvDevice *dev);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvDevice, gv_device_close)

/* Pulse the reset line and give the MCU time to boot. */
gboolean  gv_device_reset (GvDevice *dev, GError **error);
gboolean  gv_device_write (GvDevice *dev, const guint8 *data, gsize len, GError **error);
gboolean  gv_device_read (GvDevice *dev, guint8 *buf, gsize len, GError **error);
