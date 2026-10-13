/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include "proto.h"

static void
test_msg_and_pack (void)
{
  static const guint8 zero2[2] = { 0, 0 };
  /* Captured from the real chip: request-TLS message in a plaintext pack. */
  static const guint8 want[] = { 0xa0, 0x06, 0x00, 0xa6, 0xd0, 0x03, 0x00, 0x00, 0x00, 0xd7 };
  guint8 msg[8], pack[16], flag, cmd;
  const guint8 *body, *data;
  gsize n, body_len, data_len;

  n = gv_msg_encode (0xd0, zero2, 2, msg);
  n = gv_pack_encode (GV_FLAG_PLAIN, msg, n, pack);
  g_assert_cmpmem (pack, n, want, sizeof want);

  g_assert_true (gv_pack_decode (pack, n, &flag, &body, &body_len));
  g_assert_cmpint (flag, ==, GV_FLAG_PLAIN);
  g_assert_true (gv_msg_decode (body, body_len, &cmd, &data, &data_len));
  g_assert_cmpint (cmd, ==, 0xd0);
  g_assert_cmpmem (data, data_len, zero2, 2);

  pack[3] ^= 1;                                      /* bad header checksum */
  g_assert_false (gv_pack_decode (pack, n, &flag, &body, &body_len));
  g_assert_false (gv_pack_decode (want, 5, &flag, &body, &body_len));   /* truncated */
}

static void
test_image_decode (void)
{
  guint16 in[GV_IMG_PIXELS], out[GV_IMG_PIXELS];
  guint8 packed[GV_IMG_PACKED];

  for (gsize i = 0; i < GV_IMG_PIXELS; i++)
    in[i] = (i * 2654435761u) & 0xfff;
  /* inverse of the 6-bytes-per-4-pixels layout */
  for (gsize i = 0, o = 0; o < GV_IMG_PIXELS; i += 6, o += 4)
    {
      packed[i + 0] = ((in[o + 1] & 0xf) << 4) | (in[o] >> 8);
      packed[i + 1] = in[o] & 0xff;
      packed[i + 2] = in[o + 2] & 0xff;
      packed[i + 3] = in[o + 1] >> 4;
      packed[i + 4] = in[o + 3] >> 4;
      packed[i + 5] = ((in[o + 3] & 0xf) << 4) | (in[o + 2] >> 8);
    }
  gv_image_decode (packed, out);
  g_assert_cmpmem (out, sizeof out, in, sizeof in);
}

static void
test_config (void)
{
  guint8 cfg[GV_CFG_SIZE] = { 0 };
  guint8 otp[32] = { 0 };
  guint16 tcode;

  /* reg/value pairs starting at reg 0x0084, two of them reg 0x005c */
  const guint16 pairs[][2] = { { 0x0084, 1 }, { 0x005c, 0x80 }, { 0x0022, 7 }, { 0x005c, 0x90 } };
  for (gsize i = 0; i < G_N_ELEMENTS (pairs); i++)
    {
      cfg[16 + 4 * i] = pairs[i][0] & 0xff;
      cfg[17 + 4 * i] = pairs[i][0] >> 8;
      cfg[18 + 4 * i] = pairs[i][1] & 0xff;
      cfg[19 + 4 * i] = pairs[i][1] >> 8;
    }
  otp[0x16] = 0xd3;
  otp[0x17] = 0x2c;                                  /* 0xd3 + 0x2c = 0xff -> valid */
  g_assert_true (gv_otp_tcode (otp, sizeof otp, &tcode));
  g_assert_cmpint (tcode, ==, 0xe0);
  g_assert_cmpuint (gv_cfg_patch_tcode (cfg, tcode), ==, 2);
  g_assert_cmpint (cfg[22], ==, 0xe0);
  g_assert_cmpint (cfg[30], ==, 0xe0);
  g_assert_cmpint (cfg[26], ==, 7);
  g_assert_cmpint (gv_cfg_checksum (cfg), ==, cfg[254] | (cfg[255] << 8));

  otp[0x17] = 0;
  g_assert_false (gv_otp_tcode (otp, sizeof otp, &tcode));
}

static void
test_fdt (void)
{
  guint8 reply[4 + 24] = { 0x82, 0x00, 0xff, 0x0f };
  guint16 ch[GV_FDT_CHANNELS];
  guint8 base[24];

  for (int i = 0; i < GV_FDT_CHANNELS; i++)
    {
      reply[4 + 2 * i] = (345 + i) & 0xff;
      reply[5 + 2 * i] = (345 + i) >> 8;
    }
  g_assert_true (gv_fdt_channels (reply, sizeof reply, ch));
  g_assert_cmpint (ch[0], ==, 345);
  gv_fdt_base (ch, base);
  g_assert_cmpint (base[0], ==, 0x80);
  g_assert_cmpint (base[1], ==, 345 >> 1);
  g_assert_false (gv_fdt_channels (reply, 10, ch));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/proto/msg-pack", test_msg_and_pack);
  g_test_add_func ("/proto/image-decode", test_image_decode);
  g_test_add_func ("/proto/config", test_config);
  g_test_add_func ("/proto/fdt", test_fdt);
  return g_test_run ();
}
