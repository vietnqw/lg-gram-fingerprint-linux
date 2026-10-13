/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "sensor.h"
#include "device.h"
#include "error.h"
#include "tls.h"

#include <string.h>

#define POLL_MS          20
#define CMD_TIMEOUT_MS   2000
#define MIN_SIGNAL       150.0      /* mean(bg - frame) of a real touch is ~600-850 */

const guint8 gv_pmk_hash_psk0[32] = {
  0x66, 0x68, 0x7a, 0xad, 0xf8, 0x62, 0xbd, 0x77, 0x6c, 0x8f, 0xc1, 0x8b, 0x8e, 0x9f, 0x8e, 0x20,
  0x08, 0x97, 0x14, 0x85, 0x6e, 0xe2, 0x33, 0xb3, 0x90, 0x2a, 0x59, 0x1d, 0x0d, 0x5f, 0x29, 0x25,
};

struct _GvSensor
{
  GvDevice *dev;
  guint8    config[GV_CFG_SIZE];
  GvTls    *tls;                       /* NULL = no session */
  guint8    fdt_base[2 * GV_FDT_CHANNELS];
  guint16   bg[GV_IMG_PIXELS];
  gboolean  have_bg;
};

/* ---------- mailbox helpers ---------- */

static gboolean
nap (guint ms, GCancellable *cancellable, GError **error)
{
  g_usleep (ms * 1000);
  return !g_cancellable_set_error_if_cancelled (cancellable, error);
}

static gboolean
send_msg (GvSensor *s, guint8 cmd, const guint8 *payload, gsize len, GError **error)
{
  g_autofree guint8 *msg = g_malloc (len + 4);
  g_autofree guint8 *pack = g_malloc (len + 8);
  gsize n = gv_msg_encode (cmd, payload, len, msg);

  n = gv_pack_encode (GV_FLAG_PLAIN, msg, n, pack);
  return gv_device_write (s->dev, pack, n, error);
}

static gboolean
send_tls (GvSensor *s, GBytes *records, GError **error)
{
  gsize len;
  const guint8 *data = g_bytes_get_data (records, &len);
  g_autofree guint8 *pack = g_malloc (len + 4);

  return gv_device_write (s->dev, pack, gv_pack_encode (GV_FLAG_TLS, data, len, pack), error);
}

/* Send a plaintext command and wait for the chip's reply to it (skipping its ACK).
 * timeout_ms == 0 waits until cancelled. */
static GBytes *
command (GvSensor *s, guint8 cmd, const guint8 *payload, gsize len, guint timeout_ms,
         GCancellable *cancellable, GError **error)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
  guint8 buf[512];

  if (!send_msg (s, cmd, payload, len, error))
    return NULL;
  for (;;)
    {
      guint8 flag, rcmd;
      const guint8 *body, *data;
      gsize body_len, data_len;

      if (!nap (POLL_MS, cancellable, error))
        return NULL;
      if (timeout_ms && g_get_monotonic_time () > deadline)
        return gv_error_null (error, GV_ERROR_TIMEOUT, "no reply to command 0x%02x", cmd);
      if (!gv_device_read (s->dev, buf, sizeof buf, error))
        return NULL;
      if (!gv_pack_decode (buf, sizeof buf, &flag, &body, &body_len) || flag != GV_FLAG_PLAIN)
        continue;
      if (!gv_msg_decode (body, body_len, &rcmd, &data, &data_len) || rcmd != cmd)
        continue;
      return g_bytes_new (data, data_len);
    }
}

/* ---------- session bring-up ---------- */

static gboolean
check_firmware_and_psk (GvSensor *s, char **firmware, gboolean *psk_ok,
                        GCancellable *cancellable, GError **error)
{
  static const guint8 zero2[2] = { 0, 0 };
  /* 0xE4 PSK read: length 32, offset 0, flags 0xbb020001 (PMK hash), 0 */
  static const guint8 psk_req[16] = { 32, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x02, 0xbb, 0, 0, 0, 0 };
  g_autoptr(GBytes) fw = NULL, pmk = NULL;
  const guint8 *d;
  gsize n;

  fw = command (s, GV_CMD_FW_VERSION, zero2, 2, CMD_TIMEOUT_MS, cancellable, error);
  if (!fw)
    return FALSE;
  d = g_bytes_get_data (fw, &n);
  *firmware = g_strndup ((const char *) d, n);

  /* Works both in the app and in the IAP bootloader. */
  pmk = command (s, GV_CMD_PSK_READ, psk_req, sizeof psk_req, CMD_TIMEOUT_MS, cancellable, error);
  if (!pmk)
    return FALSE;
  d = g_bytes_get_data (pmk, &n);
  *psk_ok = n >= 9 + 32 && d[0] == 0 && memcmp (d + 9, gv_pmk_hash_psk0, 32) == 0;
  return TRUE;
}

