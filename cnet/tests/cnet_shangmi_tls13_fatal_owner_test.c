/*
 * Test-only forced NativeIO short-write qualification for a provider-owned
 * RFC8998 fatal alert. Linux epoll initially; no production hooks or fallback.
 * All SM2 identities come from the external temporary fixture generator.
 */
#define _POSIX_C_SOURCE 200809L
#include "cnet_module.h"
#include "cnet_owner.h"
#include "cnet_tls.h"
#include <salts/clock.h>
#include <salts/error_codes.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(CNET_INTERNAL_TESTING)
#error "The fatal alert short-write gate requires Linux and private CNet test hooks"
#endif

#define CHECK_OK(call) do { \
  const int rc_ = (call); \
  if (rc_ != SALTS_OK) { \
    fprintf(stderr, "fatal-alert owner: %s failed at line %d, status=%d\n", \
            #call, __LINE__, rc_); \
    return 1; \
  } \
} while (0)

#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "fatal-alert owner: assertion failed at line %d: %s\n", \
            __LINE__, #condition); \
    return 1; \
  } \
} while (0)

static int make_connected_tcp(int *out_listener, int *out_client, int *out_accepted) {
  struct sockaddr_in address = {0};
  socklen_t addrlen = (socklen_t)sizeof(address);
  int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int peer = -1;
  int accepted = -1;
  if (listener < 0) return SALTS_EIO;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &addrlen) != 0 ||
      listen(listener, 1) != 0) goto fail;
  peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (peer < 0 || connect(peer, (struct sockaddr *)&address, sizeof(address)) != 0)
    goto fail;
  accepted = accept(listener, NULL, NULL);
  if (accepted < 0) goto fail;
  *out_listener = listener;
  *out_client = peer;
  *out_accepted = accepted;
  return SALTS_OK;
fail:
  if (accepted >= 0) (void)close(accepted);
  if (peer >= 0) (void)close(peer);
  (void)close(listener);
  return SALTS_EIO;
}

static int send_all(int fd, const unsigned char *data, size_t count) {
  size_t offset = 0u;
  while (offset < count) {
    ssize_t sent = send(fd, data + offset, count - offset, 0);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return SALTS_EIO;
    offset += (size_t)sent;
  }
  return SALTS_OK;
}

