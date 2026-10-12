#include <cnet/cnet.h>

#include <salts/clock.h>
#include <salts/error_codes.h>

#include <tinytest.h>
#include "cnet_external_test_cleanup.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_BATCH = 16, TEST_TIMEOUT_MS = 5000 };

#ifndef CNET_SHANGMI_TLS13_TEST
#define CNET_SHANGMI_TLS13_TEST 0
#endif
#if CNET_SHANGMI_TLS13_TEST
#define CNET_TLS_TEST_SECURITY_PROFILE CNET_TLS_SECURITY_PROFILE_SHANGMI_TLS13
#define CNET_TLS_TEST_IDENTITY "localhost"
#define CNET_TLS_TEST_SERVER_PASSWORD "fixture-only-password"
#define CNET_EXTERNAL_TLS_CA getenv("SALTS_SHANGMI_CA")
#define CNET_EXTERNAL_TLS_CERT getenv("SALTS_SHANGMI_CERT")
#define CNET_EXTERNAL_TLS_KEY getenv("SALTS_SHANGMI_KEY")
#else
#define CNET_TLS_TEST_SECURITY_PROFILE CNET_TLS_SECURITY_PROFILE_STANDARD
#define CNET_TLS_TEST_IDENTITY "127.0.0.1"
#define CNET_TLS_TEST_SERVER_PASSWORD NULL
#endif

#ifndef CNET_EXTERNAL_TLS_CA
#error CNET_EXTERNAL_TLS_CA is required
#endif
#ifndef CNET_EXTERNAL_TLS_CERT
#error CNET_EXTERNAL_TLS_CERT is required
#endif
#ifndef CNET_EXTERNAL_TLS_KEY
#error CNET_EXTERNAL_TLS_KEY is required
#endif

typedef struct state_probe {
  cnet_client *client;
  cnet_connection connection;
  bool connected;
  bool terminal;
  bool failed;
} state_probe;

static native_io_backend_kind test_backend_kind(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static cnet_client_config test_client_config(void) {
  cnet_client_config config;
  memset(&config, 0, sizeof(config));
  config.backend = test_backend_kind();
  config.connection_capacity = 1u;
  config.command_capacity = 16u;
  config.request_capacity = 16u;
  config.completion_batch_capacity = TEST_BATCH;
  config.event_capacity = 32u;
  config.max_send_bytes = 64u * 1024u;
  config.receive_buffer_bytes = 32u * 1024u;
  config.connect_timeout_ms = TEST_TIMEOUT_MS;
  config.read_timeout_ms = TEST_TIMEOUT_MS;
  config.write_timeout_ms = TEST_TIMEOUT_MS;
  config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  config.tls_handshake_timeout_ms = TEST_TIMEOUT_MS;
  return config;
}

static void on_state(void *user, cnet_connection connection,
                     cnet_connection_state state,
                     const cnet_error *error) {
  state_probe *probe = (state_probe *)user;
  check_warn(probe != NULL);
  if (probe == NULL) return;
  probe->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    char version[16] = {0};
    size_t version_size = 0u;
    int status = cnet_tls_negotiated_version(
        probe->client, connection, version, sizeof(version), &version_size);
    if (status != SALTS_OK || strcmp(version, "TLSv1.3") != 0) {
      probe->failed = true;
      return;
    }
#if CNET_SHANGMI_TLS13_TEST
    {
      char cipher[64] = {0};
      size_t cipher_size = 0u;
      status = cnet_tls_negotiated_cipher(
          probe->client, connection, cipher, sizeof(cipher), &cipher_size);
      if (status != SALTS_OK || strcmp(cipher, "TLS_SM4_GCM_SM3") != 0) {
        probe->failed = true;
        return;
      }
      {
        char alpn[32] = {0};
        size_t alpn_size = 0u;
        status = cnet_tls_negotiated_alpn(
            probe->client, connection, alpn, sizeof(alpn), &alpn_size);
        if (status != SALTS_OK || strcmp(alpn, "http/1.1") != 0) {
          probe->failed = true;
          return;
        }
      }
    }
#endif
    probe->connected = true;
  }
  if (state == CNET_CONNECTION_CLOSED ||
      state == CNET_CONNECTION_FAILED)
    probe->terminal = true;
  if (state == CNET_CONNECTION_FAILED || error != NULL)
    probe->failed = true;
}

static int advance_client(cnet_client *client) {
  size_t events = 0u;
  return cnet_client_advance_external(client, &events);
}

static int route_client_completion(
    cnet_client *client,
    const native_io_completion *completion,
    bool *out_consumed) {
  size_t events = 0u;
  return cnet_client_route_external_completion(
      client, completion, out_consumed, &events);
}

