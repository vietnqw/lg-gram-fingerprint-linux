/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "tls.h"
#include "error.h"

#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define SUITE_PSK_AES128_CBC_SHA256 0x00ae

struct _GvTls
{
  GByteArray *psk;
  GChecksum  *transcript;            /* SHA-256 over all handshake messages */
  guint8      client_random[32];
  guint8      server_random[32];
  guint8      master[48];
  guint8      cli_mac[32], srv_mac[32];
  guint8      cli_key[16], srv_key[16];
  guint64     cli_seq, srv_seq;
};

static void
hmac_sha256 (const guint8 *key, gsize key_len,
             const guint8 *a, gsize a_len, const guint8 *b, gsize b_len, guint8 *out)
{
  g_autoptr(GHmac) h = g_hmac_new (G_CHECKSUM_SHA256, key, key_len);
  gsize n = 32;

  g_hmac_update (h, a, a_len);
  if (b_len)
    g_hmac_update (h, b, b_len);
  g_hmac_get_digest (h, out, &n);
}

/* TLS 1.2 PRF (P_SHA256). */
static void
prf (const guint8 *secret, gsize secret_len, const char *label,
     const guint8 *seed, gsize seed_len, guint8 *out, gsize out_len)
{
  g_autoptr(GByteArray) ls = g_byte_array_new ();
  guint8 a[32], block[32];
  gsize done = 0;

  g_byte_array_append (ls, (const guint8 *) label, strlen (label));
  g_byte_array_append (ls, seed, seed_len);
  hmac_sha256 (secret, secret_len, ls->data, ls->len, NULL, 0, a);
  while (done < out_len)
    {
      gsize n;

      hmac_sha256 (secret, secret_len, a, 32, ls->data, ls->len, block);
      n = MIN (32, out_len - done);
      memcpy (out + done, block, n);
      done += n;
      hmac_sha256 (secret, secret_len, a, 32, NULL, 0, a);
    }
}

static void
transcript_hash (GvTls *tls, guint8 *out)
{
  g_autoptr(GChecksum) copy = g_checksum_copy (tls->transcript);
  gsize n = 32;

  g_checksum_get_digest (copy, out, &n);
}

static void
put_record_header (GByteArray *out, guint8 type, gsize len)
{
  guint8 h[5] = { type, 3, 3, len >> 8, len & 0xff };

  g_byte_array_append (out, h, 5);
}

static void
record_mac (const guint8 *key, guint64 seq, guint8 type,
            const guint8 *data, gsize len, guint8 *out)
{
  guint8 hdr[13];

  for (int i = 0; i < 8; i++)
    hdr[i] = seq >> (56 - 8 * i);
  hdr[8] = type;
  hdr[9] = 3;
  hdr[10] = 3;
  hdr[11] = len >> 8;
  hdr[12] = len & 0xff;
  hmac_sha256 (key, 32, hdr, sizeof hdr, data, len, out);
}

GvTls *
gv_tls_new (const guint8 *psk, gsize psk_len)
{
  GvTls *tls = g_new0 (GvTls, 1);

  tls->psk = g_byte_array_new ();
  g_byte_array_append (tls->psk, psk, psk_len);
  tls->transcript = g_checksum_new (G_CHECKSUM_SHA256);
  return tls;
}

void
gv_tls_free (GvTls *tls)
{
  if (!tls)
    return;
  OPENSSL_cleanse (tls->master, sizeof tls->master);
  OPENSSL_cleanse (tls->cli_key, sizeof tls->cli_key);
  OPENSSL_cleanse (tls->srv_key, sizeof tls->srv_key);
  g_byte_array_unref (tls->psk);
  g_checksum_free (tls->transcript);
  g_free (tls);
}