/* The chip's TX mailbox survives a reset, so the previous session's packs may still be there:
 * only accept records that were not in the mailbox before we asked. */
static gboolean
mailbox_record (const guint8 *buf, gsize len, GPtrArray *seen, GByteArray *out)
{
  gboolean got = FALSE;
  gsize o = 0;

  while (o + 4 <= len)
    {
      guint8 flag;
      const guint8 *body;
      gsize body_len;
      gboolean known = FALSE;

      if (!gv_pack_decode (buf + o, len - o, &flag, &body, &body_len))
        break;
      if (flag == GV_FLAG_TLS && body_len)
        {
          GBytes *b = g_bytes_new (body, body_len);

          for (guint i = 0; i < seen->len && !known; i++)
            known = g_bytes_equal (b, g_ptr_array_index (seen, i));
          if (known)
            g_bytes_unref (b);
          else
            {
              g_ptr_array_add (seen, b);
              g_byte_array_append (out, body, body_len);
              got = TRUE;
            }
        }
      o += 4 + body_len;
    }
  return got;
}

static gboolean
handshake (GvSensor *s, GCancellable *cancellable, GError **error)
{
  static const guint8 zero2[2] = { 0, 0 };
  g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  g_autoptr(GByteArray) hello = g_byte_array_new ();
  g_autoptr(GByteArray) flight = g_byte_array_new ();
  g_autoptr(GByteArray) stale = g_byte_array_new ();
  g_autoptr(GBytes) out = NULL;
  g_autoptr(GvTls) tls = NULL;
  guint8 buf[2048];
  gint64 deadline;
  guint records = 0;

  /* Remember what is already in the mailbox, then ask for a new session (ClientHello). */
  if (!gv_device_read (s->dev, buf, sizeof buf, error))
    return FALSE;
  mailbox_record (buf, sizeof buf, seen, stale);
  if (!send_msg (s, GV_CMD_REQUEST_TLS, zero2, 2, error))
    return FALSE;
  deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (hello->len == 0)
    {
      if (!nap (POLL_MS, cancellable, error) || !gv_device_read (s->dev, buf, sizeof buf, error))
        return FALSE;
      mailbox_record (buf, sizeof buf, seen, hello);
      if (hello->len && (hello->len < 5 || hello->data[0] != GV_TLS_CT_HANDSHAKE))
        g_byte_array_set_size (hello, 0);
      if (g_get_monotonic_time () > deadline)
        return gv_error_false (error, GV_ERROR_TIMEOUT, "no ClientHello from sensor");
    }

  tls = gv_tls_new ((const guint8[32]) { 0 }, 32);
  out = gv_tls_on_client_hello (tls, hello->data, hello->len, error);
  if (!out || !send_tls (s, out, error))
    return FALSE;
  g_clear_pointer (&out, g_bytes_unref);

  /* ClientKeyExchange, ChangeCipherSpec, Finished: one record per pack. */
  deadline = g_get_monotonic_time () + 4 * G_USEC_PER_SEC;
  while (records < 3)
    {
      guint before;

      if (!nap (POLL_MS, cancellable, error) || !gv_device_read (s->dev, buf, sizeof buf, error))
        return FALSE;
      before = seen->len;
      mailbox_record (buf, sizeof buf, seen, flight);
      records += seen->len - before;
      if (g_get_monotonic_time () > deadline)
        return gv_error_false (error, GV_ERROR_TIMEOUT, "incomplete TLS flight from sensor");
    }
  out = gv_tls_on_client_flight (tls, flight->data, flight->len, error);
  if (!out || !send_tls (s, out, error))
    return FALSE;

  /* 0xD4 reports the chip's mbedTLS state; imaging only works once it is HANDSHAKE_OVER. */
  for (int i = 0; i < 10; i++)
    {
      g_autoptr(GBytes) st = NULL;

      if (!nap (10, cancellable, error))
        return FALSE;
      st = command (s, GV_CMD_TLS_STATE, zero2, 2, CMD_TIMEOUT_MS, cancellable, error);
      if (!st)
        return FALSE;
      if (g_bytes_get_size (st) && ((const guint8 *) g_bytes_get_data (st, NULL))[0] >= GV_TLS_STATE_OVER)
        {
          s->tls = g_steal_pointer (&tls);
          return TRUE;
        }
    }
  return gv_error_false (error, GV_ERROR_PROTO, "sensor did not finish the TLS handshake");
}

