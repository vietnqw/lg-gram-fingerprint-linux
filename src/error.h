/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

#define GV_ERROR (gv_error_quark ())
GQuark gv_error_quark (void);

typedef enum {
  GV_ERROR_IO,               /* spidev / gpio failure */
  GV_ERROR_PROTO,            /* unexpected or malformed reply, TLS failure */
  GV_ERROR_TIMEOUT,          /* the chip did not answer in time */
  GV_ERROR_NOT_PROVISIONED,  /* PSK is not ours (e.g. Windows re-keyed it): run provisioning */
  GV_ERROR_NO_FIRMWARE,      /* app firmware not running (chip in bootloader) */
  GV_ERROR_NO_CONFIG,        /* sensor config missing: run `lg-gram-fp-tool extract` */
} GvError;

/* Set error and return NULL (for one-line error returns). */
gpointer gv_error_null (GError **error, GvError code, const char *fmt, ...) G_GNUC_PRINTF (3, 4);
/* Set error and return FALSE. */
gboolean gv_error_false (GError **error, GvError code, const char *fmt, ...) G_GNUC_PRINTF (3, 4);
