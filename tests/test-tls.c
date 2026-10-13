/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Run our PSK server against a real OpenSSL TLS 1.2 client over memory BIOs.
 */
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "error.h"
#include "tls.h"

static guint8 client_psk[32];

static unsigned int
psk_cb (SSL *ssl, const char *hint, char *identity, unsigned int max_id,
        unsigned char *psk, unsigned int max_psk)
{
  g_strlcpy (identity, "Client_identity", max_id);
  memcpy (psk, client_psk, sizeof client_psk);
  return sizeof client_psk;
}

typedef struct {
  SSL_CTX *ctx;
  SSL     *ssl;
  BIO     *in, *out;   /* in: server -> client, out: client -> server */
} Client;

static void
client_init (Client *c)
{
  c->ctx = SSL_CTX_new (TLS_client_method ());
  SSL_CTX_set_min_proto_version (c->ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (c->ctx, TLS1_2_VERSION);
  SSL_CTX_set_cipher_list (c->ctx, "PSK-AES128-CBC-SHA256:@SECLEVEL=0");
  SSL_CTX_set_options (c->ctx, SSL_OP_LEGACY_SERVER_CONNECT);   /* we send no extensions */
  SSL_CTX_set_psk_client_callback (c->ctx, psk_cb);
  c->ssl = SSL_new (c->ctx);
  c->in = BIO_new (BIO_s_mem ());
  c->out = BIO_new (BIO_s_mem ());
  SSL_set_bio (c->ssl, c->in, c->out);
  SSL_set_connect_state (c->ssl);
}

static void
client_clear (Client *c)
{
  SSL_free (c->ssl);
  SSL_CTX_free (c->ctx);
}

static GBytes *
client_drain (Client *c)
{
  g_autoptr(GByteArray) a = g_byte_array_new ();
  guint8 buf[4096];
  int n;

  while ((n = BIO_read (c->out, buf, sizeof buf)) > 0)
    g_byte_array_append (a, buf, n);
  return g_byte_array_free_to_bytes (g_steal_pointer (&a));
}

static void
client_feed (Client *c, GBytes *b)
{
  BIO_write (c->in, g_bytes_get_data (b, NULL), g_bytes_get_size (b));
}

static GvTls *
handshake (Client *c, GError **error)
{
  g_autoptr(GvTls) tls = gv_tls_new ((const guint8[32]) { 0 }, 32);
  g_autoptr(GBytes) hello = NULL, flight1 = NULL, flight2 = NULL, flight3 = NULL;

  g_assert_cmpint (SSL_do_handshake (c->ssl), <=, 0);
  hello = client_drain (c);
  flight1 = gv_tls_on_client_hello (tls, g_bytes_get_data (hello, NULL), g_bytes_get_size (hello), error);
  g_assert_nonnull (flight1);
  client_feed (c, flight1);
  SSL_do_handshake (c->ssl);
  flight2 = client_drain (c);
  flight3 = gv_tls_on_client_flight (tls, g_bytes_get_data (flight2, NULL), g_bytes_get_size (flight2), error);
  if (!flight3)
    return NULL;
  client_feed (c, flight3);
  g_assert_cmpint (SSL_do_handshake (c->ssl), ==, 1);
  return g_steal_pointer (&tls);
}

static void
test_handshake_and_appdata (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GvTls) tls = NULL;
  g_autoptr(GBytes) rec = NULL, plain = NULL;
  const char *msg = "geneva image payload";
  const guint8 *r;
  gsize rl;
  Client c;

  memset (client_psk, 0, sizeof client_psk);
  client_init (&c);
  tls = handshake (&c, &error);
  g_assert_no_error (error);
  g_assert_nonnull (tls);

  g_assert_cmpint (SSL_write (c.ssl, msg, strlen (msg)), ==, (int) strlen (msg));
  rec = client_drain (&c);
  r = g_bytes_get_data (rec, &rl);
  g_assert_cmpint (r[0], ==, GV_TLS_CT_APPDATA);
  plain = gv_tls_decrypt (tls, GV_TLS_CT_APPDATA, r + 5, rl - 5, &error);
  g_assert_no_error (error);
  g_assert_cmpmem (g_bytes_get_data (plain, NULL), g_bytes_get_size (plain), msg, strlen (msg));

  /* a tampered record must be rejected */
  {
    g_autofree guint8 *bad = NULL;
    g_autoptr(GBytes) p2 = NULL;

    SSL_write (c.ssl, msg, strlen (msg));
    g_clear_pointer (&rec, g_bytes_unref);
    rec = client_drain (&c);
    bad = g_memdup2 (g_bytes_get_data (rec, &rl), rl);
    bad[rl - 20] ^= 0x01;
    p2 = gv_tls_decrypt (tls, GV_TLS_CT_APPDATA, bad + 5, rl - 5, &error);
    g_assert_null (p2);
    g_assert_error (error, GV_ERROR, GV_ERROR_PROTO);
  }
  client_clear (&c);
}

static void
test_wrong_psk (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GvTls) tls = NULL;
  Client c;

  memset (client_psk, 0x5a, sizeof client_psk);
  client_init (&c);
  tls = handshake (&c, &error);
  g_assert_null (tls);
  g_assert_nonnull (error);    /* bad MAC or bad Finished, depending on where it breaks */
  client_clear (&c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/tls/handshake-appdata", test_handshake_and_appdata);
  g_test_add_func ("/tls/wrong-psk", test_wrong_psk);
  return g_test_run ();
}