GBytes *
gv_tls_on_client_hello (GvTls *tls, const guint8 *rec, gsize len, GError **error)
{
  g_autoptr(GByteArray) msgs = g_byte_array_new ();
  g_autoptr(GByteArray) out = g_byte_array_new ();
  const guint8 *hs;
  gsize hs_len;
  guint8 sh[4 + 2 + 32 + 1 + 2 + 1] = { 0x02, 0, 0, sizeof sh - 4, 3, 3 };
  const guint8 done[4] = { 0x0e, 0, 0, 0 };

  if (len < 5 + 38 || rec[0] != GV_TLS_CT_HANDSHAKE)
    return gv_error_null (error, GV_ERROR_PROTO, "not a TLS handshake record");
  hs = rec + 5;
  hs_len = (rec[3] << 8) | rec[4];
  if (hs_len + 5 > len || hs[0] != 0x01 || hs_len < 38)
    return gv_error_null (error, GV_ERROR_PROTO, "not a ClientHello");

  g_checksum_reset (tls->transcript);
  g_checksum_update (tls->transcript, hs, hs_len);
  memcpy (tls->client_random, hs + 6, 32);
  RAND_bytes (tls->server_random, 32);

  /* ServerHello: TLS 1.2, empty session id, 0x00AE, no compression, no extensions.
   * Kept minimal on purpose: the chip appends every pack to a small RX buffer. */
  memcpy (sh + 6, tls->server_random, 32);
  sh[38] = 0;                                   /* session id length */
  sh[39] = SUITE_PSK_AES128_CBC_SHA256 >> 8;
  sh[40] = SUITE_PSK_AES128_CBC_SHA256 & 0xff;
  sh[41] = 0;                                   /* compression: null */
  g_byte_array_append (msgs, sh, sizeof sh);
  g_byte_array_append (msgs, done, sizeof done);
  g_checksum_update (tls->transcript, msgs->data, msgs->len);

  put_record_header (out, GV_TLS_CT_HANDSHAKE, msgs->len);
  g_byte_array_append (out, msgs->data, msgs->len);
  return g_byte_array_free_to_bytes (g_steal_pointer (&out));
}

static void
derive_keys (GvTls *tls)
{
  g_autoptr(GByteArray) pms = g_byte_array_new ();
  guint8 seed[64], kb[96], zero = 0, n[2] = { tls->psk->len >> 8, tls->psk->len & 0xff };

  /* PSK premaster secret: len(N) || N zero bytes || len(N) || psk (RFC 4279) */
  g_byte_array_append (pms, n, 2);
  for (guint i = 0; i < tls->psk->len; i++)
    g_byte_array_append (pms, &zero, 1);
  g_byte_array_append (pms, n, 2);
  g_byte_array_append (pms, tls->psk->data, tls->psk->len);

  memcpy (seed, tls->client_random, 32);
  memcpy (seed + 32, tls->server_random, 32);
  prf (pms->data, pms->len, "master secret", seed, 64, tls->master, 48);
  OPENSSL_cleanse (pms->data, pms->len);

  memcpy (seed, tls->server_random, 32);
  memcpy (seed + 32, tls->client_random, 32);
  prf (tls->master, 48, "key expansion", seed, 64, kb, sizeof kb);
  memcpy (tls->cli_mac, kb, 32);
  memcpy (tls->srv_mac, kb + 32, 32);
  memcpy (tls->cli_key, kb + 64, 16);
  memcpy (tls->srv_key, kb + 80, 16);
  OPENSSL_cleanse (kb, sizeof kb);
}