static gboolean
upload_config (GvSensor *s, const guint8 *otp, gsize otp_len,
               GCancellable *cancellable, GError **error)
{
  guint8 cfg[GV_CFG_SIZE];
  g_autoptr(GBytes) r = NULL;
  guint16 tcode;

  memcpy (cfg, s->config, sizeof cfg);
  if (gv_otp_tcode (otp, otp_len, &tcode))
    gv_cfg_patch_tcode (cfg, tcode);
  r = command (s, GV_CMD_UPLOAD_CFG, cfg, sizeof cfg, CMD_TIMEOUT_MS, cancellable, error);
  if (!r)
    return FALSE;
  if (g_bytes_get_size (r) < 1 || ((const guint8 *) g_bytes_get_data (r, NULL))[0] != 1)
    return gv_error_false (error, GV_ERROR_PROTO, "sensor rejected the configuration");
  return TRUE;
}

static gboolean
start (GvSensor *s, GCancellable *cancellable, GError **error)
{
  static const guint8 zero2[2] = { 0, 0 };
  g_autofree char *fw = NULL;
  g_autoptr(GBytes) otp = NULL;
  gboolean psk_ok = FALSE;

  if (s->tls)
    return TRUE;
  s->have_bg = FALSE;
  if (!gv_device_reset (s->dev, error) ||
      !check_firmware_and_psk (s, &fw, &psk_ok, cancellable, error))
    return FALSE;
  if (!g_str_has_prefix (fw, "GF3268"))
    return gv_error_false (error, GV_ERROR_NO_FIRMWARE,
                           "sensor is not running its application firmware (\"%s\"); "
                           "run `sudo geneva-fp-tool provision`", fw);
  if (!psk_ok)
    return gv_error_false (error, GV_ERROR_NOT_PROVISIONED,
                           "sensor is paired with another OS (e.g. Windows re-keyed it); "
                           "run `sudo geneva-fp-tool provision`");
  otp = command (s, GV_CMD_READ_OTP, zero2, 2, CMD_TIMEOUT_MS, cancellable, error);
  if (!otp || !handshake (s, cancellable, error))
    return FALSE;
  if (!upload_config (s, g_bytes_get_data (otp, NULL), g_bytes_get_size (otp), cancellable, error))
    {
      gv_sensor_invalidate (s);
      return FALSE;
    }
  g_debug ("sensor session ready (%s)", fw);
  return TRUE;
}

/* ---------- frames ---------- */

