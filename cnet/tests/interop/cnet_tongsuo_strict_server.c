/*
 * #1134 test-only public CNet NativeIO server for Tongsuo strict RFC8998.
 * No Tongsuo/GmSSL headers or direct symbols. Emits its ephemeral port
 * on stdout once, then verifies TLS1.3+SM4-GCM-SM3, receives and echoes
 * application data through the existing CNet owner and bounded buffers.
 */
#include <cnet/cnet.h>
#include <salts/clock.h>
#include <salts/error_codes.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char kMessage[] = "tongsuo-cnet-rfc8998-proof\n";

typedef struct server_witness {
  cnet_client *client;
  cnet_connection connection;
  unsigned char data[128];
  size_t data_size;
  int connected;
  int failed;
  int terminal;
  int send_count;
} server_witness;

static void on_state(void *user, cnet_connection connection,
                     cnet_connection_state state, const cnet_error *error) {
  server_witness *w = (server_witness *)user;
  if (w == NULL || w->client == NULL) return;
  w->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    char version[24] = {0};
    char cipher[64] = {0};
    size_t len = 0u;
    if (cnet_tls_negotiated_version(w->client, connection, version,
                                    sizeof(version), &len) != SALTS_OK ||
        strcmp(version, "TLSv1.3") != 0 ||
        cnet_tls_negotiated_cipher(w->client, connection, cipher,
                                   sizeof(cipher), &len) != SALTS_OK ||
        strcmp(cipher, "TLS_SM4_GCM_SM3") != 0) {
      w->failed = 1;
      return;
    }
    w->connected = 1;
  }
  if (state == CNET_CONNECTION_CLOSED ||
      state == CNET_CONNECTION_FAILED) w->terminal = 1;
  if (state == CNET_CONNECTION_FAILED || error != NULL) w->failed = 1;
}

static void on_receive(void *user, cnet_connection connection,
                       const cnet_receive_view *view) {
  server_witness *w = (server_witness *)user;
  (void)connection;
  if (w == NULL || view == NULL || view->kind != CNET_MESSAGE_BYTES ||
      view->data == NULL || view->size == 0u ||
      view->size > sizeof(w->data) - w->data_size) {
    if (w != NULL) w->failed = 1;
    return;
  }
  memcpy(w->data + w->data_size, view->data, view->size);
  w->data_size += view->size;
}

static void on_send(void *user, cnet_connection connection, size_t size) {
  server_witness *w = (server_witness *)user;
  (void)connection;
  if (w != NULL) {
    if (size == 0u) w->failed = 1;
    ++w->send_count;
  }
}

static int echo_message(cnet_client *client, cnet_connection connection) {
  const size_t length = sizeof(kMessage) - 1u;
  mem_buffer_t *buffer = mem_get_buffer(mem_global(), length);
  int status;
  if (buffer == NULL) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), kMessage, length);
  mem_set_used(buffer, length);
  status = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