static int drive_shared_once(cnet_listener *listener,
                             cnet_client *server,
                             cnet_client *client,
                             native_io_backend *backend,
                             uint32_t timeout_ms,
                             bool *out_listener_consumed) {
  native_io_completion completions[TEST_BATCH];
  size_t completion_count = 0u;
  size_t index;
  int status;

  if (out_listener_consumed != NULL)
    *out_listener_consumed = false;

  status = advance_client(server);
  if (status != SALTS_OK) return status;
  status = advance_client(client);
  if (status != SALTS_OK) return status;

  status = native_io_backend_observe(
      backend, completions, TEST_BATCH, timeout_ms, &completion_count);
  if (status == SALTS_ETIMEDOUT) {
    status = advance_client(server);
    if (status != SALTS_OK) return status;
    return advance_client(client);
  }
  if (status != SALTS_OK) return status;

  for (index = 0u; index < completion_count; ++index) {
    bool consumed = false;

    if (listener != NULL && listener->impl != NULL) {
      status = cnet_listener_route_external_completion(
          listener, &completions[index], &consumed);
      if (status != SALTS_OK) return status;
    }
    if (consumed) {
      if (out_listener_consumed != NULL)
        *out_listener_consumed = true;
      continue;
    }

    status = route_client_completion(server, &completions[index], &consumed);
    if (status != SALTS_OK) return status;
    if (consumed) continue;

    status = route_client_completion(client, &completions[index], &consumed);
    if (status != SALTS_OK) return status;
    if (!consumed) return SALTS_EPROTO;
  }

  status = advance_client(server);
  if (status != SALTS_OK) return status;
  return advance_client(client);
}

static native_io_backend backend;
static cnet_client server;
static cnet_client client;
static cnet_listener listener;
static cnet_tls_server tls_server;
static cnet_tls_client tls_client;
static state_probe server_probe;
static state_probe client_probe;
static cnet_connection outbound;
static cnet_connection accepted;

