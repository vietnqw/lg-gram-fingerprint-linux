/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "provision.h"
#include "error.h"
#include "hw.h"

#include <string.h>

/* LG GD_FP_W106 3.1.100.320 (2019-09-10): Wbdi.dll and the blobs we take from it. */
static const char WBDI_SHA256[] = "7437c02cabd69f25eaeced55f901b965625b9fcbb7fb45ebd2c32a5d9e4e609a";
#define CONFIG_OFFSET    0x11a6c0      /* VA 0x18011bcc0, GF3268 config (stored checksum is stale) */
#define FIRMWARE_OFFSET  0xf86fa       /* VA 0x1800f9cfa, GF3268_GM168SEC_APP_20011 image */
static const char CONFIG_SHA256[] = "1d661034be3adda17e7c017389ec4fb12a574f52e2f16dc36a234e3fd0b4bda5";
static const char FIRMWARE_SHA256[] = "226f0173ba2c61ba6df92f3d1ee15d3e2096646e78752c1a2d3314ccbda0c92f";

/* Goodix white-box encoding of the all-zero PSK, as consumed by the MCU's 0xE0 command
 * (shared across Goodix MCUs; published in goodix-fp-dump, MIT). */
static const guint8 PSK_WHITE_BOX[96] = {
  0xec, 0x35, 0xae, 0x3a, 0xbb, 0x45, 0xed, 0x3f, 0x12, 0xc4, 0x75, 0x1f, 0x1e, 0x5c, 0x2c, 0xc0,
  0x5b, 0x3c, 0x54, 0x52, 0xe9, 0x10, 0x4d, 0x9f, 0x2a, 0x31, 0x18, 0x64, 0x4f, 0x37, 0xa0, 0x4b,
  0x6f, 0xd6, 0x6b, 0x1d, 0x97, 0xcf, 0x80, 0xf1, 0x34, 0x5f, 0x76, 0xc8, 0x4f, 0x03, 0xff, 0x30,
  0xbb, 0x51, 0xbf, 0x30, 0x8f, 0x2a, 0x98, 0x75, 0xc4, 0x1e, 0x65, 0x92, 0xcd, 0x2a, 0x2f, 0x9e,
  0x60, 0x80, 0x9b, 0x17, 0xb5, 0x31, 0x60, 0x37, 0xb6, 0x9b, 0xb2, 0xfa, 0x5d, 0x4c, 0x8a, 0xc3,
  0x1e, 0xdb, 0x33, 0x94, 0x04, 0x6e, 0xc0, 0x6b, 0xbd, 0xac, 0xc5, 0x7d, 0xa6, 0xa7, 0x56, 0xc5,
};
static const guint8 PSK_PRE_FLAGS[10] = { 0x56, 0xa5, 0xbb, 0x95, 0x6b, 0x7c, 0x8d, 0x9e, 0x00, 0x00 };
#define PSK_FLAGS_WRITE  0xbb010003

/* ---------- extract ---------- */

static GBytes *
read_wbdi (const char *path, GError **error)
{
  g_autofree char *data = NULL;
  g_autofree char *lower = g_ascii_strdown (path, -1);
  gsize len;

  if (g_str_has_suffix (lower, ".zip"))
    {
      g_autoptr(GSubprocess) p = NULL;
      g_autoptr(GBytes) out = NULL;

      p = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error,
                            "unzip", "-p", path, "Wbdi.dll", NULL);
      if (!p || !g_subprocess_communicate (p, NULL, NULL, &out, NULL, error))
        return NULL;
      if (!g_subprocess_get_successful (p) || !out || g_bytes_get_size (out) == 0)
        return gv_error_null (error, GV_ERROR_IO, "%s: no Wbdi.dll inside (is `unzip` installed?)", path);
      return g_steal_pointer (&out);
    }
  if (!g_file_get_contents (path, &data, &len, error))
    return NULL;
  return g_bytes_new_take (g_steal_pointer (&data), len);
}

static gboolean
write_checked (const char *dir, const char *name, guint8 *data, gsize len,
               const char *want_sha, GError **error)
{
  g_autofree char *sha = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, len);
  g_autofree char *path = g_build_filename (dir, name, NULL);

  if (!g_str_equal (sha, want_sha))
    return gv_error_false (error, GV_ERROR_PROTO, "extracted %s does not match (sha256 %s)", name, sha);
  return g_file_set_contents_full (path, (const char *) data, len,
                                   G_FILE_SET_CONTENTS_CONSISTENT, 0644, error);
}

gboolean
gv_extract (const char *lg_driver, const char *out_dir, GError **error)
{
  g_autoptr(GBytes) dll = read_wbdi (lg_driver, error);
  g_autofree char *sha = NULL;
  const guint8 *d;
  gsize len;
  guint8 cfg[GV_CFG_SIZE], fw[GV_FIRMWARE_SIZE];

  if (!dll)
    return FALSE;
  d = g_bytes_get_data (dll, &len);
  sha = g_compute_checksum_for_data (G_CHECKSUM_SHA256, d, len);
  if (!g_str_equal (sha, WBDI_SHA256))
    return gv_error_false (error, GV_ERROR_PROTO,
                           "unsupported Wbdi.dll (sha256 %s); use LG's GD_FP_W106 driver 3.1.100.320", sha);

  memcpy (cfg, d + CONFIG_OFFSET, sizeof cfg);
  gv_cfg_fix_checksum (cfg);
  memcpy (fw, d + FIRMWARE_OFFSET, sizeof fw);

  if (g_mkdir_with_parents (out_dir, 0755) < 0)
    return gv_error_false (error, GV_ERROR_IO, "cannot create %s", out_dir);
  return write_checked (out_dir, "sensor-config.bin", cfg, sizeof cfg, CONFIG_SHA256, error) &&
         write_checked (out_dir, "firmware.bin", fw, sizeof fw, FIRMWARE_SHA256, error);
}

