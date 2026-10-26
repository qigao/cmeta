/* Separate experimental DTLS 1.2 transport for the installed C11
 * Session <-> independent Pion SCTP wire oracle. This is NEVER a
 * production DTLS provider, browser harness, stable ABI or credential
 * trust store. Both certificates are ephemeral and their SHA-256 leaf
 * digests are explicitly pinned out-of-band by this test's launcher. */
#include "pion_dtls_transport.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

enum { MAX_DTLS_RECORD_BUFFER = 16384, MAX_DTLS_RECORD_BYTES = 4096 };

static int hex_digit(char v) {
  if (v >= '0' && v <= '9') return (int)(v - '0');
  if (v >= 'a' && v <= 'f') return (int)(v - 'a') + 10;
  if (v >= 'A' && v <= 'F') return (int)(v - 'A') + 10;
  return -1;
}
static int parse_hex_fingerprint(const char *s, uint8_t output[32]) {
  size_t i;
  if (!s || strlen(s) != 64u) return -1;
  for (i = 0u; i < 32u; ++i) {
    const int high = hex_digit(s[i * 2u]);
    const int low = hex_digit(s[i * 2u + 1u]);
    if (high < 0 || low < 0) return -1;
    output[i] = (uint8_t)((high << 4) | low);
  }
  return 0;
}
static int test_selfsigned_chain_callback(int preverify_ok,
    X509_STORE_CTX *ctx) {
  /* Certificate identity is pinned by SHA256 AFTER handshake, before the
   * first SCTP application plaintext can be forwarded to Session. */
  (void)preverify_ok; (void)ctx;
  return 1;
}
static int init_ephemeral_identity(SSL_CTX *ctx, uint8_t out_fp[32]) {
  EVP_PKEY_CTX *generator = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
  EVP_PKEY *key = NULL;
  X509 *cert = NULL;
  X509_NAME *name;
  unsigned int size = 0u;
  int success = -1;
  if (!generator || EVP_PKEY_keygen_init(generator) != 1 ||
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator,
          NID_X9_62_prime256v1) != 1 ||
      EVP_PKEY_keygen(generator, &key) != 1)
    goto done;
  cert = X509_new();
  if (!cert || X509_set_version(cert, 2L) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(cert), 1L) != 1 ||
      !X509_gmtime_adj(X509_get_notBefore(cert), -60L) ||
      !X509_gmtime_adj(X509_get_notAfter(cert), 3600L) ||
      X509_set_pubkey(cert, key) != 1)
    goto done;
  name = X509_get_subject_name(cert);
  if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
          (const unsigned char *)"isolated SCTP Pion DTLS oracle",
          -1, -1, 0) != 1 ||
      X509_set_issuer_name(cert, name) != 1 ||
      X509_sign(cert, key, EVP_sha256()) <= 0 ||
      X509_digest(cert, EVP_sha256(), out_fp, &size) != 1 ||
      size != 32u ||
      SSL_CTX_use_certificate(ctx, cert) != 1 ||
      SSL_CTX_use_PrivateKey(ctx, key) != 1 ||
      SSL_CTX_check_private_key(ctx) != 1)
    goto done;
  success = 0;