static gboolean
grab (GvSensor *s, guint16 *px, GCancellable *cancellable, GError **error)
{
  static const guint8 req[2] = { 0x01, 0x00 };
  gint64 deadline = g_get_monotonic_time () + 4 * G_USEC_PER_SEC;
  guint8 head[16];

  if (!send_msg (s, GV_CMD_GET_IMAGE, req, sizeof req, error))
    return FALSE;
  for (;;)
    {
      if (!nap (30, cancellable, error) || !gv_device_read (s->dev, head, sizeof head, error))
        return FALSE;
      if (head[0] == GV_FLAG_TLS_DATA && ((head[0] + head[1] + head[2]) & 0xff) == head[3])
        break;
      if (head[0] == GV_FLAG_PLAIN && head[4] == GV_CMD_REQUEST_TLS)
        return gv_error_false (error, GV_ERROR_PROTO, "sensor asked for a new TLS session");
      if (g_get_monotonic_time () > deadline)
        return gv_error_false (error, GV_ERROR_TIMEOUT, "no image from sensor");
    }

  {
    /* The B2 pack is [9-byte prefix][TLS record]; in our read the record starts ~1.4 KB in,
     * so read generously and locate it by its header. */
    gsize len = 4 + (head[1] | (head[2] << 8)) + 4096;
    g_autofree guint8 *buf = g_malloc (len);
    g_autoptr(GBytes) img = NULL;

    if (!gv_device_read (s->dev, buf, len, error))
      return FALSE;
    for (gsize j = 4; j + 5 <= len; j++)
      {
        gsize rl;

        if (buf[j] != GV_TLS_CT_APPDATA || buf[j + 1] != 3 || buf[j + 2] != 3)
          continue;
        rl = (buf[j + 3] << 8) | buf[j + 4];
        if (rl < GV_IMG_PLAIN || j + 5 + rl > len)
          continue;
        img = gv_tls_decrypt (s->tls, GV_TLS_CT_APPDATA, buf + j + 5, rl, error);
        if (!img)
          return FALSE;
        if (g_bytes_get_size (img) < GV_IMG_PACKED)
          return gv_error_false (error, GV_ERROR_PROTO, "short image (%zu bytes)", g_bytes_get_size (img));
        gv_image_decode (g_bytes_get_data (img, NULL), px);
        return TRUE;
      }
  }
  return gv_error_false (error, GV_ERROR_PROTO, "image record not found");
}

static gboolean
background (GvSensor *s, GCancellable *cancellable, GError **error)
{
  guint8 req[2 + 2 * GV_FDT_CHANNELS] = { 0x0d, 0x01 };
  guint16 ch[GV_FDT_CHANNELS];
  g_autoptr(GBytes) r = NULL;

  for (int i = 0; i < GV_FDT_CHANNELS; i++)
    req[2 + 2 * i] = 0x80;
  r = command (s, GV_CMD_FDT_MANUAL, req, sizeof req, CMD_TIMEOUT_MS, cancellable, error);
  if (!r)
    return FALSE;
  if (!gv_fdt_channels (g_bytes_get_data (r, NULL), g_bytes_get_size (r), ch))
    return gv_error_false (error, GV_ERROR_PROTO, "bad finger-detect reply");
  gv_fdt_base (ch, s->fdt_base);
  if (!grab (s, s->bg, cancellable, error))
    return FALSE;
  s->have_bg = TRUE;
  g_debug ("background frame taken");
  return TRUE;
}

/* Arm finger detection and block until it fires: 0x32 = finger arrives, 0x34 = finger leaves.
 * 0x36 with the current base goes first; its reply also flushes stale FDT replies. */
static gboolean
wait_fdt (GvSensor *s, guint8 cmd, GCancellable *cancellable, GError **error)
{
  guint8 req[2 + 2 * GV_FDT_CHANNELS] = { 0x0d, 0x01 };
  g_autoptr(GBytes) r = NULL;

  memcpy (req + 2, s->fdt_base, sizeof s->fdt_base);
  r = command (s, GV_CMD_FDT_MANUAL, req, sizeof req, CMD_TIMEOUT_MS, cancellable, error);
  if (!r)
    return FALSE;
  g_clear_pointer (&r, g_bytes_unref);
  req[0] = cmd == GV_CMD_FDT_DOWN ? 0x0c : 0x0e;
  g_debug ("waiting for finger %s", cmd == GV_CMD_FDT_DOWN ? "down" : "up");
  r = command (s, cmd, req, sizeof req, 0, cancellable, error);
  return r != NULL;
}

/* ---------- public API ---------- */

GvSensor *
gv_sensor_new (const char *spidev, const char *gpiochip, guint reset_line,
               const guint8 *config, GError **error)
{
  g_autoptr(GvSensor) s = g_new0 (GvSensor, 1);

  s->dev = gv_device_open (spidev, gpiochip, reset_line, error);
  if (!s->dev)
    return NULL;
  memcpy (s->config, config, GV_CFG_SIZE);
  return g_steal_pointer (&s);
}

void
gv_sensor_free (GvSensor *s)
{
  if (!s)
    return;
  gv_sensor_invalidate (s);
  gv_device_close (s->dev);
  g_free (s);
}

