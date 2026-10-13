/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "error.h"

G_DEFINE_QUARK (lg-gram-fp-error-quark, gv_error)

gpointer
gv_error_null (GError **error, GvError code, const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  if (error)
    *error = g_error_new_valist (GV_ERROR, code, fmt, ap);
  va_end (ap);
  return NULL;
}

gboolean
gv_error_false (GError **error, GvError code, const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  if (error)
    *error = g_error_new_valist (GV_ERROR, code, fmt, ap);
  va_end (ap);
  return FALSE;
}