int main(int argc, char **argv) {
  cnet_client server = {0};
  cnet_listener listener = {0};
  cnet_tls_server tls = {0};
  cnet_connection connection = {0};
  cnet_client_config server_cfg = {0};
  cnet_listener_config listener_cfg = {0};
  cnet_tls_server_config tls_cfg = {0};
  cnet_observer observer = {0};
  server_witness w = {0};
  uint16_t port = 0u;
  uint64_t deadline;
  size_t events = 0u;
  int server_initialized = 0, listener_initialized = 0;
  int accepted = 0, receive_armed = 0, echoed = 0, ready = 0, ok = 0;

  if (argc != 3) {
    fputs("usage: cnet_tongsuo_server server-cert.pem server-key.pem\n", stderr);
    return 2;
  }
  server_cfg.backend = NATIVE_IO_BACKEND_EPOLL;
  server_cfg.connection_capacity = 1u;
  server_cfg.command_capacity = 16u;
  server_cfg.request_capacity = 16u;
  server_cfg.completion_batch_capacity = 8u;
  server_cfg.event_capacity = 16u;
  server_cfg.max_send_bytes = 4096u;
  server_cfg.receive_buffer_bytes = 4096u;
  server_cfg.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  server_cfg.tls_handshake_timeout_ms = 5000u;
  server_cfg.read_timeout_ms = 5000u;
  server_cfg.write_timeout_ms = 5000u;
  server_cfg.connect_timeout_ms = 5000u;

  tls_cfg.size = sizeof(tls_cfg);
  tls_cfg.cert_file = argv[1];
  tls_cfg.key_file = argv[2];
  tls_cfg.key_password = "fixture-only-password";
  tls_cfg.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
  tls_cfg.security_profile = CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13;
  if (cnet_tls_server_init(&tls, &tls_cfg) != SALTS_OK) goto done;
  if (cnet_client_init(&server, &server_cfg) != SALTS_OK) goto done;
  server_initialized = 1;

  listener_cfg.backend = NATIVE_IO_BACKEND_EPOLL;
  listener_cfg.host = "127.0.0.1";
  listener_cfg.port = 0u;
  listener_cfg.backlog = 2u;
  if (cnet_listener_init(&listener, &listener_cfg) != SALTS_OK) goto done;
  listener_initialized = 1;
  if (cnet_listener_port(&listener, &port) != SALTS_OK || port == 0u)
    goto done;
  /* Python launch controller must observe this before connecting Tongsuo. */
  printf("%u\n", (unsigned)port);
  fflush(stdout);

  w.client = &server;
  observer.on_state = on_state;
  observer.on_receive = on_receive;
  observer.on_send = on_send;
  observer.user = &w;

  deadline = cmeta_monotonic_ms() + 12000u;
  while (cmeta_monotonic_ms() < deadline && !w.failed) {
    int status;
    if (!accepted) {
      status = cnet_listener_wait(&listener, 20u, &ready);
      if (status != SALTS_OK) goto done;
      if (ready) {
        status = cnet_listener_accept_tls(
            &listener, &server, &tls, &observer, &connection);
        if (status != SALTS_OK) goto done;
        accepted = 1;
      }
    }
    status = cnet_client_poll(&server, 20u, &events);
    if (status != SALTS_OK) goto done;
    if (w.connected && !receive_armed) {
      if (cnet_receive(&server, connection, sizeof(w.data)) != SALTS_OK)
        goto done;
      receive_armed = 1;
    }
    if (w.data_size == sizeof(kMessage) - 1u && !echoed) {
      if (memcmp(w.data, kMessage, sizeof(kMessage) - 1u) != 0)
        goto done;
      if (echo_message(&server, connection) != SALTS_OK)
        goto done;
      echoed = 1;
    }
    if (echoed && w.send_count != 0) {
      ok = 1;
      break;
    }
    if (w.terminal) break;
  }

done:
  if (listener_initialized) {
    if (cnet_listener_close(&listener) != SALTS_OK) ok = 0;
    if (cnet_listener_destroy(&listener) != SALTS_OK) ok = 0;
  }
  if (server_initialized) {
    if (accepted && !w.terminal) {
      (void)cnet_close(&server, connection);
      deadline = cmeta_monotonic_ms() + 2000u;
      while (!w.terminal && cmeta_monotonic_ms() < deadline)
        (void)cnet_client_poll(&server, 20u, &events);
    }
    if (cnet_client_stop(&server, 2000u) != SALTS_OK) ok = 0;
    if (cnet_client_destroy(&server) != SALTS_OK) ok = 0;
  }
  if (cnet_tls_server_destroy(&tls) != SALTS_OK) ok = 0;
  if (!ok) {
    fprintf(stderr, "CNet NativeIO strict RFC8998 server failed: "
                    "accepted=%d connected=%d failed=%d received=%zu sent=%d\n",
            accepted, w.connected, w.failed, w.data_size, w.send_count);
    return 1;
  }
  puts("Public CNet NativeIO server -> strict Tongsuo TLS1.3 peer: PASS");
  return 0;
}
