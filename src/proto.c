/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "proto.h"

#include <string.h>

gsize
gv_msg_encode (guint8 cmd, const guint8 *payload, gsize len, guint8 *out)
{
  guint8 sum;

  out[0] = cmd;
  out[1] = (len + 1) & 0xff;
  out[2] = (len + 1) >> 8;
  if (len)
    memcpy (out + 3, payload, len);
  sum = 0;
  for (gsize i = 0; i < len + 3; i++)
    sum += out[i];
  out[len + 3] = 0xaa - sum;
  return len + 4;
}

gsize
gv_pack_encode (guint8 flag, const guint8 *body, gsize len, guint8 *out)
{
  out[0] = flag;
  out[1] = len & 0xff;
  out[2] = len >> 8;
  out[3] = out[0] + out[1] + out[2];
  memcpy (out + 4, body, len);
  return len + 4;
}

gboolean
gv_pack_decode (const guint8 *buf, gsize buf_len,
                guint8 *flag, const guint8 **body, gsize *body_len)
{
  gsize len;

  if (buf_len < 4)
    return FALSE;
  if (buf[0] != GV_FLAG_PLAIN && buf[0] != GV_FLAG_TLS && buf[0] != GV_FLAG_TLS_DATA)
    return FALSE;
  if (((buf[0] + buf[1] + buf[2]) & 0xff) != buf[3])
    return FALSE;
  len = buf[1] | (buf[2] << 8);
  if (len + 4 > buf_len)
    return FALSE;
  *flag = buf[0];
  *body = buf + 4;
  *body_len = len;
  return TRUE;
}

gboolean
gv_msg_decode (const guint8 *buf, gsize buf_len,
               guint8 *cmd, const guint8 **data, gsize *data_len)
{
  gsize len;

  if (buf_len < 4)
    return FALSE;
  len = buf[1] | (buf[2] << 8);       /* payload + checksum */
  if (len < 1 || len + 3 > buf_len)
    return FALSE;
  *cmd = buf[0];
  *data = buf + 3;
  *data_len = len - 1;
  return TRUE;
}

void
gv_image_decode (const guint8 *p, guint16 *px)
{
  for (gsize i = 0, o = 0; o + 4 <= GV_IMG_PIXELS; i += 6, o += 4)
    {
      px[o + 0] = ((p[i + 0] & 0x0f) << 8) | p[i + 1];
      px[o + 1] = (p[i + 3] << 4) | (p[i + 0] >> 4);
      px[o + 2] = ((p[i + 5] & 0x0f) << 8) | p[i + 2];
      px[o + 3] = (p[i + 4] << 4) | (p[i + 5] >> 4);
    }
}

guint16
gv_cfg_checksum (const guint8 *cfg)
{
  guint16 acc = 0xa5a5;

  for (gsize k = 0; k < 127; k++)
    acc += cfg[2 * k] | (cfg[2 * k + 1] << 8);
  return -acc;
}

void
gv_cfg_fix_checksum (guint8 *cfg)
{
  guint16 c = gv_cfg_checksum (cfg);

  cfg[254] = c & 0xff;
  cfg[255] = c >> 8;
}

/* The config is a list of [reg u16 LE][value u16 LE] pairs starting at the first reg 0x0084.
 * The vendor driver (MilanFSerModifyImageTcode) rewrites every reg 0x005c value with the
 * OTP-derived Tcode. */
guint
gv_cfg_patch_tcode (guint8 *cfg, guint16 tcode)
{
  guint patched = 0;
  gsize o;

  for (o = 0; o + 1 < GV_CFG_SIZE; o++)
    if (cfg[o] == 0x84 && cfg[o + 1] == 0x00)
      break;
  for (; o + 4 <= GV_CFG_SIZE - 2; o += 4)
    if ((cfg[o] | (cfg[o + 1] << 8)) == 0x005c)
      {
        cfg[o + 2] = tcode & 0xff;
        cfg[o + 3] = tcode >> 8;
        patched++;
      }
  gv_cfg_fix_checksum (cfg);
  return patched;
}

gboolean
gv_otp_tcode (const guint8 *otp, gsize otp_len, guint16 *tcode)
{
  if (otp_len <= 0x17 || ((otp[0x16] + otp[0x17]) & 0xff) != 0xff)
    return FALSE;
  *tcode = (((otp[0x16] >> 4) & 0xf) + 1) << 4;
  return TRUE;
}

gboolean
gv_fdt_channels (const guint8 *data, gsize len, guint16 *ch)
{
  if (len < 4 + 2 * GV_FDT_CHANNELS)
    return FALSE;
  for (gsize i = 0; i < GV_FDT_CHANNELS; i++)
    ch[i] = data[4 + 2 * i] | (data[5 + 2 * i] << 8);
  return TRUE;
}

void
gv_fdt_base (const guint16 *ch, guint8 *base)
{
  for (gsize i = 0; i < GV_FDT_CHANNELS; i++)
    {
      base[2 * i] = 0x80;
      base[2 * i + 1] = (ch[i] >> 1) & 0xff;
    }
}