done:
  X509_free(cert);
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(generator);
  return success;
}
static int peer_fingerprint_matches(SSL *ssl, const uint8_t expected[32]) {
  X509 *peer = SSL_get1_peer_certificate(ssl);
  uint8_t actual[32] = {0};
  unsigned int bytes = 0u;
  int valid = 0;
  if (peer) {
    valid = X509_digest(peer, EVP_sha256(), actual, &bytes) == 1 &&
        bytes == 32u && CRYPTO_memcmp(actual, expected, 32u) == 0;
  }
  OPENSSL_cleanse(actual, sizeof(actual));
  X509_free(peer);
  return valid;
}
int pion_test_dtls_init(pion_test_dtls *t, const char *client_fp_hex) {
  uint8_t server_fp[32];
  BIO *rx = NULL, *tx = NULL;
  size_t i;
  if (!t) return -1;
  memset(t, 0, sizeof(*t));
  memset(server_fp, 0, sizeof(server_fp));
  if (parse_hex_fingerprint(client_fp_hex,
          t->expected_client_fingerprint) != 0)
    goto fail;
  t->context = SSL_CTX_new(DTLS_server_method());
  if (!t->context ||
      SSL_CTX_set_min_proto_version(t->context, DTLS1_2_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(t->context, DTLS1_2_VERSION) != 1)
    goto fail;
  SSL_CTX_set_verify(t->context,
      SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
      test_selfsigned_chain_callback);
  if (init_ephemeral_identity(t->context, server_fp) != 0)
    goto fail;
  t->ssl = SSL_new(t->context);
  rx = BIO_new(BIO_s_mem());
  tx = BIO_new(BIO_s_mem());
  if (!t->ssl || !rx || !tx) goto fail;
  BIO_set_mem_eof_return(rx, -1);
  BIO_set_mem_eof_return(tx, -1);
  SSL_set_bio(t->ssl, rx, tx);
  rx = tx = NULL;
  SSL_set_accept_state(t->ssl);
  /* An OpenSSL memory BIO is not a datagram BIO: do not assume
   * SSL_set_mtu() succeeds. The record output remains bounded by the
   * parser/one datagram below; actual network PMTU is unqualified. */
  fputs("DTLS_SERVER_FP=", stdout);
  for (i = 0u; i < sizeof(server_fp); ++i)
    printf("%02x", (unsigned int)server_fp[i]);
  putchar('\n');
  fflush(stdout);
  OPENSSL_cleanse(server_fp, sizeof(server_fp));
  return 0;
fail:
  BIO_free(rx);
  BIO_free(tx);
  OPENSSL_cleanse(server_fp, sizeof(server_fp));
  pion_test_dtls_stop(t);
  return -1;
}
static int flush_ciphertext(pion_test_dtls *t, int fd,
    const struct sockaddr_in *peer) {
  uint8_t bytes[MAX_DTLS_RECORD_BUFFER];
  int amount;
  if (!t || !t->ssl || fd < 0 || !peer) return -1;
  while ((amount = BIO_read(SSL_get_wbio(t->ssl), bytes,
          (int)sizeof(bytes))) > 0) {
    size_t offset = 0u, n = (size_t)amount;
    while (offset < n) {
      size_t length;
      /* DTLS 1.2 record header: type, version, epoch, 48-bit seq,
       * then uint16 record length at bytes 11-12. Never split or merge
       * record contents. A flight may contain several complete records. */
      if (n - offset < 13u) return -1;
      length = ((size_t)bytes[offset + 11u] << 8u) +
          (size_t)bytes[offset + 12u] + 13u;
      if (length < 13u || length > MAX_DTLS_RECORD_BYTES ||
          length > n - offset) return -1;
      if (sendto(fd, bytes + offset, length, 0,
              (const struct sockaddr *)peer, sizeof(*peer)) !=
          (ssize_t)length)
        return -1;
      offset += length;
    }
  }
  return amount <= 0 ? 0 : -1;
}
int pion_test_dtls_input(pion_test_dtls *t, int fd,
    const struct sockaddr_in *peer, const uint8_t *encrypted,
    size_t encrypted_size, uint8_t *plaintext, size_t capacity,
    size_t *plaintext_size) {
  int result, code;
  if (plaintext_size) *plaintext_size = 0u;
  if (!t || !t->ssl || !encrypted || !encrypted_size || !plaintext ||
      !capacity || capacity > (size_t)INT_MAX || !plaintext_size ||
      encrypted_size > (size_t)INT_MAX ||
      BIO_write(SSL_get_rbio(t->ssl), encrypted,
          (int)encrypted_size) != (int)encrypted_size)
    return -1;
  if (!t->accepted) {
    result = SSL_do_handshake(t->ssl);
    if (result != 1) {
      code = SSL_get_error(t->ssl, result);
      if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE)
        return -1;
    }
    if (flush_ciphertext(t, fd, peer) != 0) return -1;
    if (!SSL_is_init_finished(t->ssl)) return 0;
    if (SSL_version(t->ssl) != DTLS1_2_VERSION)
      return -1;
    if (!peer_fingerprint_matches(t->ssl, t->expected_client_fingerprint)) {
      /* Exact certificate mismatch rather than an unrelated network error.
       * No SCTP plaintext is admitted on this bad peer-identity path. */
      puts("OPENSSL_DTLS_BAD_CLIENT_FINGERPRINT_REJECTED");
      fflush(stdout);
      return -1;
    }
    t->accepted = 1u;
    puts("OPENSSL_DTLS_CLIENT_FINGERPRINT_MATCH");
    fflush(stdout);
  }
  result = SSL_read(t->ssl, plaintext, (int)capacity);
  if (result > 0) {
    *plaintext_size = (size_t)result;
    return 1;
  }
  code = SSL_get_error(t->ssl, result);
  if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE &&
      code != SSL_ERROR_ZERO_RETURN)
    return -1;
  return flush_ciphertext(t, fd, peer) == 0 ? 0 : -1;
}
int pion_test_dtls_send(pion_test_dtls *t, int fd,
    const struct sockaddr_in *peer, const uint8_t *plaintext,
    size_t size) {
  if (!t || !t->accepted || !t->ssl || !plaintext ||
      !size || size > (size_t)INT_MAX)
    return -1;
  if (SSL_write(t->ssl, plaintext, (int)size) != (int)size) return -1;
  return flush_ciphertext(t, fd, peer);
}
void pion_test_dtls_stop(pion_test_dtls *t) {
  if (!t) return;
  SSL_free(t->ssl);
  SSL_CTX_free(t->context);
  OPENSSL_cleanse(t->expected_client_fingerprint,
      sizeof(t->expected_client_fingerprint));
  t->ssl = NULL;
  t->context = NULL;
  t->accepted = 0u;
}
