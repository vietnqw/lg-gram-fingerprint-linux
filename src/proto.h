/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Goodix "Geneva" (GF3268 sensor + GM168SEC MCU, ACPI GXFP5A8B) wire protocol:
 * pure framing / parsing / image helpers. No I/O here, so everything is unit-testable.
 *
 *   message : [cmd u8][len u16 LE = payload+1][payload][checksum = 0xAA - sum]
 *   pack    : [flag u8][len u16 LE][sum(first 3) & 0xff][body]
 *             flag 0xA0 = plaintext message, 0xB0 = TLS records, 0xB2 = TLS application data
 */
#pragma once

#include <glib.h>

#define GV_IMG_WIDTH      108
#define GV_IMG_HEIGHT     88
#define GV_IMG_PIXELS     (GV_IMG_WIDTH * GV_IMG_HEIGHT)
#define GV_IMG_PACKED     (GV_IMG_PIXELS * 3 / 2)     /* 12-bit packed: 14256 bytes */
#define GV_IMG_PLAIN      (GV_IMG_PACKED + 4)         /* decrypted record: + 4-byte trailer */

#define GV_CFG_SIZE       256
#define GV_FDT_CHANNELS   12

enum {
  GV_FLAG_PLAIN    = 0xa0,
  GV_FLAG_TLS      = 0xb0,
  GV_FLAG_TLS_DATA = 0xb2,
};

enum {
  GV_CMD_GET_IMAGE    = 0x20,
  GV_CMD_FDT_DOWN     = 0x32,
  GV_CMD_FDT_UP       = 0x34,
  GV_CMD_FDT_MANUAL   = 0x36,
  GV_CMD_UPLOAD_CFG   = 0x90,
  GV_CMD_RESET        = 0xa2,
  GV_CMD_ERASE_APP    = 0xa4,
  GV_CMD_READ_OTP     = 0xa6,
  GV_CMD_FW_VERSION   = 0xa8,
  GV_CMD_ACK          = 0xb0,
  GV_CMD_REQUEST_TLS  = 0xd0,
  GV_CMD_TLS_STATE    = 0xd4,   /* "TLS established"; reply = chip mbedTLS state */
  GV_CMD_PSK_WRITE    = 0xe0,
  GV_CMD_PSK_READ     = 0xe4,
  GV_CMD_WRITE_FW     = 0xf0,
  GV_CMD_CHECK_FW     = 0xf4,
};

#define GV_TLS_STATE_OVER 0x10   /* MBEDTLS_SSL_HANDSHAKE_OVER */

/* Encode a message into out (needs len + 4 bytes). Returns bytes written. */
gsize    gv_msg_encode (guint8 cmd, const guint8 *payload, gsize len, guint8 *out);

/* Wrap body in a pack header into out (needs len + 4 bytes). Returns bytes written. */
gsize    gv_pack_encode (guint8 flag, const guint8 *body, gsize len, guint8 *out);

/* Parse the pack at buf. On success body points into buf. */
gboolean gv_pack_decode (const guint8 *buf, gsize buf_len,
                         guint8 *flag, const guint8 **body, gsize *body_len);

/* Parse a plaintext message. On success data points into buf. */
gboolean gv_msg_decode (const guint8 *buf, gsize buf_len,
                        guint8 *cmd, const guint8 **data, gsize *data_len);

/* Unpack 12-bit pixels: 6 bytes -> 4 pixels, row-major GV_IMG_WIDTH x GV_IMG_HEIGHT. */
void     gv_image_decode (const guint8 *packed, guint16 *pixels);

/* Config blob checksum (stored LE at offset 254) and the OTP-derived Tcode patch. */
guint16  gv_cfg_checksum (const guint8 *cfg);
void     gv_cfg_fix_checksum (guint8 *cfg);
guint    gv_cfg_patch_tcode (guint8 *cfg, guint16 tcode);
gboolean gv_otp_tcode (const guint8 *otp, gsize otp_len, guint16 *tcode);

/* FDT reply (0x36/0x32/0x34): 12 channel values after a 4-byte header. */
gboolean gv_fdt_channels (const guint8 *data, gsize len, guint16 *ch);
/* Detection base for FDT commands from measured channels: ((v >> 1) << 8) | 0x80, LE. */
void     gv_fdt_base (const guint16 *ch, guint8 *base24);
