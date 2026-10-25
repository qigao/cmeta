/* #1129: strict TLCP 1.1 through CNet's actual bounded provider rings. */
#include "cnet_tls.h"
#include <salts/error_codes.h>

#include <stdio.h>
#include <string.h>

typedef struct tlcp_pair {
  cnet_tls_server server_context;
  cnet_tls_state server;
  cnet_tls_state client;
} tlcp_pair;

static void pair_destroy(tlcp_pair *pair) {
  cnet_tls_state_destroy(&pair->server);
  cnet_tls_state_destroy(&pair->client);
  (void)cnet_tls_server_destroy(&pair->server_context);
}

static int pair_create(tlcp_pair *pair, const char *trust, const char *certs,
                       const char *keys, const char *hostname,
                       cnet_tls_security_profile client_profile) {
  cnet_tls_server_config server_cfg = {0};
  cnet_tls_client_config client_cfg = {0};
  cnet_tls_context *context = NULL;
  int status;

  memset(pair, 0, sizeof(*pair));
  server_cfg.size = sizeof(server_cfg);
  server_cfg.cert_file = certs;
  server_cfg.key_file = keys;
  server_cfg.key_password = "fixture-only-password";
  server_cfg.security_profile = CNET_TLS_SECURITY_PROFILE_TLCP11;
  status = cnet_tls_server_init(&pair->server_context, &server_cfg);
  if (status != SALTS_OK) return status;

  client_cfg.size = sizeof(client_cfg);
  client_cfg.ca_file = trust;
  client_cfg.security_profile = client_profile;
  status = cnet_tls_client_context_create(&client_cfg, &context);
  if (status != SALTS_OK) return status;
  status = cnet_tls_state_init(&pair->client, context, false, hostname,
                               CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) {
    cnet_tls_context_release(context);
    return status;
  }

  context = cnet_tls_server_context(&pair->server_context);
  cnet_tls_context_retain(context);
  status = cnet_tls_state_init(&pair->server, context, true, NULL,
                               CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) cnet_tls_context_release(context);
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

static int handshake(tlcp_pair *pair) {
  unsigned int iteration;
  for (iteration = 0u; iteration < 256u; ++iteration) {
    bool client_done = false;
    bool server_done = false;
    int status = cnet_tls_handshake(&pair->client, &client_done);
    if (status != SALTS_OK) return status;
    status = transfer(&pair->client, &pair->server);
    if (status != SALTS_OK) return status;
    status = cnet_tls_handshake(&pair->server, &server_done);
    if (status != SALTS_OK) return status;
    status = transfer(&pair->server, &pair->client);
    if (status != SALTS_OK) return status;
    if (client_done && server_done) return SALTS_OK;
  }
  return SALTS_ETIMEDOUT;
}

static int exchange(cnet_tls_state *source, cnet_tls_state *target,
                    const char *message) {
  unsigned char received[96] = {0};
  size_t received_size = 0u;
  size_t length = strlen(message);
  bool written = false;
  unsigned int iteration;
  for (iteration = 0u; iteration < 256u; ++iteration) {
    size_t size = 0u;
    bool closed = false;
    int status;
    if (!written) {
      status = cnet_tls_write(source, message, length, &written);
      if (status != SALTS_OK) return status;
    }
    status = transfer(source, target);
    if (status != SALTS_OK) return status;
    status = cnet_tls_read(target, received + received_size,
                           sizeof(received) - received_size, &size, &closed);
    if (status != SALTS_OK || closed) return SALTS_EPROTO;
    received_size += size;
    if (written && received_size == length)
      return memcmp(received, message, length) == 0 ? SALTS_OK : SALTS_EPROTO;
  }
  return SALTS_ETIMEDOUT;
}

static int truncated_and_tampered_record_rejected(tlcp_pair *pair) {
  static const char payload[] = "CNet-TLCP-authenticated-record";
  unsigned char ciphertext[4096];
  unsigned char plaintext[sizeof(payload)];
  size_t captured = 0u;
  size_t read_size = 17u;
  bool complete = false;
  bool closed = true;
  int status;
  size_t index;

  status = cnet_tls_write(&pair->client, payload, sizeof(payload) - 1u,
                          &complete);
  if (status != SALTS_OK || !complete) return SALTS_EPROTO;
  for (;;) {
    size_t size = 0u;
    status = cnet_tls_take_cipher(&pair->client, ciphertext + captured,
                                  sizeof(ciphertext) - captured, &size);
    if (status == SALTS_ENOENT) break;
    if (status != SALTS_OK || size == 0u ||
        size > sizeof(ciphertext) - captured)
      return SALTS_EPROTO;
    captured += size;
  }
  if (captured <= 6u) return SALTS_EPROTO;

  /* A truncated record is retained but must expose no unauthenticated bytes. */
  status = cnet_tls_feed_cipher(&pair->server, ciphertext, captured - 1u);
  if (status != SALTS_OK) return status;
  memset(plaintext, 0xa5, sizeof(plaintext));
  status = cnet_tls_read(&pair->server, plaintext, sizeof(plaintext),
                         &read_size, &closed);
  if (status != SALTS_OK || read_size != 0u || closed) return SALTS_EPROTO;

  ciphertext[captured - 1u] ^= 0x01u;
  status = cnet_tls_feed_cipher(&pair->server, ciphertext + captured - 1u, 1u);
  if (status != SALTS_OK) return status;
  status = cnet_tls_read(&pair->server, plaintext, sizeof(plaintext),
                         &read_size, &closed);
  if (status != SALTS_EPROTO || read_size != 0u || closed)
    return SALTS_EPROTO;
  for (index = 0u; index < sizeof(plaintext); ++index)
    if (plaintext[index] != 0xa5u) return SALTS_EPROTO;
  return SALTS_OK;
}

static int good(const char *ca, const char *certs, const char *keys) {
  tlcp_pair pair = {0};
  char version[24] = {0};
  char cipher[64] = {0};
  size_t size = 0u;
  int status = pair_create(&pair, ca, certs, keys, "localhost",
                           CNET_TLS_SECURITY_PROFILE_TLCP11);
  if (status != SALTS_OK) goto done;
  status = cnet_tls_state_set_protocol_range(
      &pair.client, CNET_TLS_PROTOCOL_VERSION_DEFAULT,
      CNET_TLS_PROTOCOL_VERSION_DEFAULT);
  if (status != SALTS_OK) goto done;
  status = cnet_tls_state_set_protocol_range(
      &pair.client, CNET_TLS_PROTOCOL_VERSION_1_2,
      CNET_TLS_PROTOCOL_VERSION_1_3);
  if (status != SALTS_EINVAL) {
    status = SALTS_EPROTO;
    goto done;
  }
  status = handshake(&pair);
  if (status != SALTS_OK) goto done;
  status = cnet_tls_state_negotiated_version(&pair.client, version,
                                             sizeof(version), &size);
  if (status != SALTS_OK || strcmp(version, "TLCPv1.1") != 0) {
    status = SALTS_EPROTO;
    goto done;
  }
  status = cnet_tls_state_negotiated_cipher(&pair.client, cipher,
                                            sizeof(cipher), &size);
  if (status != SALTS_OK || strcmp(cipher, "TLS_ECC_SM4_GCM_SM3") != 0) {
    status = SALTS_EPROTO;
    goto done;
  }
  status = exchange(&pair.client, &pair.server, "CNet-TLCP-client-record");
  if (status == SALTS_OK)
    status = exchange(&pair.server, &pair.client, "CNet-TLCP-server-record");
  if (status == SALTS_OK)
    status = truncated_and_tampered_record_rejected(&pair);
done:
  pair_destroy(&pair);
  if (status != SALTS_OK)
    fprintf(stderr, "CNet TLCP verified handshake/record failed: %d\n", status);
  return status == SALTS_OK;
}

static int rejected(const char *ca, const char *certs, const char *keys,
                    const char *hostname, cnet_tls_security_profile profile) {
  tlcp_pair pair = {0};
  int status = pair_create(&pair, ca, certs, keys, hostname, profile);
  if (status == SALTS_OK) status = handshake(&pair);
  pair_destroy(&pair);
  return status != SALTS_OK;
}

static int invalid_identity_rejected(const char *certs, const char *keys) {
  cnet_tls_server_config config = {0};
  cnet_tls_server server = {0};
  int status;
  config.size = sizeof(config);
  config.cert_file = certs;
  config.key_file = keys;
  config.key_password = "fixture-only-password";
  config.security_profile = CNET_TLS_SECURITY_PROFILE_TLCP11;
  status = cnet_tls_server_init(&server, &config);
  (void)cnet_tls_server_destroy(&server);
  return status != SALTS_OK;
}

static int unsupported_mtls_rejected(const char *ca, const char *certs,
                                     const char *keys) {
  cnet_tls_client_config client_config = {0};
  cnet_tls_server_config server_config = {0};
  cnet_tls_context *client = NULL;
  cnet_tls_server server = {0};
  int status;

  client_config.size = sizeof(client_config);
  client_config.ca_file = ca;
  client_config.cert_file = certs;
  client_config.key_file = keys;
  client_config.key_password = "fixture-only-password";
  client_config.security_profile = CNET_TLS_SECURITY_PROFILE_TLCP11;
  status = cnet_tls_client_context_create(&client_config, &client);
  if (status != SALTS_ENOTSUP || client != NULL) return 0;

  server_config.size = sizeof(server_config);
  server_config.cert_file = certs;
  server_config.key_file = keys;
  server_config.key_password = "fixture-only-password";
  server_config.ca_file = ca;
  server_config.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
  server_config.security_profile = CNET_TLS_SECURITY_PROFILE_TLCP11;
  status = cnet_tls_server_init(&server, &server_config);
  (void)cnet_tls_server_destroy(&server);
  return status == SALTS_ENOTSUP;
}

int main(int argc, char **argv) {
  if (argc != 9) {
    fprintf(stderr, "usage: %s ca certs keys untrusted-ca mismatched-keys malformed-certs reversed-certs reversed-keys\n",
            argv[0]);
    return 2;
  }
#define REQUIRE_CASE(expression, label)                                      \
  do {                                                                       \
    if (!(expression)) {                                                     \
      fprintf(stderr, "CNet TLCP case failed: %s\n", label);               \
      return 1;                                                              \
    }                                                                        \
  } while (0)
  REQUIRE_CASE(good(argv[1], argv[2], argv[3]), "verified-profile");
  REQUIRE_CASE(rejected(argv[4], argv[2], argv[3], "localhost",
                        CNET_TLS_SECURITY_PROFILE_TLCP11), "wrong-ca");
  REQUIRE_CASE(rejected(argv[1], argv[2], argv[3], "wrong.example",
                        CNET_TLS_SECURITY_PROFILE_TLCP11), "wrong-san");
  REQUIRE_CASE(rejected(argv[1], argv[2], argv[3], "localhost",
                        CNET_TLS_SECURITY_PROFILE_STANDARD), "standard-peer");
  REQUIRE_CASE(rejected(argv[1], argv[2], argv[3], "localhost",
                        CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13),
               "rfc8998-peer");
  REQUIRE_CASE(invalid_identity_rejected(argv[2], argv[5]), "mismatched-key");
  REQUIRE_CASE(invalid_identity_rejected(argv[6], argv[3]), "malformed-cert");
  REQUIRE_CASE(invalid_identity_rejected(argv[7], argv[8]), "reversed-roles");
  REQUIRE_CASE(unsupported_mtls_rejected(argv[1], argv[2], argv[3]),
               "mtls-fail-fast");
#undef REQUIRE_CASE
  puts("CNet strict TLCP 1.1 owner/ring profile: PASS");
  return 0;
}
