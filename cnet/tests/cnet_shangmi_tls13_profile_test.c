/*
 * #1128: CNet's actual internal TLS ring/owner adapter, RFC 8998.
 * Keys/certificates are generated outside the binary by the pinned private
 * GmSSL CLI runner. This is NOT the CLI provider handshake test.
 */
#include "cnet_tls.h"
#include <salts/error_codes.h>

#include <stdio.h>
#include <string.h>

typedef struct shangmi_pair {
  cnet_tls_server server_context;
  cnet_tls_state server;
  cnet_tls_state client;
} shangmi_pair;

static void pair_destroy(shangmi_pair *pair) {
  cnet_tls_state_destroy(&pair->server);
  cnet_tls_state_destroy(&pair->client);
  (void)cnet_tls_server_destroy(&pair->server_context);
}

static int pair_create(shangmi_pair *pair, const char *trust, const char *cert,
                       const char *key, const char *hostname,
                       cnet_tls_security_profile client_profile) {
  cnet_tls_server_config server_cfg = {0};
  cnet_tls_client_config client_cfg = {0};
  cnet_tls_context *ctx = NULL;
  int status;

  memset(pair, 0, sizeof(*pair));
  server_cfg.size = sizeof(server_cfg);
  server_cfg.cert_file = cert;
  server_cfg.key_file = key;
  server_cfg.key_password = "fixture-only-password";
  server_cfg.security_profile = CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13;
  status = cnet_tls_server_init(&pair->server_context, &server_cfg);
  if (status != SALTS_OK) return status;

  client_cfg.size = sizeof(client_cfg);
  client_cfg.ca_file = trust;
  client_cfg.security_profile = client_profile;
  status = cnet_tls_client_context_create(&client_cfg, &ctx);
  if (status != SALTS_OK) return status;

  status = cnet_tls_state_init(&pair->client, ctx, false, hostname,
                               CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) {
    cnet_tls_context_release(ctx);
    return status;
  }
  ctx = cnet_tls_server_context(&pair->server_context);
  cnet_tls_context_retain(ctx);
  status = cnet_tls_state_init(&pair->server, ctx, true, NULL,
                               CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) cnet_tls_context_release(ctx);
  return status;
}

static int transfer(cnet_tls_state *source, cnet_tls_state *target) {
  unsigned char buffer[1024];
  for (;;) {
    size_t size = 0u;
    int status = cnet_tls_take_cipher(source, buffer, sizeof(buffer), &size);
    if (status == SALTS_ENOENT) return SALTS_OK;
    if (status != SALTS_OK || size == 0u) return SALTS_EPROTO;
    status = cnet_tls_feed_cipher(target, buffer, size);
    if (status != SALTS_OK) return status;
  }
}

static int handshake(shangmi_pair *pair) {
  unsigned int i;
  for (i = 0u; i < 256u; ++i) {
    bool client_done = false, server_done = false;
    int result = cnet_tls_handshake(&pair->client, &client_done);
    if (result != SALTS_OK) return result;
    result = transfer(&pair->client, &pair->server);
    if (result != SALTS_OK) return result;
    result = cnet_tls_handshake(&pair->server, &server_done);
    if (result != SALTS_OK) return result;
    result = transfer(&pair->server, &pair->client);
    if (result != SALTS_OK) return result;
    if (client_done && server_done) return SALTS_OK;
  }
  return SALTS_ETIMEDOUT;
}

static int receive_message(cnet_tls_state *source, cnet_tls_state *target,
                           const char *message) {
  bool write_complete = false;
  size_t total = 0u;
  size_t length = strlen(message);
  unsigned char received[128] = {0};
  unsigned int i;
  if (length == 0u || length > sizeof(received)) return SALTS_EINVAL;

  for (i = 0u; i < 256u; ++i) {
    size_t got = 0u;
    bool closed = false;
    int status;
    if (!write_complete) {
      status = cnet_tls_write(source, message, length, &write_complete);
      if (status != SALTS_OK) return status;
    }
    status = transfer(source, target);
    if (status != SALTS_OK) return status;
    status = cnet_tls_read(target, received + total, sizeof(received) - total,
                           &got, &closed);
    if (status != SALTS_OK || closed) return SALTS_EPROTO;
    total += got;
    if (write_complete && total == length)
      return memcmp(received, message, length) == 0 ? SALTS_OK : SALTS_EPROTO;
  }
  return SALTS_ETIMEDOUT;
}

/*
 * Authenticated TLS application data must never be exposed after a corrupted
 * SM4-GCM record. Exercise the real CNet private-GmSSL state and bounded ring
 * as a separate negative after the successful bidirectional exchange.
 * This test deliberately corrupts an encrypted record, not the plaintext.
 */
