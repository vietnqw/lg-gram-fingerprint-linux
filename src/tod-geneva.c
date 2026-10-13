/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * libfprint TOD driver for the Goodix GXFP5A8B ("Geneva": GF3268 + GM168SEC) SPI sensor.
 *
 * The sensor is driven by the blocking, cancellable session code in sensor.c, run from a
 * worker thread (one GTask per capture or lift-wait); results are handled on the main loop.
 * Matching happens here (SIGFM features stored in the FpPrint), because the 108x88 sensor is
 * too small for libfprint's minutiae matcher.
 */
#define FP_COMPONENT "geneva"

#include <drivers_api.h>
#include <gmodule.h>
#include <time.h>

#include "error.h"
#include "hw.h"
#include "match.h"
#include "sensor.h"

#define ENROLL_STAGES   12
#define PRINT_VERSION   1        /* fpi-data: (q aay) = (version, serialized SIGFM features) */

struct _FpiDeviceGeneva
{
  FpDevice   parent;

  GvSensor  *sensor;             /* only touched from worker threads, one task at a time */
  gint64     sleep_mark;         /* CLOCK_BOOTTIME - CLOCK_MONOTONIC at last use */
  gboolean   finger_down;        /* last capture saw a finger that has not been lifted yet */

  /* current action */
  GPtrArray *templates;          /* enroll: GBytes of captured features */
};

G_DECLARE_FINAL_TYPE (FpiDeviceGeneva, fpi_device_geneva, FPI, DEVICE_GENEVA, FpDevice)
G_DEFINE_TYPE (FpiDeviceGeneva, fpi_device_geneva, FP_TYPE_DEVICE)

/* The only exported symbol: the TOD loader looks it up to get our driver type. */
G_MODULE_EXPORT GType fpi_tod_shared_driver_get_type (void);
G_MODULE_EXPORT GType
fpi_tod_shared_driver_get_type (void)
{
  return fpi_device_geneva_get_type ();
}

/* ---------- helpers ---------- */

static gint64
sleep_mark (void)
{
  struct timespec b, m;

  clock_gettime (CLOCK_BOOTTIME, &b);
  clock_gettime (CLOCK_MONOTONIC, &m);
  return (b.tv_sec - m.tv_sec) * G_USEC_PER_SEC + (b.tv_nsec - m.tv_nsec) / 1000;
}

/* The MCU loses its session over suspend; the boot/monotonic clock gap grows only then. */
static void
check_resumed (FpiDeviceGeneva *self)
{
  gint64 mark = sleep_mark ();

  if (ABS (mark - self->sleep_mark) > G_USEC_PER_SEC)
    {
      fp_dbg ("system was suspended; restarting the sensor session");
      gv_sensor_invalidate (self->sensor);
    }
  self->sleep_mark = mark;
}

static GError *
to_fp_error (GError *e)
{
  GError *out;

  if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED) || e->domain != GV_ERROR)
    return e;
  switch (e->code)
    {
    case GV_ERROR_NOT_PROVISIONED:
    case GV_ERROR_NO_FIRMWARE:
    case GV_ERROR_NO_CONFIG:
      out = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "%s", e->message);
      break;
    default:
      out = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "%s", e->message);
    }
  g_error_free (e);
  return out;
}

static GVariant *
templates_to_variant (GPtrArray *templates)
{
  GVariantBuilder b;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < templates->len; i++)
    g_variant_builder_add_value (&b, g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                               g_ptr_array_index (templates, i), TRUE));
  return g_variant_new ("(qaay)", PRINT_VERSION, &b);
}

/* Best score of probe against all templates of an enrolled print (-1 if unusable). */
static int
print_score (FpPrint *print, GvFeatures *probe)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) arr = NULL;
  guint16 version;
  int best = -1;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(qaay)")))
    return -1;
  g_variant_get (data, "(q@aay)", &version, &arr);
  if (version != PRINT_VERSION)
    return -1;
  for (gsize i = 0; i < g_variant_n_children (arr); i++)
    {
      g_autoptr(GVariant) t = g_variant_get_child_value (arr, i);
      gsize n;
      const guint8 *d = g_variant_get_fixed_array (t, &n, 1);
      g_autoptr(GvFeatures) f = gv_features_deserialize (d, n);

      if (f)
        best = MAX (best, gv_features_score (probe, f));
    }
  return best;
}

/* ---------- worker tasks ---------- */

typedef struct
{
  GvCaptureResult result;
  GvFeatures     *features;
} Capture;