int main(int argc, char **argv) {
  cnet_tls_server server = {0};
  cnet_tls_server_config server_cfg = {0};
  cnet_tls_client_config client_cfg = {0};
  cnet_tls_context *client_ctx = NULL;
  cnet_tls_state client_tls = {0};
  cnet_session_table sessions = {0};
  cnet_command_queue commands = {0};
  cnet_event_queue events = {0};
  cnet_owner owner = {0};
  const cnet_command_queue_config command_cfg = {
      16u, sizeof(cnet_owner_connect_payload)};
  const cnet_event_queue_config event_cfg = {16u, 2u, 64u};
  const cnet_owner_config owner_cfg = {
      .backend_kind = NATIVE_IO_BACKEND_EPOLL,
      .connection_capacity = 1u,
      .request_capacity = 8u,
      .completion_batch_capacity = 8u,
      .receive_buffer_bytes = 64u,
      .receive_buffer_count = 1u,
      .write_capacity = 8u,
      .max_write_bytes = 4096u,
      .sessions = &sessions,
      .commands = &commands,
      .events = &events};
  cnet_owner_connect_payload payload = {0};
  cnet_command command = {0};
  cnet_session_handle handle = {0};
  cnet_session_terminal terminal = {0};
  unsigned char client_hello[16384] = {0};
  unsigned char alert[8] = {0};
  size_t hello_size = 0u, alert_size = 0u;
  int listener = -1, peer = -1, accepted = -1;
  unsigned int handshaking_events = 0u, failure_events = 0u;
  unsigned int connected_events = 0u, plaintext_events = 0u;
  bool complete = false, terminal_seen = false;
  uint64_t deadline;

  if (argc != 4) {
    fprintf(stderr, "usage: fatal_owner_probe root.pem server.pem server-key.pem\n");
    return 2;
  }

  CHECK_OK(cnet_module_init());
  server_cfg.size = sizeof(server_cfg);
  server_cfg.cert_file = argv[2];
  server_cfg.key_file = argv[3];
  server_cfg.key_password = "fixture-only-password";
  server_cfg.security_profile = CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13;
  CHECK_OK(cnet_tls_server_init(&server, &server_cfg));

  client_cfg.size = sizeof(client_cfg);
  client_cfg.ca_file = argv[1];
  client_cfg.security_profile = CNET_TLS_SECURITY_PROFILE_STANDARD;
  CHECK_OK(cnet_tls_client_context_create(&client_cfg, &client_ctx));
  CHECK_OK(cnet_tls_state_init(&client_tls, client_ctx, false, "localhost",
                               CNET_TLS_MIN_IO_BUFFER_BYTES));
  client_ctx = NULL; /* cnet_tls_state_init adopts the context on success. */
  CHECK_OK(cnet_tls_handshake(&client_tls, &complete));
  CHECK(!complete);
  CHECK_OK(cnet_tls_take_cipher(&client_tls, client_hello,
                                sizeof(client_hello), &hello_size));
  CHECK(hello_size > 7u);

  CHECK_OK(make_connected_tcp(&listener, &peer, &accepted));
  CHECK_OK(cnet_session_table_init(&sessions, 1u));
  CHECK_OK(cnet_command_queue_init(&commands, &command_cfg));
  CHECK_OK(cnet_event_queue_init(&events, &event_cfg));
  CHECK_OK(cnet_owner_init(&owner, &owner_cfg));
  CHECK_OK(cnet_owner_test_set_send_chunk_bytes(&owner, 1u));
  CHECK_OK(cnet_session_table_reserve(&sessions, &handle));

  payload.scheme = CNET_URI_TLS;
  payload.adopted = true;
  payload.adopted_socket = (uintptr_t)accepted;
  payload.tls_server = true;
  payload.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  payload.tls_handshake_timeout_ms = 5000u;
  payload.tls_context = cnet_tls_server_context(&server);
  CHECK(payload.tls_context != NULL);
  cnet_tls_context_retain(payload.tls_context);
  command = (cnet_command){CNET_COMMAND_CONNECT, handle, &payload,
                           sizeof(payload), 0u};
  CHECK_OK(cnet_command_queue_publish(&commands, &command));
  accepted = -1; /* The queued Owner CONNECT command owns this descriptor. */

  CHECK_OK(send_all(peer, client_hello, hello_size));
  deadline = cmeta_monotonic_ms() + 5000u;
  while (cmeta_monotonic_ms() < deadline) {
    cnet_event_view event = {0};
    cnet_session_state state = CNET_SESSION_FREE;
    int status;
    ssize_t n;

    CHECK_OK(cnet_owner_drive(&owner, 2u));
    for (;;) {
      status = cnet_event_queue_take(&events, &event);
      if (status == SALTS_ETIMEDOUT) break;
      CHECK(status == SALTS_OK);
      if (event.kind == CNET_EVENT_RECEIVE) ++plaintext_events;
      if (event.kind == CNET_EVENT_STATE) {
        if (event.state == CNET_EVENT_STATE_TLS_HANDSHAKING)
          ++handshaking_events;
        if (event.state == CNET_EVENT_STATE_CONNECTED) ++connected_events;
        if (event.state == CNET_EVENT_STATE_FAILED) {
          ++failure_events;
          CHECK(event.stage == CNET_SESSION_STAGE_HANDSHAKE);
        }
      }
      CHECK_OK(cnet_event_queue_release(&events, &event));
    }

    n = recv(peer, alert + alert_size, sizeof(alert) - alert_size, MSG_DONTWAIT);
    if (n > 0) alert_size += (size_t)n;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      CHECK(false);
    CHECK(alert_size <= 7u);
    CHECK_OK(cnet_session_table_state(&sessions, handle, &state));
    terminal_seen = state == CNET_SESSION_TERMINAL;
    if (terminal_seen && failure_events == 1u && alert_size == 7u) break;
  }

  CHECK(terminal_seen);
  /* Adopted TLS sockets enter handshake without publishing a duplicate
   * TLS_HANDSHAKING event: cnet_owner_start_tls(..., false).
   * The visible FAILED terminal and its HANDSHAKE stage remain mandatory. */
  CHECK(handshaking_events == 0u);
  CHECK(failure_events == 1u);
  CHECK(connected_events == 0u && plaintext_events == 0u);
  CHECK(alert_size == 7u && alert[0] == 21u && alert[1] == 3u &&
        alert[2] == 3u && alert[3] == 0u && alert[4] == 2u &&
        alert[5] == 2u && alert[6] != 0u);
  CHECK_OK(cnet_session_table_take_terminal(&sessions, handle, &terminal));
  CHECK(terminal.kind == CNET_SESSION_TERMINAL_FAILED);
  CHECK(terminal.stage == CNET_SESSION_STAGE_HANDSHAKE);
  CHECK(terminal.status != SALTS_OK);
  CHECK_OK(cnet_session_table_recycle(&sessions, handle));
  CHECK_OK(cnet_owner_release_session(&owner, handle));
  CHECK_OK(cnet_owner_drive(&owner, 0u));
  {
    cnet_event_view event = {0};
    CHECK(cnet_event_queue_take(&events, &event) == SALTS_ETIMEDOUT);
  }

  (void)close(peer);
  (void)close(listener);
  cnet_tls_state_destroy(&client_tls);
  CHECK_OK(cnet_tls_server_destroy(&server));
  CHECK_OK(cnet_command_queue_close(&commands));
  CHECK_OK(cnet_owner_close(&owner));
  CHECK_OK(cnet_owner_destroy(&owner));
  CHECK_OK(cnet_event_queue_close(&events));
  CHECK_OK(cnet_event_queue_destroy(&events));
  CHECK_OK(cnet_command_queue_destroy(&commands));
  CHECK_OK(cnet_session_table_destroy(&sessions));
  CHECK_OK(cnet_module_shutdown());
  puts("CNet TLS fatal-alert partial NativeIO owner: PASS");
  return 0;
}