static int tampered_record_rejected(shangmi_pair *pair) {
  static const char payload[] = "CNet-RFC8998-authenticated-record";
  unsigned char ciphertext[4096];
  unsigned char plaintext[sizeof(payload)];
  size_t captured = 0u;
  size_t read_size = 123u;
  bool complete = false;
  bool peer_closed = true;
  int status;
  size_t i;

  status = cnet_tls_write(&pair->client, payload, sizeof(payload) - 1u,
                          &complete);
  if (status != SALTS_OK || !complete) return SALTS_EPROTO;

  for (;;) {
    size_t chunk = 0u;
    status = cnet_tls_take_cipher(&pair->client, ciphertext + captured,
                                  sizeof(ciphertext) - captured, &chunk);
    if (status == SALTS_ENOENT) break;
    if (status != SALTS_OK || chunk == 0u ||
        chunk > sizeof(ciphertext) - captured) return SALTS_EPROTO;
    captured += chunk;
    if (captured == sizeof(ciphertext)) return SALTS_EPROTO;
  }

  /* A TLS 1.3 record header is 5 bytes. The final byte of a complete
   * encrypted application record is authenticated by its GCM tag. */
  if (captured <= 5u) return SALTS_EPROTO;
  ciphertext[captured - 1u] ^= 0x01u;
  status = cnet_tls_feed_cipher(&pair->server, ciphertext, captured);
  if (status != SALTS_OK) return status;

  memset(plaintext, 0xa5, sizeof(plaintext));
  status = cnet_tls_read(&pair->server, plaintext, sizeof(plaintext),
                         &read_size, &peer_closed);
  if (status != SALTS_EPROTO || read_size != 0u || peer_closed)
    return SALTS_EPROTO;
  for (i = 0u; i < sizeof(plaintext); ++i)
    if (plaintext[i] != 0xa5u) return SALTS_EPROTO;

  return SALTS_OK;
}

static int good(const char *ca, const char *cert, const char *key) {
  shangmi_pair pair = {0};
  char version[32] = {0};
  char cipher[80] = {0};
  size_t size = 0u;
  int status = pair_create(&pair, ca, cert, key, "localhost",
                           CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13);
  if (status != SALTS_OK) goto done;
  status = cnet_tls_state_set_protocol_range(&pair.client,
             CNET_TLS_PROTOCOL_VERSION_1_2, CNET_TLS_PROTOCOL_VERSION_1_3);
  if (status != SALTS_EINVAL) {
    status = SALTS_EPROTO;
    goto done;
  }
  status = handshake(&pair);
  if (status != SALTS_OK) goto done;
  status = cnet_tls_state_negotiated_version(&pair.client, version,
                                             sizeof(version), &size);
  if (status != SALTS_OK || strcmp(version, "TLSv1.3") != 0) {
    status = SALTS_EPROTO;
    goto done;
  }
  status = cnet_tls_state_negotiated_cipher(&pair.client, cipher,
                                            sizeof(cipher), &size);
  if (status != SALTS_OK || strcmp(cipher, "TLS_SM4_GCM_SM3") != 0) {
    status = SALTS_EPROTO;
    goto done;
  }
  status = receive_message(&pair.client, &pair.server,
                           "CNet-SM2-SM4-GCM-client-record");
  if (status != SALTS_OK) goto done;
  status = receive_message(&pair.server, &pair.client,
                           "CNet-SM2-SM4-GCM-server-record");
  if (status != SALTS_OK) goto done;
  status = tampered_record_rejected(&pair);
done:
  pair_destroy(&pair);
  if (status != SALTS_OK)
    fprintf(stderr, "CNet RFC8998 verified handshake/record failed: %d\n", status);
  return status == SALTS_OK;
}

static int rejected(const char *ca, const char *cert, const char *key,
                    const char *hostname, cnet_tls_security_profile profile) {
  shangmi_pair pair = {0};
  int status = pair_create(&pair, ca, cert, key, hostname, profile);
  if (status == SALTS_OK) status = handshake(&pair);
  /* No success is allowed for an untrusted identity or cross-profile peer. */
  pair_destroy(&pair);
  if (status == SALTS_OK) {
    fprintf(stderr, "CNet RFC8998 incorrectly accepted incompatible peer\n");
    return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  cnet_tls_client_config bad = {0};
  cnet_tls_context *ctx = NULL;
  int status;
  if (argc != 5) {
    fprintf(stderr, "usage: %s ca.pem cert.pem key.pem untrusted-ca.pem\n", argv[0]);
    return 2;
  }
  bad.size = sizeof(bad);
  bad.security_profile = (cnet_tls_security_profile)99;
  status = cnet_tls_client_context_create(&bad, &ctx);
  if (status != SALTS_EINVAL || ctx != NULL) return 1;
  if (!good(argv[1], argv[2], argv[3]) ||
      !rejected(argv[4], argv[2], argv[3], "localhost",
                CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13) ||
      !rejected(argv[1], argv[2], argv[3], "wrong.example",
                CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13) ||
      !rejected(argv[1], argv[2], argv[3], "localhost",
                CNET_TLS_SECURITY_PROFILE_STANDARD))
    return 1;
  puts("CNet RFC8998 strict TLS owner/ring profile: PASS");
  return 0;
}
