/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Minimal TLS 1.2 PSK *server* for the Geneva MCU, which is a TLS client (mbedTLS) that only
 * speaks TLS_PSK_WITH_AES_128_CBC_SHA256 (0x00AE). Records go over the SPI mailbox in 0xB0
 * packs; the image comes back as one oversized application-data record in a 0xB2 pack, which
 * a regular TLS library would reject, hence this small hand-rolled implementation.
 */
#pragma once

#include <glib.h>

typedef struct _GvTls GvTls;

GvTls  *gv_tls_new (const guint8 *psk, gsize psk_len);
void    gv_tls_free (GvTls *tls);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvTls, gv_tls_free)

/* ClientHello record -> ServerHello + ServerHelloDone (one record, one pack). */
GBytes *gv_tls_on_client_hello (GvTls *tls, const guint8 *rec, gsize len, GError **error);

/* The chip's ClientKeyExchange, ChangeCipherSpec and Finished records (any grouping) ->
 * our ChangeCipherSpec + Finished. Fails if the chip's Finished does not verify (wrong PSK). */
GBytes *gv_tls_on_client_flight (GvTls *tls, const guint8 *data, gsize len, GError **error);

/* Decrypt the payload of one client record (after its 5-byte header). */
GBytes *gv_tls_decrypt (GvTls *tls, guint8 content_type,
                        const guint8 *body, gsize len, GError **error);

/* Encrypt data as one server record (used by tests). */
GBytes *gv_tls_encrypt (GvTls *tls, guint8 content_type,
                        const guint8 *data, gsize len);

enum {
  GV_TLS_CT_CCS       = 20,
  GV_TLS_CT_HANDSHAKE = 22,
  GV_TLS_CT_APPDATA   = 23,
};