GBytes *
gv_tls_on_client_flight (GvTls *tls, const guint8 *data, gsize len, GError **error)
{
  g_autoptr(GByteArray) out = g_byte_array_new ();
  g_autoptr(GBytes) fin = NULL;
  const guint8 *cke = NULL, *fin_enc = NULL;
  gsize cke_len = 0, fin_len = 0, o = 0;
  gboolean seen_ccs = FALSE;
  guint8 th[32], want[12], verify[16] = { 0x14, 0, 0, 12 };
  const guint8 ccs[6] = { GV_TLS_CT_CCS, 3, 3, 0, 1, 1 };

  while (o + 5 <= len)
    {
      gsize rl = (data[o + 3] << 8) | data[o + 4];

      if (o + 5 + rl > len)
        break;
      if (data[o] == GV_TLS_CT_CCS)
        seen_ccs = TRUE;
      else if (data[o] == GV_TLS_CT_HANDSHAKE && !seen_ccs && rl >= 4 && data[o + 5] == 16)
        cke = data + o + 5, cke_len = rl;
      else if (data[o] == GV_TLS_CT_HANDSHAKE && seen_ccs)
        fin_enc = data + o + 5, fin_len = rl;
      o += 5 + rl;
    }
  if (!cke || !fin_enc)
    return gv_error_null (error, GV_ERROR_PROTO, "incomplete client flight");

  g_checksum_update (tls->transcript, cke, cke_len);
  derive_keys (tls);

  fin = gv_tls_decrypt (tls, GV_TLS_CT_HANDSHAKE, fin_enc, fin_len, error);
  if (!fin)
    return NULL;
  transcript_hash (tls, th);
  prf (tls->master, 48, "client finished", th, 32, want, 12);
  if (g_bytes_get_size (fin) != 16 ||
      CRYPTO_memcmp ((const guint8 *) g_bytes_get_data (fin, NULL) + 4, want, 12) != 0)
    return gv_error_null (error, GV_ERROR_NOT_PROVISIONED,
                          "sensor Finished does not verify (PSK mismatch)");
  g_checksum_update (tls->transcript, g_bytes_get_data (fin, NULL), 16);

  transcript_hash (tls, th);
  prf (tls->master, 48, "server finished", th, 32, verify + 4, 12);

  g_byte_array_append (out, ccs, sizeof ccs);
  {
    g_autoptr(GBytes) rec = gv_tls_encrypt (tls, GV_TLS_CT_HANDSHAKE, verify, sizeof verify);
    g_byte_array_append (out, g_bytes_get_data (rec, NULL), g_bytes_get_size (rec));
  }
  return g_byte_array_free_to_bytes (g_steal_pointer (&out));
}

static gboolean
aes_cbc (gboolean enc, const guint8 *key, const guint8 *iv,
         const guint8 *in, gsize len, guint8 *out)
{
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new ();
  int n = 0, m = 0;
  gboolean ok;

  ok = ctx &&
       EVP_CipherInit_ex (ctx, EVP_aes_128_cbc (), NULL, key, iv, enc) &&
       EVP_CIPHER_CTX_set_padding (ctx, 0) &&
       EVP_CipherUpdate (ctx, out, &n, in, len) &&
       EVP_CipherFinal_ex (ctx, out + n, &m);
  EVP_CIPHER_CTX_free (ctx);
  return ok && (gsize) (n + m) == len;
}

GBytes *
gv_tls_encrypt (GvTls *tls, guint8 type, const guint8 *data, gsize len)
{
  g_autoptr(GByteArray) out = g_byte_array_new ();
  gsize plen = len + 32, pad = 15 - (plen % 16), total = plen + pad + 1;
  g_autofree guint8 *p = g_malloc (total);
  g_autofree guint8 *c = g_malloc (total);
  guint8 iv[16];

  memcpy (p, data, len);
  record_mac (tls->srv_mac, tls->srv_seq++, type, data, len, p + len);
  memset (p + plen, pad, pad + 1);
  RAND_bytes (iv, sizeof iv);
  aes_cbc (TRUE, tls->srv_key, iv, p, total, c);

  put_record_header (out, type, 16 + total);
  g_byte_array_append (out, iv, 16);
  g_byte_array_append (out, c, total);
  return g_byte_array_free_to_bytes (g_steal_pointer (&out));
}

GBytes *
gv_tls_decrypt (GvTls *tls, guint8 type, const guint8 *body, gsize len, GError **error)
{
  g_autofree guint8 *p = NULL;
  gsize ct_len, pad, dlen;
  guint8 mac[32];

  if (len < 16 + 48 || (len - 16) % 16 != 0)
    return gv_error_null (error, GV_ERROR_PROTO, "bad TLS record length %zu", len);
  ct_len = len - 16;
  p = g_malloc (ct_len);
  if (!aes_cbc (FALSE, tls->cli_key, body, body + 16, ct_len, p))
    return gv_error_null (error, GV_ERROR_PROTO, "AES decrypt failed");
  pad = p[ct_len - 1];
  if (pad + 1 + 32 > ct_len)
    return gv_error_null (error, GV_ERROR_PROTO, "bad TLS padding");
  dlen = ct_len - pad - 1 - 32;
  record_mac (tls->cli_mac, tls->cli_seq++, type, p, dlen, mac);
  if (CRYPTO_memcmp (mac, p + dlen, 32) != 0)
    return gv_error_null (error, GV_ERROR_PROTO, "bad TLS record MAC");
  return g_bytes_new (p, dlen);
}
