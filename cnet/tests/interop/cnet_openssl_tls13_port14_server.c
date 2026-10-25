/* CNet public NativeIO <-> independent OpenSSL TLS 1.3 mTLS.
 *
 * This is deliberately NOT a direct GmSSL provider client or a plaintext
 * development gateway. TLS remains owned by production CNet/CNetTLS; a
 * Python/OpenSSL client supplies real default X25519-first ClientHello,
 * HelloRetryRequest, compatibility CCS and P-256 ECDSA certificates.
 *
 * Missing/untrusted/wrong-EKU client identities must never reach the
 * application receive/send callbacks. Only loopback ephemeral test ports.
 */
#include <cnet/cnet.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TLS_PORT14_TIMEOUT_MS = 7000, TLS_PORT14_MAX_DATA = 128 };
static const char kPayload[] = "salts-cnet-hrr-ccs-mtls-probe\n";

typedef struct tls_port14_witness {
  cnet_client *client;
  cnet_connection connection;
  unsigned char payload[TLS_PORT14_MAX_DATA];
  size_t received;
  unsigned int sends;
  bool connected;
  bool terminal;
  bool failed;
} tls_port14_witness;

static void tls_port14_state(void *user, cnet_connection connection,
                             cnet_connection_state state,
                             const cnet_error *error) {
  tls_port14_witness *w = (tls_port14_witness *)user;
  if (!w || !w->client) return;
  w->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    char version[32] = {0}, cipher[96] = {0};
    size_t size = 0u;
    if (cnet_tls_negotiated_version(w->client, connection,
                                   version, sizeof(version), &size) != SALTS_OK ||
        strcmp(version, "TLSv1.3") != 0 ||
        cnet_tls_negotiated_cipher(w->client, connection,
                                   cipher, sizeof(cipher), &size) != SALTS_OK ||
        strcmp(cipher, "TLS_AES_128_GCM_SHA256") != 0) {
      w->failed = true;
      return;
    }
    w->connected = true;
  }
  if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED)
    w->terminal = true;
  if (state == CNET_CONNECTION_FAILED || error != NULL)
    w->failed = true;
}

static void tls_port14_receive(void *user, cnet_connection connection,
                               const cnet_receive_view *view) {
  tls_port14_witness *w = (tls_port14_witness *)user;
  (void)connection;
  if (!w || !view || view->kind != CNET_MESSAGE_BYTES ||
      !view->data || view->size == 0u ||
      view->size > sizeof(w->payload) - w->received) {
    if (w) w->failed = true;
    return;
  }
  memcpy(w->payload + w->received, view->data, view->size);
  w->received += view->size;
}
static void tls_port14_send_done(void *user, cnet_connection connection,
                                size_t size) {
  tls_port14_witness *w = (tls_port14_witness *)user;
  (void)connection;
  if (!w) return;
  if (!size) w->failed = true;
  ++w->sends;
}

static int tls_port14_echo(cnet_client *client, cnet_connection connection) {
  const size_t n = sizeof(kPayload) - 1u;
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), n);
  int rc;
  if (!buffer) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), kPayload, n);
  mem_set_used(buffer, n);
  rc = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return rc;
}

