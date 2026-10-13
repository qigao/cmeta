/*
 * #1134 test-only public CNet NativeIO client against Tongsuo strict RFC8998.
 * No direct GmSSL/Tongsuo headers, API calls, or alternative production TLS.
 */
#include <cnet/cnet.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kMessage[] = "cnet-tongsuo-rfc8998-proof\n";

typedef struct witness {
  cnet_client *client;
  cnet_connection connection;
  char received[256];
  size_t received_size;
  int connected;
  int terminal;
  int failed;
  int sent;
} witness;

static void on_state(void *user, cnet_connection connection,
                     cnet_connection_state state, const cnet_error *error) {
  witness *w = (witness *)user;
  if (w == NULL || w->client == NULL) return;
  w->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    char version[24] = {0};
    char cipher[64] = {0};
    size_t size = 0u;
    if (cnet_tls_negotiated_version(w->client, connection, version,
                                    sizeof(version), &size) != SALTS_OK ||
        strcmp(version, "TLSv1.3") != 0 ||
        cnet_tls_negotiated_cipher(w->client, connection, cipher,
                                   sizeof(cipher), &size) != SALTS_OK ||
        strcmp(cipher, "TLS_SM4_GCM_SM3") != 0) {
      w->failed = 1;
      return;
    }
    w->connected = 1;
  }
  if (state == CNET_CONNECTION_FAILED) w->failed = 1;
  if (state == CNET_CONNECTION_FAILED ||
      state == CNET_CONNECTION_CLOSED) w->terminal = 1;
  if (error != NULL) w->failed = 1;
}

static void on_receive(void *user, cnet_connection connection,
                       const cnet_receive_view *view) {
  witness *w = (witness *)user;
  (void)connection;
  if (w == NULL || view == NULL || view->kind != CNET_MESSAGE_BYTES ||
      view->data == NULL || view->size == 0u ||
      view->size > sizeof(w->received) - w->received_size - 1u) {
    if (w != NULL) w->failed = 1;
    return;
  }
  memcpy(w->received + w->received_size, view->data, view->size);
  w->received_size += view->size;
  w->received[w->received_size] = '\0';
}

static void on_send(void *user, cnet_connection connection, size_t size) {
  witness *w = (witness *)user;
  (void)connection;
  if (w != NULL) {
    if (size == 0u) w->failed = 1;
    ++w->sent;
  }
}

static int send_message(cnet_client *client, cnet_connection connection) {
  const size_t n = sizeof(kMessage) - 1u;
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), n);
  int status;
  if (buffer == NULL) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), kMessage, n);
  mem_set_used(buffer, n);
  status = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

int main(int argc, char **argv) {
  cnet_client client = {0};
  cnet_tls_client tls = {0};
  cnet_connection connection = {0};
  cnet_client_config cfg = {0};
  cnet_tls_client_config policy = {0};
  cnet_connect_options options = {0};
  witness w = {0};
  char uri[90];
  char reversed[sizeof(kMessage)] = {0};
  size_t i, count = 0u;
  uint64_t deadline;
  int client_initialized = 0, status, ok = 0, reject_peer = 0;

  if (argc != 3 && argc != 4) {
    fputs("usage: cnet_tongsuo_client ca-cert.pem port [wrong-ca|wrong-san]\n", stderr);
    return 2;
  }
  if (argc == 4) {
    if (strcmp(argv[3], "wrong-ca") != 0 &&
        strcmp(argv[3], "wrong-san") != 0) return 2;
    reject_peer = 1;
  }
  if (snprintf(uri, sizeof(uri), "tls://127.0.0.1:%s", argv[2]) < 1)
    return 2;
  for (i = 0u; i < sizeof(kMessage) - 2u; ++i)
    reversed[i] = kMessage[sizeof(kMessage) - 3u - i];

  cfg.backend = NATIVE_IO_BACKEND_EPOLL;
  cfg.connection_capacity = 1u;
  cfg.command_capacity = 16u;
  cfg.request_capacity = 16u;
  cfg.completion_batch_capacity = 8u;
  cfg.event_capacity = 16u;
  cfg.max_send_bytes = 4096u;
  cfg.receive_buffer_bytes = 4096u;
  cfg.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  cfg.connect_timeout_ms = 5000u;
  cfg.tls_handshake_timeout_ms = 5000u;
  cfg.read_timeout_ms = 5000u;
  cfg.write_timeout_ms = 5000u;
  if (cnet_client_init(&client, &cfg) != SALTS_OK) goto done;
  client_initialized = 1;

  policy.size = sizeof(policy);
  policy.ca_file = argv[1];
  policy.server_name = argc == 4 && strcmp(argv[3], "wrong-san") == 0
                         ? "wrong.example" : "localhost";
  policy.security_profile = CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13;
  if (cnet_tls_client_init(&tls, &policy) != SALTS_OK) goto done;

  w.client = &client;
  options.uri = uri;
  options.tls_client = &tls;
  options.observer.on_state = on_state;
  options.observer.on_receive = on_receive;
  options.observer.on_send = on_send;
  options.observer.user = &w;
  if (cnet_connect(&client, &options, &connection) != SALTS_OK) goto done;
  if (cnet_tls_client_destroy(&tls) != SALTS_OK) goto done;

  deadline = cmeta_monotonic_ms() + 10000u;
  while (!w.connected && !w.failed && cmeta_monotonic_ms() < deadline) {
    status = cnet_client_poll(&client, 20u, &count);
    if (status != SALTS_OK) goto done;
  }
  if (reject_peer) {
    /* This is a *peer verification* negative on an admitted TCP socket:
     * the client must terminate without publishing CONNECTED or plaintext. */
    ok = !w.connected && w.failed && w.terminal &&
         w.received_size == 0u && w.sent == 0;
    goto done;
  }
  if (!w.connected || w.failed) goto done;
  if (cnet_receive(&client, connection, sizeof(w.received) - 1u) != SALTS_OK ||
      send_message(&client, connection) != SALTS_OK) goto done;

  while (!w.failed && cmeta_monotonic_ms() < deadline) {
    status = cnet_client_poll(&client, 20u, &count);
    if (status != SALTS_OK) goto done;
    if (strstr(w.received, reversed) != NULL && w.sent > 0) {
      ok = 1;
      break;
    }
    if (w.terminal) break;
  }

done:
  if (client_initialized) {
    if (!w.terminal && connection.slot != 0u) {
      (void)cnet_close(&client, connection);
      deadline = cmeta_monotonic_ms() + 2000u;
      while (!w.terminal && cmeta_monotonic_ms() < deadline)
        (void)cnet_client_poll(&client, 20u, &count);
    }
    if (cnet_client_stop(&client, 2000u) != SALTS_OK) ok = 0;
    if (cnet_client_destroy(&client) != SALTS_OK) ok = 0;
  }
  if (cnet_tls_client_destroy(&tls) != SALTS_OK) ok = 0;
  if (!ok)
    fprintf(stderr, "CNet strict RFC8998/Tongsuo peer test failed: connected=%d "
                    "failed=%d received=%zu sent=%d\n",
            w.connected, w.failed, w.received_size, w.sent);
  else if (reject_peer)
    puts("CNet public NativeIO client rejected independent untrusted Tongsuo peer: PASS");
  else puts("CNet public NativeIO client ↔ independent Tongsuo strict RFC8998: PASS");
  return ok ? 0 : 1;
}
