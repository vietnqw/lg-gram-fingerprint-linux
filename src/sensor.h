/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Sensor session: TLS bring-up, finger detection and frame capture. Blocking and
 * cancellable; used by the libfprint driver (from a worker thread) and by geneva-fp-tool.
 */
#pragma once

#include <gio/gio.h>
#include "proto.h"

typedef struct _GvSensor GvSensor;

typedef enum {
  GV_CAPTURE_OK,          /* frame holds a finger image */
  GV_CAPTURE_RETRY,       /* bad contact or stale background (refreshed); ask to try again */
} GvCaptureResult;

GvSensor *gv_sensor_new (const char *spidev, const char *gpiochip, guint reset_line,
                         const guint8 *config, GError **error);
void      gv_sensor_free (GvSensor *sensor);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvSensor, gv_sensor_free)

/* Forget the TLS session (after suspend, errors); the next call re-initialises the chip. */
void      gv_sensor_invalidate (GvSensor *sensor);

/* Wait for a finger to arrive, then capture it. The result also carries the background frame
 * the image was taken against (both GV_IMG_PIXELS 12-bit pixels). */
gboolean  gv_sensor_capture (GvSensor *sensor, guint16 *frame, guint16 *background,
                             GvCaptureResult *result, GCancellable *cancellable, GError **error);

/* Wait until the finger is lifted (and refresh the background frame). */
gboolean  gv_sensor_wait_lift (GvSensor *sensor, GCancellable *cancellable, GError **error);

/* Low-level access for provisioning: a plaintext command and its reply (timeout 0 = wait
 * until cancelled), and a hardware reset. Both drop the TLS session. */
GBytes   *gv_sensor_command (GvSensor *sensor, guint8 cmd, const guint8 *payload, gsize len,
                             guint timeout_ms, GCancellable *cancellable, GError **error);
gboolean  gv_sensor_reset (GvSensor *sensor, GError **error);

/* Firmware version string and whether the chip holds our PSK (works in the app and in the
 * IAP bootloader); optionally reset the chip first. */
gboolean  gv_sensor_status (GvSensor *sensor, gboolean reset, char **firmware, gboolean *psk_ok,
                            GCancellable *cancellable, GError **error);

/* Mean background-minus-frame signal; a real finger gives several hundred counts. */
double    gv_signal (const guint16 *background, const guint16 *frame);

/* PMK hash the chip reports (0xE4) when provisioned with our all-zero PSK. */
extern const guint8 gv_pmk_hash_psk0[32];