static void
capture_free (Capture *c)
{
  gv_features_free (c->features);
  g_free (c);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Capture, capture_free)

static void
capture_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  FpiDeviceGeneva *self = source;
  g_autofree guint16 *frame = g_new (guint16, GV_IMG_PIXELS);
  g_autofree guint16 *bg = g_new (guint16, GV_IMG_PIXELS);
  Capture *c = g_new0 (Capture, 1);
  GError *error = NULL;

  check_resumed (self);
  /* A finger still resting from the previous attempt must not count as a new touch
   * (e.g. PAM retrying right after a no-match). */
  if (self->finger_down)
    {
      fp_dbg ("waiting for the previous finger to lift");
      if (!gv_sensor_wait_lift (self->sensor, cancellable, &error))
        {
          capture_free (c);
          g_task_return_error (task, error);
          return;
        }
      self->finger_down = FALSE;
    }
  if (!gv_sensor_capture (self->sensor, frame, bg, &c->result, cancellable, &error))
    {
      capture_free (c);
      g_task_return_error (task, error);
      return;
    }
  self->finger_down = TRUE;
  if (c->result == GV_CAPTURE_OK)
    {
      c->features = gv_features_extract (bg, frame);
      if (!c->features || gv_features_keypoints (c->features) < GV_MIN_KEYPOINTS)
        {
          fp_dbg ("too little detail in capture, asking for a retry");
          g_clear_pointer (&c->features, gv_features_free);
          c->result = GV_CAPTURE_RETRY;
        }
    }
  g_task_return_pointer (task, c, (GDestroyNotify) capture_free);
}

static void
lift_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  FpiDeviceGeneva *self = source;
  GError *error = NULL;

  check_resumed (self);
  if (gv_sensor_wait_lift (self->sensor, cancellable, &error))
    {
      self->finger_down = FALSE;
      g_task_return_boolean (task, TRUE);
    }
  else
    g_task_return_error (task, error);
}

static void
run (FpiDeviceGeneva *self, GTaskThreadFunc fn, GAsyncReadyCallback cb)
{
  g_autoptr(GTask) task = g_task_new (self, fpi_device_get_cancellable (FP_DEVICE (self)), cb, NULL);

  g_task_run_in_thread (task, fn);
}

static Capture *
capture_finish (FpiDeviceGeneva *self, GAsyncResult *res, GError **error)
{
  Capture *c = g_task_propagate_pointer (G_TASK (res), error);

  fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                           c && c->result == GV_CAPTURE_OK ? FP_FINGER_STATUS_PRESENT : FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_NEEDED);
  if (!c && *error)
    *error = to_fp_error (*error);
  return c;
}

static void
start_capture (FpiDeviceGeneva *self, GAsyncReadyCallback cb)
{
  fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED,
                                           FP_FINGER_STATUS_PRESENT);
  run (self, capture_thread, cb);
}

/* ---------- enroll ---------- */

static void enroll_capture_cb (GObject *src, GAsyncResult *res, gpointer user_data);

static void
enroll_lift_cb (GObject *src, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceGeneva *self = FPI_DEVICE_GENEVA (src);
  GError *error = NULL;

  fpi_device_report_finger_status_changes (FP_DEVICE (self), FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_PRESENT);
  if (!g_task_propagate_boolean (G_TASK (res), &error))
    {
      g_clear_pointer (&self->templates, g_ptr_array_unref);
      fpi_device_enroll_complete (FP_DEVICE (self), NULL, to_fp_error (error));
      return;
    }
  start_capture (self, enroll_capture_cb);
}

static void
enroll_capture_cb (GObject *src, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceGeneva *self = FPI_DEVICE_GENEVA (src);
  FpDevice *dev = FP_DEVICE (self);
  GError *error = NULL;
  g_autoptr(Capture) c = NULL;
  FpPrint *print = NULL;

  c = capture_finish (self, res, &error);
  if (!c)
    {
      g_clear_pointer (&self->templates, g_ptr_array_unref);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }
  if (c->result != GV_CAPTURE_OK)
    {
      fpi_device_enroll_progress (dev, self->templates->len, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      run (self, lift_thread, enroll_lift_cb);
      return;
    }

  g_ptr_array_add (self->templates, gv_features_serialize (c->features));
  fpi_device_get_enroll_data (dev, &print);
  fpi_device_enroll_progress (dev, self->templates->len, NULL, NULL);
  if (self->templates->len < ENROLL_STAGES)
    {
      run (self, lift_thread, enroll_lift_cb);
      return;
    }

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, "fpi-data", templates_to_variant (self->templates), NULL);
  g_clear_pointer (&self->templates, g_ptr_array_unref);
  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceGeneva *self = FPI_DEVICE_GENEVA (dev);

  self->templates = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  start_capture (self, enroll_capture_cb);
}