int main(int argc, char **argv) {
  cnet_client client = {0};
  cnet_listener listener = {0};
  cnet_tls_server tls = {0};
  cnet_connection connection = {0};
  cnet_client_config client_cfg = {0};
  cnet_listener_config listener_cfg = {0};
  cnet_tls_server_config tls_cfg = {0};
  cnet_observer observer = {0};
  tls_port14_witness w = {0};
  uint16_t port = 0u;
  uint64_t deadline;
  size_t count = 0u;
  bool client_ready = false, listener_ready = false, accepted = false;
  bool receive_armed = false, echoed = false, ok = false, expect_reject = false;
  static const char *const alpn[] = {"http/1.1"};

  if (argc != 4 && argc != 5) {
    fputs("usage: cnet-port14-mtls ca.pem server-cert.pem server-key.pem [reject]\n", stderr);
    return 2;
  }
  if (argc == 5) {
    if (strcmp(argv[4], "reject") != 0) return 2;
    expect_reject = true;
  }
  client_cfg.backend = NATIVE_IO_BACKEND_EPOLL;
  client_cfg.connection_capacity = 1u;
  client_cfg.command_capacity = 16u;
  client_cfg.request_capacity = 16u;
  client_cfg.completion_batch_capacity = 8u;
  client_cfg.event_capacity = 16u;
  client_cfg.max_send_bytes = 4096u;
  client_cfg.receive_buffer_bytes = 4096u;
  client_cfg.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  client_cfg.tls_handshake_timeout_ms = TLS_PORT14_TIMEOUT_MS;
  client_cfg.connect_timeout_ms = TLS_PORT14_TIMEOUT_MS;
  client_cfg.read_timeout_ms = TLS_PORT14_TIMEOUT_MS;
  client_cfg.write_timeout_ms = TLS_PORT14_TIMEOUT_MS;

  tls_cfg.size = sizeof(tls_cfg);
  tls_cfg.ca_file = argv[1];
  tls_cfg.cert_file = argv[2];
  tls_cfg.key_file = argv[3];
  tls_cfg.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
  tls_cfg.security_profile = CNET_TLS_SECURITY_PROFILE_STANDARD;
  tls_cfg.alpn_protocols = alpn;
  tls_cfg.alpn_protocol_count = 1u;
  if (cnet_tls_server_init(&tls, &tls_cfg) != SALTS_OK)
    goto cleanup;
  if (cnet_client_init(&client, &client_cfg) != SALTS_OK)
    goto cleanup;
  client_ready = true;

  listener_cfg.backend = NATIVE_IO_BACKEND_EPOLL;
  listener_cfg.host = "127.0.0.1";
  listener_cfg.port = 0u;
  listener_cfg.backlog = 2u;
  if (cnet_listener_init(&listener, &listener_cfg) != SALTS_OK)
    goto cleanup;
  listener_ready = true;
  if (cnet_listener_port(&listener, &port) != SALTS_OK || port == 0u)
    goto cleanup;
  printf("CNET_PORT14_TLS_PORT=%u\n", (unsigned int)port);
  fflush(stdout);

  w.client = &client;
  observer.on_state = tls_port14_state;
  observer.on_receive = tls_port14_receive;
  observer.on_send = tls_port14_send_done;
  observer.user = &w;
  deadline = cmeta_monotonic_ms() + TLS_PORT14_TIMEOUT_MS + 2000u;
  while (cmeta_monotonic_ms() < deadline && !w.failed) {
    if (!accepted) {
      int ready = 0;
      if (cnet_listener_wait(&listener, 20u, &ready) != SALTS_OK)
        goto cleanup;
      if (ready) {
        if (cnet_listener_accept_tls(&listener, &client,
                                     &tls, &observer, &connection) != SALTS_OK)
          goto cleanup;
        accepted = true;
      }
    }
    if (cnet_client_poll(&client, 20u, &count) != SALTS_OK)
      goto cleanup;
    if (w.connected && !receive_armed) {
      if (cnet_receive(&client, connection, sizeof(w.payload)) != SALTS_OK)
        goto cleanup;
      receive_armed = true;
    }
    if (w.received == sizeof(kPayload) - 1u && !echoed) {
      if (memcmp(w.payload, kPayload, sizeof(kPayload) - 1u) != 0)
        goto cleanup;
      if (tls_port14_echo(&client, connection) != SALTS_OK)
        goto cleanup;
      echoed = true;
    }
    if (echoed && w.sends > 0u) {
      ok = !expect_reject && w.connected && !w.failed;
      break;
    }
    if (w.terminal) break;
  }
  if (expect_reject)
    ok = accepted && w.terminal && !echoed &&
         w.received == 0u && w.sends == 0u;

cleanup:
  if (listener_ready) {
    if (cnet_listener_close(&listener) != SALTS_OK) ok = false;
    if (cnet_listener_destroy(&listener) != SALTS_OK) ok = false;
  }
  if (client_ready) {
    if (accepted && !w.terminal) {
      (void)cnet_close(&client, connection);
      deadline = cmeta_monotonic_ms() + 2000u;
      while (!w.terminal && cmeta_monotonic_ms() < deadline)
        (void)cnet_client_poll(&client, 20u, &count);
    }
    if (cnet_client_stop(&client, 2000u) != SALTS_OK) ok = false;
    if (cnet_client_destroy(&client) != SALTS_OK) ok = false;
  }
  if (cnet_tls_server_destroy(&tls) != SALTS_OK) ok = false;
  if (!ok)
    fprintf(stderr, "CNet TLS13 port14 mTLS failed: accepted=%d connected=%d "
                    "failed=%d terminal=%d received=%zu sent=%u expected-reject=%d\n",
            (int)accepted, (int)w.connected, (int)w.failed, (int)w.terminal,
            w.received, w.sends, (int)expect_reject);
  else puts(expect_reject
        ? "CNet TLS13 port14 rejected invalid OpenSSL mTLS peer: PASS"
        : "CNet TLS13 port14 verified independent OpenSSL mTLS echo: PASS");
  return ok ? 0 : 1;
}