void
gv_sensor_invalidate (GvSensor *s)
{
  g_clear_pointer (&s->tls, gv_tls_free);
  s->have_bg = FALSE;
}

double
gv_signal (const guint16 *bg, const guint16 *frame)
{
  gint64 sum = 0;

  for (gsize i = 0; i < GV_IMG_PIXELS; i++)
    sum += (gint) bg[i] - (gint) frame[i];
  return (double) sum / GV_IMG_PIXELS;
}

/* Errors the user has to act on, or cancellation, are not worth a session retry. */
static gboolean
cancelled (const GError *e)
{
  return g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static gboolean
retryable (const GError *e)
{
  return e->domain == GV_ERROR &&
         e->code != GV_ERROR_NOT_PROVISIONED && e->code != GV_ERROR_NO_FIRMWARE &&
         e->code != GV_ERROR_NO_CONFIG;
}

static gboolean
capture_once (GvSensor *s, guint16 *frame, guint16 *bg_out, GvCaptureResult *result,
              GCancellable *cancellable, GError **error)
{
  double sig;

  if (!start (s, cancellable, error))
    return FALSE;
  if (!s->have_bg && !background (s, cancellable, error))
    return FALSE;
  if (!wait_fdt (s, GV_CMD_FDT_DOWN, cancellable, error) || !grab (s, frame, cancellable, error))
    return FALSE;

  sig = gv_signal (s->bg, frame);
  g_debug ("finger signal %.0f", sig);
  if (sig < -MIN_SIGNAL)
    {
      /* The background was taken with a finger on the sensor; take a fresh one now. */
      *result = GV_CAPTURE_RETRY;
      return background (s, cancellable, error);
    }
  memcpy (bg_out, s->bg, sizeof s->bg);
  *result = sig < MIN_SIGNAL ? GV_CAPTURE_RETRY : GV_CAPTURE_OK;
  return TRUE;
}

gboolean
gv_sensor_capture (GvSensor *s, guint16 *frame, guint16 *bg_out, GvCaptureResult *result,
                   GCancellable *cancellable, GError **error)
{
  g_autoptr(GError) first = NULL;

  if (capture_once (s, frame, bg_out, result, cancellable, &first))
    return TRUE;
  if (!cancelled (first))
    gv_sensor_invalidate (s);      /* a cancelled wait leaves a usable session behind */
  if (!retryable (first))
    {
      g_propagate_error (error, g_steal_pointer (&first));
      return FALSE;
    }
  g_debug ("capture failed (%s); restarting the sensor session", first->message);
  if (capture_once (s, frame, bg_out, result, cancellable, error))
    return TRUE;
  if (error && *error && !cancelled (*error))
    gv_sensor_invalidate (s);
  return FALSE;
}

gboolean
gv_sensor_wait_lift (GvSensor *s, GCancellable *cancellable, GError **error)
{
  g_autoptr(GError) e = NULL;

  if (!s->tls || !s->have_bg)
    return TRUE;                          /* no session: nothing is armed */
  if (wait_fdt (s, GV_CMD_FDT_UP, cancellable, &e) && background (s, cancellable, &e))
    return TRUE;
  if (!cancelled (e))
    gv_sensor_invalidate (s);
  g_propagate_error (error, g_steal_pointer (&e));
  return FALSE;
}

gboolean
gv_sensor_status (GvSensor *s, gboolean reset, char **firmware, gboolean *psk_ok,
                  GCancellable *cancellable, GError **error)
{
  gv_sensor_invalidate (s);
  *psk_ok = FALSE;
  if (reset && !gv_device_reset (s->dev, error))
    return FALSE;
  return check_firmware_and_psk (s, firmware, psk_ok, cancellable, error);
}

GBytes *
gv_sensor_command (GvSensor *s, guint8 cmd, const guint8 *payload, gsize len,
                   guint timeout_ms, GCancellable *cancellable, GError **error)
{
  gv_sensor_invalidate (s);
  return command (s, cmd, payload, len, timeout_ms, cancellable, error);
}

gboolean
gv_sensor_reset (GvSensor *s, GError **error)
{
  gv_sensor_invalidate (s);
  return gv_device_reset (s->dev, error);
}