/* ---------- verify / identify ---------- */

static void
match_capture_cb (GObject *src, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceGeneva *self = FPI_DEVICE_GENEVA (src);
  FpDevice *dev = FP_DEVICE (self);
  gboolean verify = fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY;
  GError *error = NULL;
  g_autoptr(Capture) c = capture_finish (self, res, &error);

  if (!c || c->result != GV_CAPTURE_OK)
    {
      if (!error)
        error = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
      if (error->domain == FP_DEVICE_RETRY)
        {
          if (verify)
            fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
          else
            fpi_device_identify_report (dev, NULL, NULL, error);
          error = NULL;
        }
    }
  else if (verify)
    {
      FpPrint *enrolled = NULL;
      int score;

      fpi_device_get_verify_data (dev, &enrolled);
      score = print_score (enrolled, c->features);
      fp_dbg ("verify score %d (threshold %d)", score, GV_MATCH_THRESHOLD);
      fpi_device_verify_report (dev, score >= GV_MATCH_THRESHOLD ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                NULL, NULL);
    }
  else
    {
      GPtrArray *gallery = NULL;
      FpPrint *best = NULL;
      int best_score = -1;

      fpi_device_get_identify_data (dev, &gallery);
      for (guint i = 0; i < gallery->len; i++)
        {
          FpPrint *p = g_ptr_array_index (gallery, i);
          int s = print_score (p, c->features);

          if (s > best_score)
            best_score = s, best = p;
        }
      fp_dbg ("identify best score %d (threshold %d)", best_score, GV_MATCH_THRESHOLD);
      fpi_device_identify_report (dev, best_score >= GV_MATCH_THRESHOLD ? best : NULL, NULL, NULL);
    }

  if (verify)
    fpi_device_verify_complete (dev, error);
  else
    fpi_device_identify_complete (dev, error);
}

static void
dev_match (FpDevice *dev)
{
  start_capture (FPI_DEVICE_GENEVA (dev), match_capture_cb);
}

/* ---------- lifecycle ---------- */

static void
dev_probe (FpDevice *dev)
{
  fpi_device_probe_complete (dev, GV_ACPI_ID, NULL, NULL);
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceGeneva *self = FPI_DEVICE_GENEVA (dev);
  const char *spidev = fpi_device_get_udev_data (dev, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);
  guint8 config[GV_CFG_SIZE];
  GError *error = NULL;

  if (!gv_config_load (GV_CONFIG_PATH, config, &error) ||
      !(self->sensor = gv_sensor_new (spidev, GV_GPIOCHIP, GV_RESET_LINE, config, &error)))
    {
      fpi_device_open_complete (dev, to_fp_error (error));
      return;
    }
  self->sleep_mark = sleep_mark ();
  fpi_device_open_complete (dev, NULL);
}

static void
dev_close (FpDevice *dev)
{
  FpiDeviceGeneva *self = FPI_DEVICE_GENEVA (dev);

  g_clear_pointer (&self->sensor, gv_sensor_free);
  fpi_device_close_complete (dev, NULL);
}

static void
fpi_device_geneva_init (FpiDeviceGeneva *self)
{
}

static const FpIdEntry id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_id = GV_ACPI_ID },
  { .udev_types = 0 }
};

static void
fpi_device_geneva_class_init (FpiDeviceGenevaClass *klass)
{
  FpDeviceClass *dc = FP_DEVICE_CLASS (klass);

  dc->id = "geneva";
  dc->full_name = "Goodix GXFP5A8B fingerprint sensor";
  dc->type = FP_DEVICE_TYPE_UDEV;
  dc->id_table = id_table;
  dc->scan_type = FP_SCAN_TYPE_PRESS;
  dc->nr_enroll_stages = ENROLL_STAGES;
  dc->temp_hot_seconds = -1;     /* capacitive sensor, idles in MCU finger-detect: no heat issue */

  dc->probe = dev_probe;
  dc->open = dev_open;
  dc->close = dev_close;
  dc->enroll = dev_enroll;
  dc->verify = dev_match;
  dc->identify = dev_match;
}