/* ---------- provision ---------- */

static guint32
crc32_mpeg2 (const guint8 *d, gsize n)
{
  guint32 crc = 0xffffffff;

  for (gsize i = 0; i < n; i++)
    {
      crc ^= (guint32) d[i] << 24;
      for (int b = 0; b < 8; b++)
        crc = crc & 0x80000000 ? (crc << 1) ^ 0x04c11db7 : crc << 1;
    }
  return crc;
}

static void
put32 (guint8 *p, guint32 v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static gboolean
status_ok (GBytes *r, guint8 want)
{
  return r && g_bytes_get_size (r) >= 1 && ((const guint8 *) g_bytes_get_data (r, NULL))[0] == want;
}

gboolean
gv_provision (GvSensor *s, const guint8 *firmware, gsize len,
              GvProgressFunc progress, gpointer user_data, GError **error)
{
  static const guint8 erase_req[2] = { 0x00, 50 };
  g_autofree guint8 *img = NULL;
  g_autofree char *fw = NULL;
  g_autoptr(GBytes) r = NULL;
  guint8 psk_req[12 + 114];
  gsize code_len, img_len;
  gboolean psk_ok = FALSE;

  if (len != GV_FIRMWARE_SIZE)
    return gv_error_false (error, GV_ERROR_PROTO, "firmware image has the wrong size");

  /* 1. Erase the app: the MCU drops into its IAP bootloader. */
  progress ("Erasing sensor application", 0.0, user_data);
  r = gv_sensor_command (s, GV_CMD_ERASE_APP, erase_req, sizeof erase_req, 6000, NULL, error);
  if (!r)
    return FALSE;
  g_clear_pointer (&r, g_bytes_unref);
  g_usleep (G_USEC_PER_SEC);
  if (!gv_sensor_reset (s, error))
    return FALSE;

  /* 2. Write the all-zero PSK (white-box encoded) and check the PMK hash. */
  progress ("Writing pairing key", 0.1, user_data);
  put32 (psk_req, sizeof psk_req - 12);         /* total */
  put32 (psk_req + 4, sizeof psk_req - 12);     /* chunk length */
  put32 (psk_req + 8, 0);                       /* offset */
  memcpy (psk_req + 12, PSK_PRE_FLAGS, 10);
  put32 (psk_req + 22, PSK_FLAGS_WRITE);
  put32 (psk_req + 26, sizeof PSK_WHITE_BOX);
  memcpy (psk_req + 30, PSK_WHITE_BOX, sizeof PSK_WHITE_BOX);
  r = gv_sensor_command (s, GV_CMD_PSK_WRITE, psk_req, sizeof psk_req, 4000, NULL, error);
  if (!r)
    return FALSE;
  g_clear_pointer (&r, g_bytes_unref);
  if (!gv_sensor_status (s, FALSE, &fw, &psk_ok, NULL, error))
    return FALSE;
  if (!psk_ok)
    return gv_error_false (error, GV_ERROR_PROTO, "pairing key was not accepted");
  g_clear_pointer (&fw, g_free);

  /* 3. Flash the app as the vendor's WriteApp does:
   *    [crc32(size || crc32(code))][size][crc32(code)][code], 256-byte chunks, area 2. */
  code_len = len - 4;
  img_len = 12 + code_len;
  img = g_malloc (img_len);
  put32 (img + 4, code_len);
  put32 (img + 8, crc32_mpeg2 (firmware, code_len));
  put32 (img, crc32_mpeg2 (img + 4, 8));
  memcpy (img + 12, firmware, code_len);
  for (gsize o = 0; o < img_len; o += 256)
    {
      gsize n = MIN (256, img_len - o);
      guint8 req[12 + 256];
      int tries = 0;

      progress ("Writing sensor firmware", 0.15 + 0.8 * o / img_len, user_data);
      put32 (req, o);
      put32 (req + 4, n);
      put32 (req + 8, 2);
      memcpy (req + 12, img + o, n);
      while (!status_ok (r = gv_sensor_command (s, GV_CMD_WRITE_FW, req, 12 + n, 3000, NULL, NULL), 1))
        {
          g_clear_pointer (&r, g_bytes_unref);
          if (++tries == 3)
            return gv_error_false (error, GV_ERROR_PROTO, "firmware write failed at offset %zu", o);
        }
      g_clear_pointer (&r, g_bytes_unref);
    }

  /* 4. Boot it and check it runs with our key. */
  progress ("Starting sensor", 0.97, user_data);
  if (!gv_sensor_reset (s, error))
    return FALSE;
  g_usleep (2 * G_USEC_PER_SEC);
  if (!gv_sensor_status (s, FALSE, &fw, &psk_ok, NULL, error))
    return FALSE;
  if (!g_str_has_prefix (fw, "GF3268") || !psk_ok)
    return gv_error_false (error, GV_ERROR_PROTO, "sensor did not start correctly (\"%s\")", fw);
  progress ("Done", 1.0, user_data);
  return TRUE;
}