static void test_external_tls_listener_shared_progress(void) {
  enum { LISTENER_BUFFER_BYTES = 16384, CLIENT_BUFFER_BYTES = 4096 };
  static const char *alpn[] = {"http/1.1"};
  native_io_backend_config backend_config = {
      test_backend_kind(), 32u, 64u, TEST_BATCH};
  cnet_client_config server_config = test_client_config();
  cnet_client_config client_config = test_client_config();
  cnet_stream_socket_options server_socket_options = CNET_STREAM_SOCKET_OPTIONS_INIT;
  cnet_stream_endpoint bind = CNET_STREAM_ENDPOINT_INIT;
  cnet_stream_endpoint local = CNET_STREAM_ENDPOINT_INIT;
  cnet_tls_server_config server_tls_config = {
      .size = sizeof(server_tls_config),
      .cert_file = CNET_EXTERNAL_TLS_CERT,
      .key_file = CNET_EXTERNAL_TLS_KEY,
      .key_password = CNET_TLS_TEST_SERVER_PASSWORD,
      .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
      .alpn_protocols = alpn,
      .alpn_protocol_count = 1u,
      .security_profile = CNET_TLS_TEST_SECURITY_PROFILE};
  cnet_tls_client_config client_tls_config = {
      .size = sizeof(client_tls_config),
      .ca_file = CNET_EXTERNAL_TLS_CA,
      .server_name = CNET_TLS_TEST_IDENTITY,
      .alpn_protocols = alpn,
      .alpn_protocol_count = 1u,
      .security_profile = CNET_TLS_TEST_SECURITY_PROFILE};
  cnet_connect_options connect_options;
  cnet_observer server_observer = {0};
  native_io_request first = {0};
  native_io_request duplicate = {0};
  bool listener_consumed = false;
  bool accept_completion_seen = false;
  bool accepted_started = false;
  uint64_t deadline;
  uint64_t listener_receive_buffer = 0u;
  uint64_t listener_send_buffer = 0u;
  char uri[64];
#if CNET_SHANGMI_TLS13_TEST
  /* Each negative test must reach the same admitted TCP/TLS connection.
   * Only peer trust, peer DNS identity, or client security profile differs.
   */
  const char *failure_case = getenv("SALTS_SHANGMI_FAILURE_CASE");
  bool expect_failure = failure_case != NULL && failure_case[0] != '\0';
  bool require_mtls = getenv("SALTS_SHANGMI_MTLS") != NULL;
  bool reject_client_certificate = false;
  if (expect_failure) {
    if (strcmp(failure_case, "wrong-ca") == 0) {
      client_tls_config.ca_file = getenv("SALTS_SHANGMI_UNTRUSTED_CA");
      check(client_tls_config.ca_file != NULL);
    } else if (strcmp(failure_case, "wrong-san") == 0) {
      client_tls_config.server_name = "wrong.example";
    } else if (strcmp(failure_case, "standard-peer") == 0) {
      client_tls_config.security_profile = CNET_TLS_SECURITY_PROFILE_STANDARD;
    } else if (strcmp(failure_case, "missing-client-cert") == 0) {
      check(require_mtls);
      reject_client_certificate = true;
    } else if (strcmp(failure_case, "untrusted-client-ca") == 0) {
      check(require_mtls);
      reject_client_certificate = true;
    } else {
      check(0); /* Unknown scenario is a failing test, never a skip. */
    }
  }
  if (require_mtls) {
    server_tls_config.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
    server_tls_config.ca_file = CNET_EXTERNAL_TLS_CA;
    if (!expect_failure || strcmp(failure_case, "missing-client-cert") != 0) {
      client_tls_config.cert_file = getenv("SALTS_SHANGMI_CLIENT_CERT");
      client_tls_config.key_file = getenv("SALTS_SHANGMI_CLIENT_KEY");
      client_tls_config.key_password = CNET_TLS_TEST_SERVER_PASSWORD;
      if (expect_failure && strcmp(failure_case, "untrusted-client-ca") == 0) {
        client_tls_config.cert_file = getenv("SALTS_SHANGMI_UNTRUSTED_CLIENT_CERT");
        client_tls_config.key_file = getenv("SALTS_SHANGMI_UNTRUSTED_CLIENT_KEY");
      }
      check(client_tls_config.cert_file != NULL);
      check(client_tls_config.key_file != NULL);
    }
  }
#endif

  check(native_io_backend_init(&backend, &backend_config) == SALTS_OK);
  check(cnet_client_init_external(
             &server, &server_config, &backend) == SALTS_OK);
  check(cnet_client_init_external(
             &client, &client_config, &backend) == SALTS_OK);
  server_socket_options.receive_buffer_bytes = CLIENT_BUFFER_BYTES;
  server_socket_options.send_buffer_bytes = CLIENT_BUFFER_BYTES;
  check_equal(cnet_client_set_stream_socket_options(&server, &server_socket_options),
              SALTS_OK);

  check(cnet_tls_server_init(
             &tls_server, &server_tls_config) == SALTS_OK);
  check(cnet_tls_client_init(
             &tls_client, &client_tls_config) == SALTS_OK);

  bind.family = CNET_DATAGRAM_ADDRESS_IPV4;
  bind.address[0] = 127u;
  bind.address[3] = 1u;
  check(cnet_listener_open(
             &listener, test_backend_kind(),
             CNET_DATAGRAM_ADDRESS_IPV4) == SALTS_OK);
  check_equal(cnet_listener_tcp_option_set(
                  &listener, CNET_TCP_SOCKET_RECEIVE_BUFFER_BYTES, LISTENER_BUFFER_BYTES),
              SALTS_OK);
  check_equal(cnet_listener_tcp_option_set(
                  &listener, CNET_TCP_SOCKET_SEND_BUFFER_BYTES, LISTENER_BUFFER_BYTES),
              SALTS_OK);
  check_equal(cnet_listener_tcp_option_get(
                  &listener, CNET_TCP_SOCKET_RECEIVE_BUFFER_BYTES, &listener_receive_buffer),
              SALTS_OK);
  check_equal(cnet_listener_tcp_option_get(
                  &listener, CNET_TCP_SOCKET_SEND_BUFFER_BYTES, &listener_send_buffer),
              SALTS_OK);
  check(cnet_listener_bind_open_endpoint(&listener, &bind) == SALTS_OK);
  check(cnet_listener_local_endpoint(&listener, &local) == SALTS_OK);
  check(local.port != 0u);
  check(cnet_listener_listen(&listener, 8u) == SALTS_OK);
  check(cnet_listener_attach_external(&listener, &backend) == SALTS_OK);

  {
    int ready = 0;
    check(cnet_listener_wait(&listener, 0u, &ready) == SALTS_ENOTSUP);
  }

  check(cnet_listener_submit_external_accept(
             &listener, &first) == SALTS_OK);
  check(native_io_request_valid(first));
  check(cnet_listener_submit_external_accept(
             &listener, &duplicate) == SALTS_OK);
  check(duplicate.slot == first.slot);
  check(duplicate.generation == first.generation);

  check(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u",
                  (unsigned int)local.port) > 0);

  client_probe.client = &client;
  connect_options = (cnet_connect_options){
      .uri = uri,
      .observer = {
          .on_state = on_state,
          .user = &client_probe},
      .tls_client = &tls_client};
  check(cnet_connect(&client, &connect_options, &outbound) == SALTS_OK);
  check(cnet_tls_client_destroy(&tls_client) == SALTS_OK);

  server_probe.client = &server;
  server_observer.on_state = on_state;
  server_observer.user = &server_probe;

  deadline = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
  while (
#if CNET_SHANGMI_TLS13_TEST
         (expect_failure
              ? (reject_client_certificate ? !server_probe.failed : !client_probe.failed)
              : ((!client_probe.connected || !server_probe.connected) &&
                 !client_probe.failed && !server_probe.failed)) &&
#else
         ((!client_probe.connected || !server_probe.connected) &&
          !client_probe.failed && !server_probe.failed) &&
#endif
         cmeta_monotonic_ms() < deadline) {
    check(drive_shared_once(
               &listener, &server, &client, &backend,
               25u, &listener_consumed) == SALTS_OK);

    if (listener_consumed) accept_completion_seen = true;

    if (accept_completion_seen && !accepted_started) {
      native_io_request blocked = {0};
      check(cnet_listener_submit_external_accept(
                 &listener, &blocked) == SALTS_EALREADY);
      check(!native_io_request_valid(blocked));
      check(cnet_listener_accept_tls(
                 &listener, &server, &tls_server,
                 &server_observer, &accepted) == SALTS_OK);
      check(accepted.slot != 0u);
      check(accepted.generation != 0u);
      accepted_started = true;
    }
  }

  check(accept_completion_seen);
  check(accepted_started);
#if CNET_SHANGMI_TLS13_TEST
  if (expect_failure) {
    /* Fail closed: TCP acceptance and TLS negotiation were attempted, but
     * the untrusted / wrong-profile client must never report CONNECTED.
     * Server-side completion may race the client's certificate rejection.
     */
    if (reject_client_certificate) {
      /* A TLS 1.3 client may send its Finished before the server rejects
       * a missing/untrusted client identity. The server must NEVER become
       * CONNECTED; require its terminal verification failure. */
      check(!server_probe.connected);
      check(server_probe.failed);
      check(server_probe.terminal);
    } else {
      check(!client_probe.connected);
      check(client_probe.failed);
      check(client_probe.terminal);
    }
    return; /* after_each owns orderly bounded cleanup of both NativeIO owners. */
  }
#endif
  check(client_probe.connected);
  check(server_probe.connected);
  check(!client_probe.failed);
  check(!server_probe.failed);

  {
    uint64_t inherited = 0u;
    check_equal(cnet_connection_tcp_option_get(
                    &server, accepted, CNET_TCP_SOCKET_RECEIVE_BUFFER_BYTES, &inherited),
                SALTS_OK);
    /* Socket getters report live kernel values: Darwin can grow an accepted
     * socket's buffers during the TLS handshake. The client policy must not
     * replace the larger listener budget with CLIENT_BUFFER_BYTES. */
    check_greater_equal(inherited, listener_receive_buffer);
    check_equal(cnet_connection_tcp_option_get(
                    &server, accepted, CNET_TCP_SOCKET_SEND_BUFFER_BYTES, &inherited),
                SALTS_OK);
    check_greater_equal(inherited, listener_send_buffer);
  }

  check(cnet_close(&client, outbound) == SALTS_OK);
  check(cnet_close(&server, accepted) == SALTS_OK);

  deadline = cmeta_monotonic_ms() + TEST_TIMEOUT_MS;
  while ((!client_probe.terminal || !server_probe.terminal) &&
         cmeta_monotonic_ms() < deadline) {
    check(drive_shared_once(
               &listener, &server, &client, &backend,
               25u, NULL) == SALTS_OK);
  }
  check(client_probe.terminal);
  check(server_probe.terminal);
  check(!client_probe.failed);
  check(!server_probe.failed);

  check(cnet_listener_close(&listener) == SALTS_OK);
  check(cnet_listener_destroy(&listener) == SALTS_OK);
  check(cnet_tls_server_destroy(&tls_server) == SALTS_OK);
  check(cnet_client_stop_external(&server) == SALTS_OK);
  check(cnet_client_destroy(&server) == SALTS_OK);
  check(cnet_client_stop_external(&client) == SALTS_OK);
  check(cnet_client_destroy(&client) == SALTS_OK);
  check(native_io_backend_close(&backend) == SALTS_OK);
  check(native_io_backend_destroy(&backend) == SALTS_OK);
}

suite("CNet external TLS listener") {
    before_each() {
        check_null(backend.impl);
        server_probe = (state_probe){0};
        client_probe = (state_probe){0};
        outbound = (cnet_connection){0};
        accepted = (cnet_connection){0};
    }
    after_each() {
        external_test_cleanup cleanup = {&backend, &listener, {{&server, accepted}, {&client, outbound}}};
        cleanup_external_test(&cleanup);
        if (tls_client.impl != NULL)
            check_warn(cnet_tls_client_destroy(&tls_client) == SALTS_OK);
        if (tls_server.impl != NULL)
            check_warn(cnet_tls_server_destroy(&tls_server) == SALTS_OK);
    }

    group("shared TLS progress") {
        it("external tls listener shared progress") { test_external_tls_listener_shared_progress(); }
    }
}
