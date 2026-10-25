#include "cnet_test_internal.h"
#include "cnet_tls.h"
#if defined(CNET_INTERNAL_PROFILING)
  #include "cnet_client_internal.h"
#endif
#include "tinytest.h"

#include <salts/clock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cnet_tls_test_send_bytes(cnet_client *client, cnet_connection connection,
                                    const void *data, size_t size) {
  mem_buffer_t *buffer;
  int status;
  if (client == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  buffer = mem_get_buffer(mem_global(), size);
  if (buffer == NULL) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  status = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

#if defined(CNET_INTERNAL_PROFILING)
static size_t cnet_tls_test_trace_index(const cnet_owner_trace_event *events, size_t count,
                                        cnet_owner_trace_kind kind) {
  for (size_t index = 0u; index < count; ++index)
    if (events[index].kind == kind) return index;
  return SIZE_MAX;
}

static uint64_t cnet_tls_test_trace_bytes(const cnet_owner_trace_event *events, size_t count,
                                          cnet_owner_trace_kind kind) {
  uint64_t total = 0u;
  for (size_t index = 0u; index < count; ++index) {
    if (events[index].kind != kind) continue;
    total = events[index].bytes > UINT64_MAX - total ? UINT64_MAX : total + events[index].bytes;
  }
  return total;
}
#endif

static const char CNET_TLS_TEST_CERTIFICATE[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC7TCCAdWgAwIBAgIUT4pOT+qAkLpsC1bUF3bYRrTHssQwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDMyMzA4MDcwMloXDTM2MDMy\n"
    "MDA4MDcwMlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEAtNuutQlZVrXBW97HX5HfMXbMkES9n2eglXQRzU7Qg4Mm\n"
    "KtAprpkBVSFHeAti0NyPgasoaoJTBi1xBhDsGTWTto0TJVHhW5QcYSPRc8x/acWQ\n"
    "NxBSMdWf8Rp9QxbaECyQbWr+QDb/c1a9QU0fGFntQBnLfk9lLJG7MRTwg38ufnSk\n"
    "OqqyAtbT4V5ZwImkOo9MdECcZMvRDYnvH1atIUvGRI7O3M466jGe+5WN4E42h8VN\n"
    "PSJw2IBvbFxePZ3yMWpiVRkbsWlq1hJIHGvnGD+4IPGr2nB/FmR+P969KFm/gSvG\n"
    "L9tYFRw36Cfa+cnwWAYNpLspwOaaAQcpeMN8tAGIPwIDAQABozcwNTAUBgNVHREE\n"
    "DTALgglsb2NhbGhvc3QwHQYDVR0OBBYEFHSKGrYW6d59EU5htbnpgVhPLaQiMA0G\n"
    "CSqGSIb3DQEBCwUAA4IBAQBhIzu8IJ7Pm30nKOfvwgQRKbJDWIBKZz/NYoIP5Ljm\n"
    "fZG+ZZT0BnuCObKTvPwAWERwbIn5cIDNCkVKhQoJc4+KqR9fXptxML+Q3e4lCVo3\n"
    "5jjQpG/r18aZxhfroinp6iCfGcECw/JAXPxC8jOhEgVOPQd/LybM9vO8vraH/dIR\n"
    "YRmIoBvGw+wQMt/PcV0GxYLo6LsYJFs0FuJyiufJ2auNtmW5h8qOdtnagmeo0ehp\n"
    "g5VqPlB3EMa/01r9WmfNQJcBbEF8ONhhPXZCV4uplsXGtN8+Xxrzb3SAYQR9xFry\n"
    "x9YTzT8UMLc26vY1RiF6uwODUJzmSaqmefmapVsWrgi3\n"
    "-----END CERTIFICATE-----\n";

static int cnet_tls_test_read_fixture(const char *path, uint8_t *buffer, size_t capacity,
                                      size_t *out_size) {
  FILE *file;
  size_t size;
  int trailing;
  if (path == NULL || buffer == NULL || capacity == 0u || out_size == NULL) return SALTS_EINVAL;
  *out_size = 0u;
  file = fopen(path, "rb");
  if (file == NULL) return SALTS_EIO;
  size = fread(buffer, 1u, capacity, file);
  trailing = fgetc(file);
  if (ferror(file) || trailing != EOF) {
    (void)fclose(file);
    return SALTS_EMSGSIZE;
  }
  (void)fclose(file);
  if (size == 0u) return SALTS_EIO;
  *out_size = size;
  return SALTS_OK;
}

typedef struct cnet_tls_test_pair {
  cnet_tls_server server_context;
  cnet_tls_state client;
  cnet_tls_state server;
  const char *ca_path;
  const char *cert_path;
  const char *key_path;
} cnet_tls_test_pair;

static int cnet_tls_test_transfer(cnet_tls_state *source, cnet_tls_state *target) {
  unsigned char buffer[1024];
  for (;;) {
    size_t size = 0u;
    int status = cnet_tls_take_cipher(source, buffer, sizeof(buffer), &size);
    if (status == SALTS_ENOENT) return SALTS_OK;
    if (status != SALTS_OK) return status;
    if (size == 0u) return SALTS_EPROTO;
    status = cnet_tls_feed_cipher(target, buffer, size);
    if (status != SALTS_OK) return status;
  }
}

static int cnet_tls_test_pair_init(cnet_tls_test_pair *pair) {
  static const char *server_alpn[] = {"h2", "http/1.1"};
  static const char *client_alpn[] = {"http/1.1", "h2"};
  cnet_tls_server_config server_config;
  cnet_tls_client_config client_config;
  cnet_tls_context *client_context = NULL;
  cnet_tls_context *server_context;
  int status;

  memset(pair, 0, sizeof(*pair));
  pair->ca_path = CNET_TLS_TEST_IP_CA;
  pair->cert_path = CNET_TLS_TEST_IP_CERT;
  pair->key_path = CNET_TLS_TEST_IP_KEY;

  server_config = (cnet_tls_server_config){.size = sizeof(server_config),
                                           .cert_file = pair->cert_path,
                                           .key_file = pair->key_path,
                                           .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                           .alpn_protocols = server_alpn,
                                           .alpn_protocol_count = 2u};
  status = cnet_tls_server_init(&pair->server_context, &server_config);
  if (status != SALTS_OK) return status;
  client_config = (cnet_tls_client_config){.size = sizeof(client_config),
                                           .ca_file = pair->ca_path,
                                           .alpn_protocols = client_alpn,
                                           .alpn_protocol_count = 2u};
  status = cnet_tls_client_context_create(&client_config, &client_context);
  if (status != SALTS_OK) return status;
  status = cnet_tls_state_init(&pair->client, client_context, false, "localhost",
                               CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) {
    cnet_tls_context_release(client_context);
    return status;
  }
  server_context = cnet_tls_server_context(&pair->server_context);
  cnet_tls_context_retain(server_context);
  status =
      cnet_tls_state_init(&pair->server, server_context, true, NULL, CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) cnet_tls_context_release(server_context);
  return status;
}

static int cnet_tls_test_fixture_pair_init(cnet_tls_test_pair *pair, const char *ca_file,
                                           const char *cert_file, const char *key_file,
                                           const char *server_name) {
  cnet_tls_server_config server_config;
  cnet_tls_client_config client_config;
  cnet_tls_context *client_context = NULL;
  cnet_tls_context *server_context;
  int status;

  if (pair == NULL || ca_file == NULL || cert_file == NULL || key_file == NULL ||
      server_name == NULL)
    return SALTS_EINVAL;
  memset(pair, 0, sizeof(*pair));
  pair->ca_path = ca_file;
  pair->cert_path = cert_file;
  pair->key_path = key_file;

  server_config = (cnet_tls_server_config){.size = sizeof(server_config),
                                           .cert_file = cert_file,
                                           .key_file = key_file,
                                           .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
  status = cnet_tls_server_init(&pair->server_context, &server_config);
  if (status != SALTS_OK) return status;

  client_config = (cnet_tls_client_config){.size = sizeof(client_config), .ca_file = ca_file};
  status = cnet_tls_client_context_create(&client_config, &client_context);
  if (status != SALTS_OK) {
    (void)cnet_tls_server_destroy(&pair->server_context);
    return status;
  }
  status = cnet_tls_state_init(&pair->client, client_context, false, server_name,
                               CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) {
    cnet_tls_context_release(client_context);
    (void)cnet_tls_server_destroy(&pair->server_context);
    return status;
  }

  server_context = cnet_tls_server_context(&pair->server_context);
  cnet_tls_context_retain(server_context);
  status =
      cnet_tls_state_init(&pair->server, server_context, true, NULL, CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) {
    cnet_tls_context_release(server_context);
    cnet_tls_state_destroy(&pair->client);
    (void)cnet_tls_server_destroy(&pair->server_context);
  }
  return status;
}

static void cnet_tls_test_pair_destroy(cnet_tls_test_pair *pair) {
  cnet_tls_state_destroy(&pair->server);
  cnet_tls_state_destroy(&pair->client);
  (void)cnet_tls_server_destroy(&pair->server_context);
  memset(pair, 0, sizeof(*pair));
}

static int cnet_tls_test_handshake(cnet_tls_test_pair *pair) {
  size_t iteration;
  for (iteration = 0u; iteration < 256u; ++iteration) {
    bool client_complete = false;
    bool server_complete = false;
    int status = cnet_tls_handshake(&pair->client, &client_complete);
    if (status != SALTS_OK) return status;
    status = cnet_tls_test_transfer(&pair->client, &pair->server);
    if (status != SALTS_OK) return status;
    status = cnet_tls_handshake(&pair->server, &server_complete);
    if (status != SALTS_OK) return status;
    status = cnet_tls_test_transfer(&pair->server, &pair->client);
    if (status != SALTS_OK) return status;
    if (client_complete && server_complete) return SALTS_OK;
  }
  return SALTS_ETIMEDOUT;
}

static int cnet_tls_test_transfer_available(cnet_tls_state *source, cnet_tls_state *target,
                                            size_t *out_transferred) {
  unsigned char buffer[1024];
  size_t transferred = 0u;
  if (source == NULL || target == NULL || out_transferred == NULL) return SALTS_EINVAL;
  *out_transferred = 0u;
  for (;;) {
    size_t capacity = cnet_tls_cipher_input_capacity(target);
    size_t size = 0u;
    int status;
    if (capacity == 0u) break;
    if (capacity > sizeof(buffer)) capacity = sizeof(buffer);
    status = cnet_tls_take_cipher(source, buffer, capacity, &size);
    if (status == SALTS_ENOENT) break;
    if (status != SALTS_OK) return status;
    if (size == 0u || size > capacity) return SALTS_EPROTO;
    status = cnet_tls_feed_cipher(target, buffer, size);
    if (status != SALTS_OK) return status;
    transferred += size;
  }
  *out_transferred = transferred;
  return SALTS_OK;
}

static int cnet_tls_test_round_trip(cnet_tls_state *source, cnet_tls_state *target,
                                    const unsigned char *payload, size_t payload_size) {
  unsigned char *received = NULL;
  size_t received_size = 0u;
  bool write_complete = false;
  size_t iteration;
  int status = SALTS_OK;

  if (source == NULL || target == NULL || payload == NULL || payload_size == 0u)
    return SALTS_EINVAL;
  received = (unsigned char *)malloc(payload_size);
  if (received == NULL) return SALTS_ENOMEM;

  for (iteration = 0u; iteration < 65536u; ++iteration) {
    bool progressed = false;

    while (received_size < payload_size) {
      size_t size = 0u;
      bool peer_closed = false;
      status = cnet_tls_read(target, received + received_size, payload_size - received_size, &size,
                             &peer_closed);
      if (status != SALTS_OK) goto cleanup;
      if (peer_closed) {
        status = SALTS_ECONNABORTED;
        goto cleanup;
      }
      if (size == 0u) break;
      received_size += size;
      progressed = true;
    }

    if (!write_complete) {
      status = cnet_tls_write(source, payload, payload_size, &write_complete);
      if (status != SALTS_OK) goto cleanup;
    }

    {
      size_t transferred = 0u;
      status = cnet_tls_test_transfer_available(source, target, &transferred);
      if (status != SALTS_OK) goto cleanup;
      if (transferred != 0u) progressed = true;
    }

    if (write_complete && received_size == payload_size) {
      status = memcmp(received, payload, payload_size) == 0 ? SALTS_OK : SALTS_EPROTO;
      goto cleanup;
    }
    if (!progressed && !write_complete) {
      status = SALTS_EPROTO;
      goto cleanup;
    }
  }
  status = SALTS_ETIMEDOUT;

cleanup:
  free(received);
  return status;
}

static int cnet_tls_test_reset_client(cnet_tls_test_pair *pair,
                                      const cnet_tls_client_config *config,
                                      const char *server_name) {
  cnet_tls_context *context = NULL;
  int status;
  cnet_tls_state_destroy(&pair->client);
  status = cnet_tls_client_context_create(config, &context);
  if (status != SALTS_OK) return status;
  status =
      cnet_tls_state_init(&pair->client, context, false, server_name, CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status != SALTS_OK) cnet_tls_context_release(context);
  return status;
}

static int cnet_tls_test_mtls_pair_init(cnet_tls_test_pair *pair, cnet_tls_client_auth client_auth,
                                        bool client_identity) {
  cnet_tls_server_config server_config;
  cnet_tls_client_config client_config;
  cnet_tls_context *server_context;
  int status = cnet_tls_test_pair_init(pair);
  if (status != SALTS_OK) return status;
  cnet_tls_state_destroy(&pair->client);
  cnet_tls_state_destroy(&pair->server);
  status = cnet_tls_server_destroy(&pair->server_context);
  if (status != SALTS_OK) goto cleanup;
  server_config = (cnet_tls_server_config){.size = sizeof(server_config),
                                           .cert_file = pair->cert_path,
                                           .key_file = pair->key_path,
                                           .ca_file = pair->ca_path,
                                           .client_auth = client_auth};
  status = cnet_tls_server_init(&pair->server_context, &server_config);
  if (status != SALTS_OK) goto cleanup;
  client_config = (cnet_tls_client_config){.size = sizeof(client_config),
                                           .ca_file = pair->ca_path,
                                           .cert_file = client_identity ? pair->cert_path : NULL,
                                           .key_file = client_identity ? pair->key_path : NULL};
  status = cnet_tls_test_reset_client(pair, &client_config, "localhost");
  if (status != SALTS_OK) goto cleanup;
  server_context = cnet_tls_server_context(&pair->server_context);
  cnet_tls_context_retain(server_context);
  status =
      cnet_tls_state_init(&pair->server, server_context, true, NULL, CNET_TLS_MIN_IO_BUFFER_BYTES);
  if (status == SALTS_OK) return SALTS_OK;
  cnet_tls_context_release(server_context);
cleanup:
  cnet_tls_test_pair_destroy(pair);
  return status;
}

typedef struct cnet_tls_network_probe {
  cnet_client *client;
  cnet_connection connection;
  char received[16];
  unsigned char *dynamic_received;
  size_t dynamic_capacity;
  char alpn[16];
  char tls_version[16];
  char tls_cipher[128];
  size_t received_size;
  size_t alpn_size;
  size_t tls_version_size;
  size_t tls_cipher_size;
  int alpn_status;
  int tls_version_status;
  int tls_cipher_status;
  int connected;
  int connected_count;
  int handshaking;
  int sent;
  int terminal;
  int failed;
  int failure_status;
  const char *failure_stage;
  mem_slice_t owned_slice;
  cnet_message_kind owned_kind;
  size_t owned_received_size;
} cnet_tls_network_probe;

static void cnet_tls_network_state(void *user, cnet_connection connection,
                                   cnet_connection_state state, const cnet_error *error) {
  cnet_tls_network_probe *probe = (cnet_tls_network_probe *)user;
  probe->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    probe->connected = 1;
    ++probe->connected_count;
    probe->alpn_status = cnet_tls_negotiated_alpn(probe->client, connection, probe->alpn,
                                                  sizeof(probe->alpn), &probe->alpn_size);
    probe->tls_version_status =
        cnet_tls_negotiated_version(probe->client, connection, probe->tls_version,
                                    sizeof(probe->tls_version), &probe->tls_version_size);
    probe->tls_cipher_status =
        cnet_tls_negotiated_cipher(probe->client, connection, probe->tls_cipher,
                                   sizeof(probe->tls_cipher), &probe->tls_cipher_size);
  } else if (state == CNET_CONNECTION_TLS_HANDSHAKING) {
    ++probe->handshaking;
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    probe->terminal = 1;
    if (state == CNET_CONNECTION_FAILED || error != NULL) {
      probe->failed = 1;
      if (error != NULL) {
        probe->failure_status = error->status;
        probe->failure_stage = error->stage;
      }
    }
  }
}

static void cnet_tls_network_receive(void *user, cnet_connection connection,
                                     const cnet_receive_view *view) {
  cnet_tls_network_probe *probe = (cnet_tls_network_probe *)user;
  (void)connection;
  if (view == NULL || view->kind != CNET_MESSAGE_BYTES) {
    probe->failed = 1;
    return;
  }
  if (probe->dynamic_received != NULL) {
    if (probe->received_size > probe->dynamic_capacity ||
        view->size > probe->dynamic_capacity - probe->received_size) {
      probe->failed = 1;
      return;
    }
    memcpy(probe->dynamic_received + probe->received_size, view->data, view->size);
  } else {
    if (view->size > sizeof(probe->received) - probe->received_size) {
      probe->failed = 1;
      return;
    }
    memcpy(probe->received + probe->received_size, view->data, view->size);
  }
  probe->received_size += view->size;
}

static void cnet_tls_network_receive_owned(void *user, cnet_connection connection,
                                           mem_slice_t slice, cnet_message_kind kind) {
  cnet_tls_network_probe *probe = (cnet_tls_network_probe *)user;
  (void)connection;
  if (kind != CNET_MESSAGE_BYTES || slice.buffer == NULL || slice.data == NULL ||
      slice.length == 0u || probe->owned_slice.buffer != NULL) {
    probe->failed = 1;
    mem_slice_release(&slice);
    return;
  }
  probe->owned_slice = slice;
  probe->owned_kind = kind;
  probe->owned_received_size = slice.length;
}

static void cnet_tls_network_send(void *user, cnet_connection connection, size_t size) {
  cnet_tls_network_probe *probe = (cnet_tls_network_probe *)user;
  (void)connection;
  if (size == 0u) probe->failed = 1;
  ++probe->sent;
}

static cnet_client_config cnet_tls_network_config(void) {
  const cnet_client_config config = {.backend =
#if defined(_WIN32)
                                         NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
                                         NATIVE_IO_BACKEND_EPOLL,
#else
                                         NATIVE_IO_BACKEND_KQUEUE,
#endif
                                     .connection_capacity = 2u,
                                     .command_capacity = 16u,
                                     .request_capacity = 8u,
                                     .completion_batch_capacity = 8u,
                                     .event_capacity = 16u,
                                     .max_send_bytes = 1024u,
                                     .receive_buffer_bytes = 1024u,
                                     .connect_timeout_ms = 2000u,
                                     .read_timeout_ms = 2000u,
                                     .write_timeout_ms = 2000u,
                                     .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                     .tls_handshake_timeout_ms = 2000u};
  return config;
}

static cnet_client_config cnet_tls_large_network_config(void) {
  cnet_client_config config = cnet_tls_network_config();
  config.command_capacity = 32u;
  config.request_capacity = 32u;
  config.completion_batch_capacity = 32u;
  config.event_capacity = 64u;
  config.max_send_bytes = 256u * 1024u;
  config.receive_buffer_bytes = 32u * 1024u;
  config.read_timeout_ms = 5000u;
  config.write_timeout_ms = 5000u;
  return config;
}

static int cnet_tls_network_drive(cnet_client *client, cnet_client *server, cnet_listener *listener,
                                  cnet_tls_server *tls_server, cnet_tls_network_probe *server_probe,
                                  bool *accepted) {
  size_t events = 0u;
  int ready = 0;
  int status = cnet_client_poll(client, 1u, &events);
  if (status != SALTS_OK) return status;
  if (!*accepted) {
    status = cnet_listener_wait(listener, 0u, &ready);
    if (status != SALTS_OK) return status;
    if (ready != 0) {
      const cnet_observer observer = {.on_state = cnet_tls_network_state,
                                      .on_receive = cnet_tls_network_receive,
                                      .user = server_probe,
                                      .on_send = cnet_tls_network_send};
      status = cnet_listener_accept_tls(listener, server, tls_server, &observer,
                                        &server_probe->connection);
      if (status != SALTS_OK) return status;
      *accepted = true;
    }
  }
  return cnet_client_poll(server, 1u, &events);
}

spec("CNet bounded TLS engine") {

  group("plaintext and transport receive ownership") {
    static cnet_tls_test_pair pair;
    const cnet_tls_protocol_version versions[] = {
        CNET_TLS_PROTOCOL_VERSION_1_2, CNET_TLS_PROTOCOL_VERSION_1_3};

    before_each() {
      memset(&pair, 0, sizeof(pair));
      check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    }

    after_each() { cnet_tls_test_pair_destroy(&pair); }

    for (size_t version = 0u; version < sizeof(versions) / sizeof(versions[0]); ++version) {
      for (int probe = 0; probe <= 1; ++probe) {
        it("keeps TLS %d plaintext independent of transport storage, probe=%d",
            (int)versions[version], probe) {
          enum { payload_bytes = 64, prefix_bytes = 17, transport_fill = 0x5a,
                 completion_fill = 0xa5 };
          unsigned char payload[payload_bytes];
          unsigned char received[payload_bytes] = {0};
          unsigned char transport_expected[payload_bytes];
          unsigned char *transport = cnet_tls_state_read_buffer(&pair.server);
          const size_t transport_bytes = cnet_tls_state_io_buffer_bytes(&pair.server);
          size_t size = 0u;
          bool complete = false;
          bool peer_closed = false;
          bool plaintext_pending = false;

          check_equal(cnet_tls_state_set_protocol_range(&pair.client, versions[version],
                                                         versions[version]), SALTS_OK);
          check_equal(cnet_tls_state_set_protocol_range(&pair.server, versions[version],
                                                         versions[version]), SALTS_OK);
          check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
          for (size_t i = 0u; i < sizeof(payload); ++i) payload[i] = (unsigned char)i;
          check_equal(cnet_tls_write(&pair.client, payload, sizeof(payload), &complete), SALTS_OK);
          check_true(complete);
          check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);

          /* Outstanding transport bytes may coexist with buffered TLS records.
           * Neither ordinary decryption nor a no-demand probe owns these bytes. */
          memset(transport, transport_fill, transport_bytes);
          memset(transport_expected, transport_fill, sizeof(transport_expected));
          if (probe) {
            check_equal(cnet_tls_probe_peer_close(&pair.server, &peer_closed, &plaintext_pending),
                        SALTS_OK);
            check_true(plaintext_pending);
            check_false(peer_closed);
            check_equal(transport, transport_expected, sizeof(transport_expected));
          }
          check_equal(cnet_tls_read(&pair.server, received, prefix_bytes, &size, &peer_closed),
                      SALTS_OK);
          check_equal(size, (size_t)prefix_bytes);
          check_false(peer_closed);
          check_equal(transport, transport_expected, sizeof(transport_expected));

          /* A subsequent transport completion must not overwrite an unread
           * plaintext suffix retained by a partial application read. */
          memset(transport, completion_fill, transport_bytes);
          check_equal(cnet_tls_read(&pair.server, received + prefix_bytes,
                                     sizeof(received) - prefix_bytes, &size, &peer_closed), SALTS_OK);
          check_equal(size, sizeof(received) - prefix_bytes);
          check_false(peer_closed);
          check_equal(received, payload, sizeof(payload));
        }
      }
    }
  }

  it("round-trips repeated TLS records across plaintext boundaries") {
    static const size_t sizes[] = {1024u, 16383u, 16384u, 16385u, 65535u, 65536u, 131072u};
    cnet_tls_test_pair pair;
    unsigned char *payload = NULL;
    size_t index;
    size_t byte_index;

    payload = (unsigned char *)malloc(sizes[sizeof(sizes) / sizeof(sizes[0]) - 1u]);
    check_not_null(payload);
    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);

    for (index = 0u; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
      const size_t size = sizes[index];
      for (byte_index = 0u; byte_index < size; ++byte_index)
        payload[byte_index] = (unsigned char)((byte_index * 131u + index * 17u) & 0xffu);
      check_equal(cnet_tls_test_round_trip(&pair.client, &pair.server, payload, size), SALTS_OK);
      check_equal(cnet_tls_test_round_trip(&pair.server, &pair.client, payload, size), SALTS_OK);
    }

    cnet_tls_test_pair_destroy(&pair);
    free(payload);
  }

  it("defers provider receive while an application TLS write is pending") {
    enum { payload_size = 64 * 1024 };
    static const unsigned char control[] = {0x00u, 0x00u, 0x04u, 0x08u};
    cnet_tls_test_pair pair;
    unsigned char *payload = NULL;
    unsigned char *received = NULL;
    unsigned char control_received[sizeof(control)] = {0};
    size_t received_size = 0u;
    size_t control_size = 0u;
    bool payload_complete = false;
    bool control_complete = false;
    bool peer_closed = false;
    size_t transferred = 0u;
    size_t iteration;

    payload = (unsigned char *)malloc(payload_size);
    received = (unsigned char *)malloc(payload_size);
    check_not_null(payload);
    check_not_null(received);
    for (iteration = 0u; iteration < payload_size; ++iteration)
      payload[iteration] = (unsigned char)((iteration * 43u + 13u) & 0xffu);

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);

    /*
     * A 64 KiB plaintext write cannot fit all resulting ciphertext in the
     * bounded output ring at once, so the provider must retain a pending
     * application record and report an incomplete logical write.
     */
    check_equal(cnet_tls_write(&pair.server, payload, payload_size, &payload_complete), SALTS_OK);
    check_false(payload_complete);

    check_equal(cnet_tls_write(&pair.client, control, sizeof(control), &control_complete),
                SALTS_OK);
    check_true(control_complete);
    check_equal(cnet_tls_test_transfer_available(&pair.client, &pair.server, &transferred),
                SALTS_OK);
    check_greater(transferred, (size_t)0u);

    /*
     * The incoming control record is complete, but entering GmSSL recv here
     * would overwrite TLS_CONNECT.record while tls_send still owns a pending
     * record. CNet must leave the ciphertext buffered until the application
     * write has been fully accepted.
     */
    check_equal(cnet_tls_read(&pair.server, control_received, sizeof(control_received),
                              &control_size, &peer_closed),
                SALTS_OK);
    check_equal(control_size, (size_t)0u);
    check_false(peer_closed);

    for (iteration = 0u; iteration < 65536u; ++iteration) {
      bool progressed = false;

      check_equal(cnet_tls_test_transfer_available(&pair.server, &pair.client, &transferred),
                  SALTS_OK);
      if (transferred != 0u) progressed = true;

      while (received_size < payload_size) {
        size_t size = 0u;
        check_equal(cnet_tls_read(&pair.client, received + received_size,
                                  payload_size - received_size, &size, &peer_closed),
                    SALTS_OK);
        check_false(peer_closed);
        if (size == 0u) break;
        received_size += size;
        progressed = true;
      }

      if (!payload_complete) {
        check_equal(cnet_tls_write(&pair.server, payload, payload_size, &payload_complete),
                    SALTS_OK);
      }

      if (payload_complete && received_size == payload_size) break;
      if (!progressed && !payload_complete) {
        check_true(false);
        break;
      }
    }

    check_true(payload_complete);
    check_equal(received_size, (size_t)payload_size);
    check_equal(memcmp(received, payload, payload_size), 0);

    control_size = 0u;
    peer_closed = false;
    check_equal(cnet_tls_read(&pair.server, control_received, sizeof(control_received),
                              &control_size, &peer_closed),
                SALTS_OK);
    check_false(peer_closed);
    check_equal(control_size, sizeof(control));
    check_equal(memcmp(control_received, control, sizeof(control)), 0);

    cnet_tls_test_pair_destroy(&pair);
    free(received);
    free(payload);
  }

  it("preserves large retained slicev bytes through the public TLS owner path") {
    static const char *server_alpn[] = {"h2"};
    static const char *client_alpn[] = {"h2"};
    enum {
      frame_prefix_bytes = 9,
      first_payload_bytes = 16383,
      frame_payload_bytes = 16384,
      frame_count = 4,
      second_total_bytes = frame_count * (frame_prefix_bytes + frame_payload_bytes)
    };
    cnet_client client = {0};
    cnet_client server = {0};
    cnet_listener listener = {0};
    cnet_tls_client tls_client = {0};
    cnet_tls_server tls_server = {0};
    cnet_client_config client_config = cnet_tls_large_network_config();
    cnet_client_config server_config = cnet_tls_large_network_config();
    cnet_listener_config listener_config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_tls_server_config tls_server_config = {.size = sizeof(tls_server_config),
                                                .cert_file = CNET_TLS_TEST_IP_CERT,
                                                .key_file = CNET_TLS_TEST_IP_KEY,
                                                .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                                .alpn_protocols = server_alpn,
                                                .alpn_protocol_count = 1u};
    cnet_tls_client_config tls_client_config = {.size = sizeof(tls_client_config),
                                                .ca_file = CNET_TLS_TEST_IP_CA,
                                                .server_name = "localhost",
                                                .alpn_protocols = client_alpn,
                                                .alpn_protocol_count = 1u};
    cnet_tls_network_probe client_probe = {.client = &client};
    cnet_tls_network_probe server_probe = {.client = &server};
    cnet_connect_options connect_options;
    cnet_connection client_connection = {0};
    unsigned char *received = NULL;
    unsigned char *expected = NULL;
    char uri[64];
    uint16_t port = 0u;
    uint64_t deadline;
    bool accepted = false;
    size_t sent_before;
    size_t index;
#if defined(CNET_INTERNAL_PROFILING)
    cnet_owner_trace_event client_trace[64] = {{0}};
    cnet_owner_trace_event server_trace[64] = {{0}};
    cnet_client_poll_profile client_profile = {0};
    cnet_client_poll_profile server_profile = {0};
#endif

    received = (unsigned char *)calloc(second_total_bytes, sizeof(*received));
    expected = (unsigned char *)calloc(second_total_bytes, sizeof(*expected));
    check_not_null(received);
    check_not_null(expected);
    server_probe.dynamic_received = received;
    server_probe.dynamic_capacity = second_total_bytes;

    check_equal(cnet_tls_server_init(&tls_server, &tls_server_config), SALTS_OK);
    check_equal(cnet_tls_client_init(&tls_client, &tls_client_config), SALTS_OK);
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    check_equal(cnet_client_init(&server, &server_config), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    connect_options = (cnet_connect_options){.uri = uri,
                                             .observer = {.on_state = cnet_tls_network_state,
                                                          .on_receive = cnet_tls_network_receive,
                                                          .user = &client_probe,
                                                          .on_send = cnet_tls_network_send},
                                             .tls_client = &tls_client};
    check_equal(cnet_connect(&client, &connect_options, &client_connection), SALTS_OK);
    check_equal(cnet_tls_client_destroy(&tls_client), SALTS_OK);

    deadline = cmeta_monotonic_ms() + 5000u;
    while ((!client_probe.connected || !server_probe.connected) && cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_true(accepted);
    check_true(client_probe.connected);
    check_true(server_probe.connected);
    check_false(client_probe.failed);
    check_false(server_probe.failed);

    {
      mem_buffer_t *prefix = mem_get_buffer(mem_global(), frame_prefix_bytes);
      mem_buffer_t *payload = mem_get_buffer(mem_global(), first_payload_bytes);
      mem_slice_t slices[2] = {{0}};
      unsigned char *prefix_data;
      unsigned char *payload_data;

      check_not_null(prefix);
      check_not_null(payload);
      prefix_data = (unsigned char *)mem_buffer_data(prefix);
      payload_data = (unsigned char *)mem_buffer_data(payload);
      prefix_data[0] = 0u;
      prefix_data[1] = 0x3fu;
      prefix_data[2] = 0xffu;
      prefix_data[3] = 0u;
      prefix_data[4] = 0u;
      prefix_data[5] = 0u;
      prefix_data[6] = 0u;
      prefix_data[7] = 0u;
      prefix_data[8] = 1u;
      for (index = 0u; index < first_payload_bytes; ++index)
        payload_data[index] = (unsigned char)((index * 29u + 7u) & 0xffu);
      mem_set_used(prefix, frame_prefix_bytes);
      mem_set_used(payload, first_payload_bytes);
      memcpy(expected, prefix_data, frame_prefix_bytes);
      memcpy(expected + frame_prefix_bytes, payload_data, first_payload_bytes);
      slices[0] = mem_slice(prefix, 0u, frame_prefix_bytes);
      slices[1] = mem_slice(payload, 0u, first_payload_bytes);
      check_not_null(slices[0].buffer);
      check_not_null(slices[1].buffer);

      check_equal(cnet_receive(&server, server_probe.connection, 8u), SALTS_OK);
      sent_before = (size_t)client_probe.sent;
      check_equal(cnet_send_slicev(&client, client_connection, slices, 2u), SALTS_OK);
      mem_slice_release(&slices[0]);
      mem_slice_release(&slices[1]);
      mem_buffer_release(prefix);
      mem_buffer_release(payload);

      deadline = cmeta_monotonic_ms() + 5000u;
      while ((server_probe.received_size < frame_prefix_bytes + first_payload_bytes ||
              (size_t)client_probe.sent < sent_before + 1u) &&
             !server_probe.failed && !client_probe.failed && cmeta_monotonic_ms() < deadline)
        check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                           &accepted),
                    SALTS_OK);
      check_false(client_probe.failed);
      check_false(server_probe.failed);
      check_equal(server_probe.received_size, (size_t)(frame_prefix_bytes + first_payload_bytes));
      check_equal(memcmp(received, expected, server_probe.received_size), 0);
    }

    server_probe.received_size = 0u;
    memset(received, 0, second_total_bytes);
    memset(expected, 0, second_total_bytes);
    check_equal(cnet_receive(&server, server_probe.connection, 24u), SALTS_OK);
    sent_before = (size_t)client_probe.sent;

    for (size_t frame = 0u; frame < frame_count; ++frame) {
      mem_buffer_t *prefix = mem_get_buffer(mem_global(), frame_prefix_bytes);
      mem_buffer_t *payload = mem_get_buffer(mem_global(), frame_payload_bytes);
      mem_slice_t slices[2] = {{0}};
      unsigned char *prefix_data;
      unsigned char *payload_data;
      const size_t offset = frame * (frame_prefix_bytes + frame_payload_bytes);
      const uint32_t stream_id = (uint32_t)(frame * 2u + 1u);

      check_not_null(prefix);
      check_not_null(payload);
      prefix_data = (unsigned char *)mem_buffer_data(prefix);
      payload_data = (unsigned char *)mem_buffer_data(payload);
      prefix_data[0] = 0u;
      prefix_data[1] = 0x40u;
      prefix_data[2] = 0u;
      prefix_data[3] = 0u;
      prefix_data[4] = 0u;
      prefix_data[5] = (unsigned char)((stream_id >> 24u) & 0x7fu);
      prefix_data[6] = (unsigned char)((stream_id >> 16u) & 0xffu);
      prefix_data[7] = (unsigned char)((stream_id >> 8u) & 0xffu);
      prefix_data[8] = (unsigned char)(stream_id & 0xffu);
      for (index = 0u; index < frame_payload_bytes; ++index)
        payload_data[index] = (unsigned char)((index * 31u + frame * 19u + 3u) & 0xffu);
      mem_set_used(prefix, frame_prefix_bytes);
      mem_set_used(payload, frame_payload_bytes);
      memcpy(expected + offset, prefix_data, frame_prefix_bytes);
      memcpy(expected + offset + frame_prefix_bytes, payload_data, frame_payload_bytes);
      slices[0] = mem_slice(prefix, 0u, frame_prefix_bytes);
      slices[1] = mem_slice(payload, 0u, frame_payload_bytes);
      check_not_null(slices[0].buffer);
      check_not_null(slices[1].buffer);
      check_equal(cnet_send_slicev(&client, client_connection, slices, 2u), SALTS_OK);
      mem_slice_release(&slices[0]);
      mem_slice_release(&slices[1]);
      mem_buffer_release(prefix);
      mem_buffer_release(payload);
    }

    deadline = cmeta_monotonic_ms() + 5000u;
    while ((server_probe.received_size < second_total_bytes ||
            (size_t)client_probe.sent < sent_before + frame_count) &&
           !server_probe.failed && !client_probe.failed && cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_false(client_probe.failed);
    check_false(server_probe.failed);
    check_equal(server_probe.received_size, (size_t)second_total_bytes);
    check_equal(memcmp(received, expected, second_total_bytes), 0);

    /*
     * Mirror CHTTP H2 egress: the protocol layer materializes one contiguous
     * connection-local wire buffer, then CNet retains/sends that logical
     * buffer over TLS. Validate server -> client in both one-record-ish and
     * multi-frame/multi-record shapes.
     */
    client_probe.dynamic_received = received;
    client_probe.dynamic_capacity = second_total_bytes;
    client_probe.received_size = 0u;
    memset(received, 0, second_total_bytes);
    memset(expected, 0, second_total_bytes);
    {
      const size_t wire_size = frame_prefix_bytes + first_payload_bytes;
      mem_buffer_t *wire = mem_get_buffer(mem_global(), wire_size);
      unsigned char *wire_data;
      check_not_null(wire);
      wire_data = (unsigned char *)mem_buffer_data(wire);
      wire_data[0] = 0u;
      wire_data[1] = 0x3fu;
      wire_data[2] = 0xffu;
      wire_data[3] = 0u;
      wire_data[4] = 0u;
      wire_data[5] = 0u;
      wire_data[6] = 0u;
      wire_data[7] = 0u;
      wire_data[8] = 1u;
      for (index = frame_prefix_bytes; index < wire_size; ++index)
        wire_data[index] = (unsigned char)(((index - frame_prefix_bytes) * 37u + 11u) & 0xffu);
      mem_set_used(wire, wire_size);
      memcpy(expected, wire_data, wire_size);

#if defined(CNET_INTERNAL_PROFILING)
      check_equal(cnet_client_profile_begin(&client), SALTS_OK);
      check_equal(cnet_client_profile_trace_bind(&client, client_trace,
                                                 sizeof(client_trace) / sizeof(client_trace[0])),
                  SALTS_OK);
      check_equal(cnet_client_profile_begin(&server), SALTS_OK);
      check_equal(cnet_client_profile_trace_bind(&server, server_trace,
                                                 sizeof(server_trace) / sizeof(server_trace[0])),
                  SALTS_OK);
#endif
      check_equal(cnet_receive(&client, client_connection, 8u), SALTS_OK);
      sent_before = (size_t)server_probe.sent;
      check_equal(cnet_send_buffer(&server, server_probe.connection, wire), SALTS_OK);
      mem_buffer_release(wire);

      deadline = cmeta_monotonic_ms() + 5000u;
      while ((client_probe.received_size < wire_size ||
              (size_t)server_probe.sent < sent_before + 1u) &&
             !server_probe.failed && !client_probe.failed && cmeta_monotonic_ms() < deadline)
        check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                           &accepted),
                    SALTS_OK);
      check_false(client_probe.failed);
      check_false(server_probe.failed);
      check_equal(client_probe.received_size, wire_size);
      check_equal(memcmp(received, expected, wire_size), 0);
#if defined(CNET_INTERNAL_PROFILING)
      check_equal(cnet_client_profile_take(&client, &client_profile), SALTS_OK);
      check_equal(cnet_client_profile_take(&server, &server_profile), SALTS_OK);
      check_equal(client_profile.owner.trace_dropped, UINT64_C(0));
      check_equal(server_profile.owner.trace_dropped, UINT64_C(0));
      {
        const size_t read_arm =
            cnet_tls_test_trace_index(client_trace, (size_t)client_profile.owner.trace_event_count,
                                      CNET_OWNER_TRACE_TLS_READ_ARM);
        const size_t read_completion =
            cnet_tls_test_trace_index(client_trace, (size_t)client_profile.owner.trace_event_count,
                                      CNET_OWNER_TRACE_TLS_READ_COMPLETION);
        const size_t decrypt =
            cnet_tls_test_trace_index(client_trace, (size_t)client_profile.owner.trace_event_count,
                                      CNET_OWNER_TRACE_TLS_DECRYPT);
        const size_t publish =
            cnet_tls_test_trace_index(client_trace, (size_t)client_profile.owner.trace_event_count,
                                      CNET_OWNER_TRACE_PLAINTEXT_PUBLISH);
        const size_t write_submit =
            cnet_tls_test_trace_index(server_trace, (size_t)server_profile.owner.trace_event_count,
                                      CNET_OWNER_TRACE_TLS_WRITE_SUBMIT);
        const size_t write_completion =
            cnet_tls_test_trace_index(server_trace, (size_t)server_profile.owner.trace_event_count,
                                      CNET_OWNER_TRACE_TLS_WRITE_COMPLETION);
        const size_t client_trace_count = (size_t)client_profile.owner.trace_event_count;
        const size_t server_trace_count = (size_t)server_profile.owner.trace_event_count;
        const bool client_events_present =
            read_arm < client_trace_count && read_completion < client_trace_count &&
            decrypt < client_trace_count && publish < client_trace_count;
        const bool server_events_present =
            write_submit < server_trace_count && write_completion < server_trace_count;
        check_true(client_events_present);
        check_true(server_events_present);
        if (client_events_present) {
          check_true(read_arm < read_completion);
          check_true(read_completion < decrypt);
          check_true(decrypt < publish);
          check_true(client_trace[read_completion].bytes > 0u);
        }
        if (server_events_present) check_true(write_submit < write_completion);
        check_equal(cnet_tls_test_trace_bytes(client_trace,
                                              (size_t)client_profile.owner.trace_event_count,
                                              CNET_OWNER_TRACE_TLS_DECRYPT),
                    (uint64_t)wire_size);
        check_equal(cnet_tls_test_trace_bytes(client_trace,
                                              (size_t)client_profile.owner.trace_event_count,
                                              CNET_OWNER_TRACE_PLAINTEXT_PUBLISH),
                    (uint64_t)wire_size);
      }
#endif
    }

    client_probe.received_size = 0u;
    memset(received, 0, second_total_bytes);
    memset(expected, 0, second_total_bytes);
    {
      mem_buffer_t *wire = mem_get_buffer(mem_global(), second_total_bytes);
      unsigned char *wire_data;
      check_not_null(wire);
      wire_data = (unsigned char *)mem_buffer_data(wire);
      for (size_t frame = 0u; frame < frame_count; ++frame) {
        const size_t offset = frame * (frame_prefix_bytes + frame_payload_bytes);
        const uint32_t stream_id = (uint32_t)(frame * 2u + 1u);
        wire_data[offset + 0u] = 0u;
        wire_data[offset + 1u] = 0x40u;
        wire_data[offset + 2u] = 0u;
        wire_data[offset + 3u] = 0u;
        wire_data[offset + 4u] = 0u;
        wire_data[offset + 5u] = (unsigned char)((stream_id >> 24u) & 0x7fu);
        wire_data[offset + 6u] = (unsigned char)((stream_id >> 16u) & 0xffu);
        wire_data[offset + 7u] = (unsigned char)((stream_id >> 8u) & 0xffu);
        wire_data[offset + 8u] = (unsigned char)(stream_id & 0xffu);
        for (index = 0u; index < frame_payload_bytes; ++index)
          wire_data[offset + frame_prefix_bytes + index] =
              (unsigned char)((index * 41u + frame * 23u + 5u) & 0xffu);
      }
      mem_set_used(wire, second_total_bytes);
      memcpy(expected, wire_data, second_total_bytes);

      check_equal(cnet_receive(&client, client_connection, 24u), SALTS_OK);
      sent_before = (size_t)server_probe.sent;
      check_equal(cnet_send_buffer(&server, server_probe.connection, wire), SALTS_OK);
      mem_buffer_release(wire);

      deadline = cmeta_monotonic_ms() + 5000u;
      while ((client_probe.received_size < second_total_bytes ||
              (size_t)server_probe.sent < sent_before + 1u) &&
             !server_probe.failed && !client_probe.failed && cmeta_monotonic_ms() < deadline)
        check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                           &accepted),
                    SALTS_OK);
      check_false(client_probe.failed);
      check_false(server_probe.failed);
      check_equal(client_probe.received_size, (size_t)second_total_bytes);
      check_equal(memcmp(received, expected, second_total_bytes), 0);
    }

    check_equal(cnet_close(&client, client_connection), SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((!client_probe.terminal || !server_probe.terminal) && cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_true(client_probe.terminal);
    check_true(server_probe.terminal);
    check_false(client_probe.failed);
    check_false(server_probe.failed);

    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_stop(&server, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_client_destroy(&server), SALTS_OK);
    check_equal(cnet_tls_server_destroy(&tls_server), SALTS_OK);
    free(expected);
    free(received);
  }

  it("accepts explicit CA files with trailing NUL padding") {
    char *directory = tt_make_temp_dir("cnet-ca-nul-");
    char ca_path[512];
    cnet_tls_client_config config = {.size = sizeof(config)};
    cnet_tls_context *context = NULL;

    check_not_null(directory);
    check_greater(snprintf(ca_path, sizeof(ca_path), "%s/ca.pem", directory), 0);
    check_equal(
        tt_write_file(ca_path, CNET_TLS_TEST_CERTIFICATE, sizeof(CNET_TLS_TEST_CERTIFICATE)), 0);

    config.ca_file = ca_path;
    check_equal(cnet_tls_client_context_create(&config, &context), SALTS_OK);
    check_not_null(context);
    cnet_tls_context_release(context);

    check_equal(tt_remove_tree(directory), 0);
    free(directory);
  }

  it("loads only hash.N certificates from ca_path") {
    char *directory = tt_make_temp_dir("cnet-ca-path-");
    char hashed_path[512];
    char ignored_path[512];
    cnet_tls_client_config config = {.size = sizeof(config)};
    cnet_tls_context *context = NULL;

    check_not_null(directory);
    check_greater(snprintf(hashed_path, sizeof(hashed_path), "%s/0123abcd.0", directory), 0);
    check_greater(snprintf(ignored_path, sizeof(ignored_path), "%s/not-a-hash.pem", directory), 0);
    check_equal(tt_write_file(hashed_path, CNET_TLS_TEST_CERTIFICATE,
                              sizeof(CNET_TLS_TEST_CERTIFICATE) - 1u),
                0);
    check_equal(tt_write_file(ignored_path, CNET_TLS_TEST_CERTIFICATE,
                              sizeof(CNET_TLS_TEST_CERTIFICATE) - 1u),
                0);

    config.ca_path = directory;
    check_equal(cnet_tls_client_context_create(&config, &context), SALTS_OK);
    check_not_null(context);
    cnet_tls_context_release(context);

    check_equal(tt_remove_file(hashed_path), 0);
    context = NULL;
    check_equal(cnet_tls_client_context_create(&config, &context), SALTS_EIO);
    check_null(context);

    check_equal(tt_remove_tree(directory), 0);
    free(directory);
  }

  it("preserves the full TLS ALPN wire bound instead of a provider count cap") {
    static const char *five_protocols[] = {"p1", "p2", "p3", "p4", "p5"};
    char max_name[256];
    const char *overflow_protocols[256];
    cnet_tls_client_config config = {.size = sizeof(config),
                                     .ca_file = CNET_TLS_TEST_IP_CA,
                                     .alpn_protocols = five_protocols,
                                     .alpn_protocol_count =
                                         sizeof(five_protocols) / sizeof(five_protocols[0])};
    cnet_tls_context *context = NULL;
    size_t index;

    check_equal(cnet_tls_client_context_create(&config, &context), SALTS_OK);
    check_not_null(context);
    cnet_tls_context_release(context);

    memset(max_name, 'a', sizeof(max_name) - 1u);
    max_name[sizeof(max_name) - 1u] = '\0';
    for (index = 0u; index < sizeof(overflow_protocols) / sizeof(overflow_protocols[0]); ++index)
      overflow_protocols[index] = max_name;
    config.alpn_protocols = overflow_protocols;
    config.alpn_protocol_count = sizeof(overflow_protocols) / sizeof(overflow_protocols[0]);
    context = NULL;
    check_equal(cnet_tls_client_context_create(&config, &context), SALTS_ERANGE);
    check_null(context);
  }

  it("loads an unencrypted P-256 PKCS8 identity when key_password is null") {
    cnet_tls_server server = {0};
    cnet_tls_server_config config = {.size = sizeof(config),
                                     .cert_file = CNET_TLS_TEST_P256_CERT,
                                     .key_file = CNET_TLS_TEST_P256_KEY,
                                     .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
    check_equal(cnet_tls_server_init(&server, &config), SALTS_OK);
    check_equal(cnet_tls_server_destroy(&server), SALTS_OK);
  }

  it("rejects trust bundle size arithmetic overflow before allocation") {
    size_t required = 1u;
    check_equal(cnet_test_tls_der_bundle_required_size(0u, 1u, &required), SALTS_OK);
    check_equal(required, (size_t)1u);
    check_equal(cnet_test_tls_der_bundle_required_size(1u, SIZE_MAX, &required), SALTS_ERANGE);
    check_equal(required, (size_t)0u);
    check_equal(cnet_test_tls_der_bundle_required_size(0u, SIZE_MAX, &required), SALTS_ERANGE);
    check_equal(required, (size_t)0u);
    check_equal(cnet_test_tls_der_bundle_required_size(SIZE_MAX, 1u, &required), SALTS_ERANGE);
    check_equal(required, (size_t)0u);
  }

  it("verifies an IP literal against subjectAltName iPAddress without SNI") {
    cnet_tls_test_pair pair;
    check_equal(cnet_tls_test_fixture_pair_init(&pair, CNET_TLS_TEST_IP_CA, CNET_TLS_TEST_IP_CERT,
                                                CNET_TLS_TEST_IP_KEY, "127.0.0.1"),
                SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("rejects a mismatched IP literal without falling back to DNS identity") {
    cnet_tls_test_pair pair;
    check_equal(cnet_tls_test_fixture_pair_init(&pair, CNET_TLS_TEST_IP_CA, CNET_TLS_TEST_IP_CERT,
                                                CNET_TLS_TEST_IP_KEY, "127.0.0.2"),
                SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("projects the peer certificate chain with bounded ordered views") {
    cnet_tls_test_pair pair;
    cnet_tls_peer_certificate_chain chain = {0};
    cnet_tls_peer_certificate_chain parsed = {0};
    unsigned char *duplicated = NULL;
    size_t leaf_size;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_state_peer_certificate_chain(&pair.client, &chain), SALTS_ENOTCONN);
    check_equal(chain.count, (size_t)0u);
    check_equal(chain.total_bytes, (size_t)0u);

    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(cnet_tls_state_peer_certificate_chain(&pair.client, &chain), SALTS_OK);
    check_greater(chain.count, (size_t)0u);
    check_true(chain.count <= CNET_TLS_PEER_CHAIN_MAX_CERTIFICATES);
    check_greater(chain.total_bytes, (size_t)0u);
    check_true(chain.total_bytes <= CNET_TLS_PEER_CHAIN_MAX_BYTES);
    check_not_null(chain.certificates[0].data);
    check_greater(chain.certificates[0].size, (size_t)0u);

    leaf_size = chain.certificates[0].size;
    check_true(leaf_size <= CNET_TLS_PEER_CHAIN_MAX_BYTES / 2u);
    duplicated = (unsigned char *)malloc(leaf_size * 2u);
    check_not_null(duplicated);
    memcpy(duplicated, chain.certificates[0].data, leaf_size);
    memcpy(duplicated + leaf_size, chain.certificates[0].data, leaf_size);

    check_equal(cnet_tls_peer_certificate_chain_parse(duplicated, leaf_size * 2u, &parsed),
                SALTS_OK);
    check_equal(parsed.count, (size_t)2u);
    check_equal(parsed.total_bytes, leaf_size * 2u);
    check_true(parsed.certificates[0].data == duplicated);
    check_true(parsed.certificates[1].data == duplicated + leaf_size);
    check_equal(parsed.certificates[0].size, leaf_size);
    check_equal(parsed.certificates[1].size, leaf_size);

    memset(&parsed, 0x5a, sizeof(parsed));
    check_equal(cnet_tls_peer_certificate_chain_parse(duplicated, leaf_size * 2u - 1u, &parsed),
                SALTS_EPROTO);
    check_equal(parsed.count, (size_t)0u);
    check_equal(parsed.total_bytes, (size_t)0u);
    check_null(parsed.certificates[0].data);

    memset(&parsed, 0x5a, sizeof(parsed));
    check_equal(cnet_tls_peer_certificate_chain_parse(duplicated,
                                                      CNET_TLS_PEER_CHAIN_MAX_BYTES + 1u, &parsed),
                SALTS_ERANGE);
    check_equal(parsed.count, (size_t)0u);
    check_equal(parsed.total_bytes, (size_t)0u);
    check_null(parsed.certificates[0].data);

    free(duplicated);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("reports negotiated TLS protocol and cipher only after handshake") {
    cnet_tls_test_pair pair;
    char client_version[16] = {0};
    char server_version[16] = {0};
    char client_cipher[128] = {0};
    char server_cipher[128] = {0};
    size_t client_version_size = 0u;
    size_t server_version_size = 0u;
    size_t client_cipher_size = 0u;
    size_t server_cipher_size = 0u;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_state_negotiated_version(&pair.client, client_version,
                                                  sizeof(client_version), &client_version_size),
                SALTS_ENOTCONN);
    check_equal(client_version_size, (size_t)0u);
    check_equal(cnet_tls_state_negotiated_cipher(&pair.client, client_cipher, sizeof(client_cipher),
                                                 &client_cipher_size),
                SALTS_ENOTCONN);
    check_equal(client_cipher_size, (size_t)0u);

    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(cnet_tls_state_negotiated_version(&pair.client, client_version,
                                                  sizeof(client_version), &client_version_size),
                SALTS_OK);
    check_equal(cnet_tls_state_negotiated_version(&pair.server, server_version,
                                                  sizeof(server_version), &server_version_size),
                SALTS_OK);
    check_equal(strcmp(client_version, server_version), 0);
    check_true(strcmp(client_version, "TLSv1.2") == 0 || strcmp(client_version, "TLSv1.3") == 0);
    check_equal(client_version_size, strlen(client_version));
    check_equal(server_version_size, strlen(server_version));

    check_equal(cnet_tls_state_negotiated_cipher(&pair.client, client_cipher, sizeof(client_cipher),
                                                 &client_cipher_size),
                SALTS_OK);
    check_equal(cnet_tls_state_negotiated_cipher(&pair.server, server_cipher, sizeof(server_cipher),
                                                 &server_cipher_size),
                SALTS_OK);
    check_equal(strcmp(client_cipher, server_cipher), 0);
    check_greater(client_cipher_size, (size_t)0u);
    check_equal(client_cipher_size, strlen(client_cipher));
    check_equal(server_cipher_size, strlen(server_cipher));

    check_equal(
        cnet_tls_state_negotiated_version(&pair.client, client_version, 1u, &client_version_size),
        SALTS_EMSGSIZE);
    check_equal(client_version_size, (size_t)0u);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("continues to negotiate TLS 1.2 when both peers cap at TLS 1.2") {
    cnet_tls_test_pair pair;
    char version[16] = {0};
    size_t version_size = 0u;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_state_set_protocol_range(&pair.client, CNET_TLS_PROTOCOL_VERSION_DEFAULT,
                                                  CNET_TLS_PROTOCOL_VERSION_1_2),
                SALTS_OK);
    check_equal(cnet_tls_state_set_protocol_range(&pair.server, CNET_TLS_PROTOCOL_VERSION_DEFAULT,
                                                  CNET_TLS_PROTOCOL_VERSION_1_2),
                SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(
        cnet_tls_state_negotiated_version(&pair.client, version, sizeof(version), &version_size),
        SALTS_OK);
    check_equal(strcmp(version, "TLSv1.2"), 0);
    check_equal(version_size, strlen(version));
    cnet_tls_test_pair_destroy(&pair);
  }

  it("negotiates TLS 1.3 when both peers require TLS 1.3") {
    cnet_tls_test_pair pair;
    char version[16] = {0};
    size_t version_size = 0u;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_state_set_protocol_range(&pair.client, CNET_TLS_PROTOCOL_VERSION_1_3,
                                                  CNET_TLS_PROTOCOL_VERSION_DEFAULT),
                SALTS_OK);
    check_equal(cnet_tls_state_set_protocol_range(&pair.server, CNET_TLS_PROTOCOL_VERSION_1_3,
                                                  CNET_TLS_PROTOCOL_VERSION_DEFAULT),
                SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(
        cnet_tls_state_negotiated_version(&pair.client, version, sizeof(version), &version_size),
        SALTS_OK);
    check_equal(strcmp(version, "TLSv1.3"), 0);
    check_equal(version_size, strlen(version));
    cnet_tls_test_pair_destroy(&pair);
  }

  it("carries one maximum TLS plaintext record with the public minimum IO buffer") {
    enum { payload_size = 16 * 1024 };
    cnet_tls_test_pair pair;
    unsigned char *payload = NULL;
    unsigned char *received = NULL;
    size_t received_size = 0u;
    bool complete = false;
    bool peer_closed = false;

    payload = (unsigned char *)malloc(payload_size);
    received = (unsigned char *)malloc(payload_size);
    check_not_null(payload);
    check_not_null(received);
    memset(payload, 0xa5, payload_size);
    memset(received, 0, payload_size);

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(cnet_tls_state_io_buffer_bytes(&pair.client), (size_t)CNET_TLS_MIN_IO_BUFFER_BYTES);

    check_equal(cnet_tls_write(&pair.client, payload, payload_size, &complete), SALTS_OK);
    check_true(complete);
    check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);
    check_equal(cnet_tls_read(&pair.server, received, payload_size, &received_size, &peer_closed),
                SALTS_OK);
    check_equal(received_size, (size_t)payload_size);
    check_equal(memcmp(received, payload, payload_size), 0);
    check_false(peer_closed);

    cnet_tls_test_pair_destroy(&pair);
    free(received);
    free(payload);
  }

  it("keeps GmSSL recv state clean when probing empty ciphertext before a write") {
    cnet_tls_test_pair pair;
    static const unsigned char payload = 0x6bu;
    unsigned char received = 0u;
    size_t received_size = 0u;
    bool complete = false;
    bool peer_closed = false;
    bool plaintext_pending = false;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);

    check_equal(cnet_tls_probe_peer_close(&pair.client, &peer_closed, &plaintext_pending),
                SALTS_OK);
    check_false(peer_closed);
    check_false(plaintext_pending);

    check_equal(cnet_tls_write(&pair.client, &payload, sizeof(payload), &complete), SALTS_OK);
    check_true(complete);
    check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);
    check_equal(
        cnet_tls_read(&pair.server, &received, sizeof(received), &received_size, &peer_closed),
        SALTS_OK);
    check_equal(received_size, sizeof(received));
    check_equal(received, payload);
    check_false(peer_closed);

    cnet_tls_test_pair_destroy(&pair);
  }

  it("probes peer close without consuming pending application plaintext") {
    cnet_tls_test_pair pair;
    static const unsigned char payload = 0x5au;
    unsigned char received = 0u;
    size_t received_size = 0u;
    bool complete = false;
    bool peer_closed = false;
    bool plaintext_pending = false;
    bool notify_generated = false;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);

    check_equal(cnet_tls_write(&pair.client, &payload, sizeof(payload), &complete), SALTS_OK);
    check_true(complete);
    check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);
    check_equal(cnet_tls_probe_peer_close(&pair.server, &peer_closed, &plaintext_pending),
                SALTS_OK);
    check_false(peer_closed);
    check_true(plaintext_pending);

    check_equal(
        cnet_tls_read(&pair.server, &received, sizeof(received), &received_size, &peer_closed),
        SALTS_OK);
    check_equal(received_size, sizeof(received));
    check_equal(received, payload);
    check_false(peer_closed);

    check_equal(cnet_tls_shutdown(&pair.client, &notify_generated), SALTS_OK);
    check_true(notify_generated);
    check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);
    plaintext_pending = false;
    check_equal(cnet_tls_probe_peer_close(&pair.server, &peer_closed, &plaintext_pending),
                SALTS_OK);
    check_true(peer_closed);
    check_false(plaintext_pending);

    cnet_tls_test_pair_destroy(&pair);
  }

  it("verifies localhost negotiates server-preferred ALPN and carries bytes") {
    static const char request[] = "ping";
    cnet_tls_test_pair pair;
    const unsigned char *alpn = NULL;
    unsigned char plaintext[16];
    size_t alpn_size = 0u;
    size_t plaintext_size = 0u;
    bool complete = false;
    bool peer_closed = false;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(cnet_tls_get_negotiated_alpn(&pair.client, &alpn, &alpn_size), SALTS_OK);
    check_equal(alpn_size, (size_t)2u);
    check_equal(memcmp(alpn, "h2", 2u), 0);
    check_equal(cnet_tls_get_negotiated_alpn(&pair.server, &alpn, &alpn_size), SALTS_OK);
    check_equal(alpn_size, (size_t)2u);
    check_equal(memcmp(alpn, "h2", 2u), 0);

    check_equal(cnet_tls_write(&pair.client, request, sizeof(request) - 1u, &complete), SALTS_OK);
    check_true(complete);
    check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);
    check_equal(
        cnet_tls_read(&pair.server, plaintext, sizeof(plaintext), &plaintext_size, &peer_closed),
        SALTS_OK);
    check_false(peer_closed);
    check_equal(plaintext_size, sizeof(request) - 1u);
    check_equal(memcmp(plaintext, request, sizeof(request) - 1u), 0);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("exchanges close-notify without treating clean EOF as plaintext") {
    cnet_tls_test_pair pair;
    unsigned char plaintext[16];
    size_t plaintext_size = 0u;
    bool notify_generated = false;
    bool peer_closed = false;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    check_equal(cnet_tls_shutdown(&pair.client, &notify_generated), SALTS_OK);
    check_true(notify_generated);
    check_equal(cnet_tls_test_transfer(&pair.client, &pair.server), SALTS_OK);
    check_equal(
        cnet_tls_read(&pair.server, plaintext, sizeof(plaintext), &plaintext_size, &peer_closed),
        SALTS_OK);
    check_true(peer_closed);
    check_equal(plaintext_size, (size_t)0u);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("rejects a trusted certificate whose identity does not match") {
    cnet_tls_test_pair pair;
    cnet_tls_client_config config;

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    config = (cnet_tls_client_config){.size = sizeof(config), .ca_file = pair.ca_path};
    check_equal(cnet_tls_test_reset_client(&pair, &config, "example.com"), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
    cnet_tls_test_pair_destroy(&pair);
  }

  it("rejects a self-signed certificate outside the configured trust store") {
    cnet_tls_test_pair pair;
    cnet_tls_client_config config = {.size = sizeof(config)};

    check_equal(cnet_tls_test_pair_init(&pair), SALTS_OK);
    check_equal(cnet_tls_test_reset_client(&pair, &config, "localhost"), SALTS_OK);
    check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
    cnet_tls_test_pair_destroy(&pair);
  }

  group("mutual certificate authentication") {
    static cnet_tls_test_pair pair;
    before_each() { memset(&pair, 0, sizeof(pair)); }
    after_each() { cnet_tls_test_pair_destroy(&pair); }

    it("requires and verifies a configured client certificate") {
      check_equal(cnet_tls_test_mtls_pair_init(&pair, CNET_TLS_CLIENT_AUTH_REQUIRED, true),
                  SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
    }

    it("rejects an absent client identity when client authentication is required") {
      check_equal(cnet_tls_test_mtls_pair_init(&pair, CNET_TLS_CLIENT_AUTH_REQUIRED, false),
                  SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
    }
  }

  it("derives RFC 5929 tls-server-end-point from a SHA-384 certificate signature") {
    static const uint8_t expected[] = {
        0xf9u, 0xccu, 0xacu, 0x9fu, 0xefu, 0x55u, 0x8cu, 0x77u, 0x04u, 0xd2u, 0x37u, 0x4au,
        0xa6u, 0x00u, 0xe0u, 0x95u, 0x46u, 0xc9u, 0xb3u, 0x6cu, 0xe5u, 0xd6u, 0x09u, 0xafu,
        0xcdu, 0x49u, 0xacu, 0xfbu, 0xd6u, 0x9fu, 0x37u, 0xa4u, 0x28u, 0x94u, 0x4fu, 0x2bu,
        0x63u, 0x3fu, 0x7fu, 0xdbu, 0x6bu, 0x30u, 0xecu, 0xa2u, 0xc3u, 0x01u, 0x16u, 0x06u};
    uint8_t certificate[2048] = {0};
    uint8_t binding[CNET_TLS_SERVER_END_POINT_MAX_BYTES] = {0};
    uint8_t short_binding[47] = {0};
    size_t certificate_size = 0u;
    size_t binding_size = 0u;

    check_equal(cnet_tls_test_read_fixture(CNET_TLS_TEST_SHA384_DER, certificate,
                                           sizeof(certificate), &certificate_size),
                SALTS_OK);
    check_equal(cnet_tls_server_end_point_binding_from_certificate(
                    certificate, certificate_size, binding, sizeof(binding), &binding_size),
                SALTS_OK);
    check_equal(binding_size, sizeof(expected));
    check_equal(memcmp(binding, expected, sizeof(expected)), 0);

    binding_size = 0u;
    check_equal(
        cnet_tls_server_end_point_binding_from_certificate(
            certificate, certificate_size, short_binding, sizeof(short_binding), &binding_size),
        SALTS_EMSGSIZE);
    check_equal(binding_size, sizeof(expected));
  }

  it("drives verified TLS and ALPN through the public listener and client APIs") {
    static const char request[] = "ping";
    static const char second_request[] = "more";
    static const char final_request[] = "done";
    static const char response[] = "pong";
    static const char combined_requests[] = "pingmore";
    static const char *server_alpn[] = {"h2", "http/1.1"};
    static const char *client_alpn[] = {"http/1.1", "h2"};
    mem_buffer_t *request_first = NULL;
    mem_buffer_t *request_second = NULL;
    mem_slice_t request_segments[2] = {{0}};
    cnet_client client = {0};
    cnet_client server = {0};
    cnet_listener listener = {0};
    cnet_tls_client tls_client = {0};
    cnet_tls_server tls_server = {0};
    cnet_client_config client_config = cnet_tls_network_config();
    cnet_client_config server_client_config = cnet_tls_network_config();
    cnet_listener_config listener_config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_tls_server_config tls_server_config;
    cnet_tls_client_config tls_client_config;
    cnet_tls_network_probe client_probe = {.client = &client};
    cnet_tls_network_probe server_probe = {.client = &server};
    cnet_connect_options connect_options;
    cnet_start_tls_options upgrade = CNET_START_TLS_OPTIONS_INIT;
    cnet_connection client_connection = {0};
    mem_buffer_t *final_buffer = NULL;
    static const uint8_t expected_server_end_point[] = {
        0xe5u, 0xbbu, 0xecu, 0x0eu, 0x49u, 0x9du, 0xc1u, 0x00u, 0xdbu, 0xc2u, 0x41u,
        0x4eu, 0x7au, 0x09u, 0xc6u, 0x87u, 0xe0u, 0xefu, 0xbeu, 0x47u, 0x38u, 0x16u,
        0x26u, 0x2cu, 0x16u, 0xacu, 0x85u, 0xd1u, 0xd7u, 0xb6u, 0x71u, 0xdau};
    char peer_certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY] = {0};
    uint8_t server_end_point[CNET_TLS_SERVER_END_POINT_MAX_BYTES] = {0};
    size_t server_end_point_size = 0u;
    uint8_t client_channel_binding[CNET_TLS_CHANNEL_BINDING_BYTES] = {0};
    uint8_t server_channel_binding[CNET_TLS_CHANNEL_BINDING_BYTES] = {0};
    char server_name[] = "localhost";
    const char *ca_path = CNET_TLS_TEST_IP_CA;
    const char *cert_path = CNET_TLS_TEST_IP_CERT;
    const char *key_path = CNET_TLS_TEST_IP_KEY;
    char uri[64];
    uint16_t port = 0u;
    uint64_t deadline;
    bool accepted = false;

    tls_server_config = (cnet_tls_server_config){.size = sizeof(tls_server_config),
                                                 .cert_file = cert_path,
                                                 .key_file = key_path,
                                                 .client_auth = CNET_TLS_CLIENT_AUTH_NONE,
                                                 .alpn_protocols = server_alpn,
                                                 .alpn_protocol_count = 2u};
    tls_client_config = (cnet_tls_client_config){.size = sizeof(tls_client_config),
                                                 .ca_file = ca_path,
                                                 .server_name = server_name,
                                                 .alpn_protocols = client_alpn,
                                                 .alpn_protocol_count = 2u};

    check_equal(cnet_tls_server_init(&tls_server, &tls_server_config), SALTS_OK);
    check_equal(cnet_tls_client_init(&tls_client, &tls_client_config), SALTS_OK);
    server_name[0] = 'x';
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    check_equal(cnet_client_init(&server, &server_client_config), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_tcp_option_set(&listener, CNET_TCP_SOCKET_NODELAY, 1u), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    connect_options = (cnet_connect_options){.uri = uri,
                                             .observer = {.on_state = cnet_tls_network_state,
                                                          .on_receive = cnet_tls_network_receive,
                                                          .user = &client_probe,
                                                          .on_send = cnet_tls_network_send},
                                             .tls_client = &tls_client};
    check_equal(cnet_connect(&client, &connect_options, &client_connection), SALTS_OK);
    check_equal(cnet_tls_client_destroy(&tls_client), SALTS_OK);
    check_null(tls_client.impl);
    client_probe.connection = client_connection;
    check_equal(cnet_tls_server_end_point_binding(&client, client_connection, server_end_point,
                                                  sizeof(server_end_point), &server_end_point_size),
                SALTS_ENOTCONN);
    check_equal(server_end_point_size, (size_t)0u);

    deadline = cmeta_monotonic_ms() + 5000u;
    while ((!client_probe.connected || !server_probe.connected) && cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_true(accepted);
    check_true(client_probe.connected);
    check_true(server_probe.connected);
    upgrade.server_name = "localhost";
    {
      uint64_t nodelay = 0u;
      check_equal(cnet_connection_tcp_option_get(
                      &server, server_probe.connection, CNET_TCP_SOCKET_NODELAY, &nodelay), SALTS_OK);
      check_equal(nodelay, UINT64_C(1));
    }
    check_equal(cnet_start_tls(&client, client_connection, &upgrade), SALTS_ENOTSUP);
    check_equal(cnet_start_tls_server(&server, server_probe.connection, &tls_server),
                SALTS_ENOTSUP);
    check_equal(client_probe.alpn_status, SALTS_OK);
    check_equal(server_probe.alpn_status, SALTS_OK);
    check_equal(client_probe.alpn_size, (size_t)2u);
    check_equal(server_probe.alpn_size, (size_t)2u);
    check_equal(memcmp(client_probe.alpn, "h2", 2u), 0);
    check_equal(memcmp(server_probe.alpn, "h2", 2u), 0);
    check_equal(client_probe.tls_version_status, SALTS_OK);
    check_equal(server_probe.tls_version_status, SALTS_OK);
    check_equal(strcmp(client_probe.tls_version, server_probe.tls_version), 0);
    check_true(strcmp(client_probe.tls_version, "TLSv1.2") == 0 ||
               strcmp(client_probe.tls_version, "TLSv1.3") == 0);
    check_equal(client_probe.tls_version_size, strlen(client_probe.tls_version));
    check_equal(server_probe.tls_version_size, strlen(server_probe.tls_version));
    check_equal(client_probe.tls_cipher_status, SALTS_OK);
    check_equal(server_probe.tls_cipher_status, SALTS_OK);
    check_greater(client_probe.tls_cipher_size, (size_t)0u);
    check_equal(strcmp(client_probe.tls_cipher, server_probe.tls_cipher), 0);
    check_equal(client_probe.tls_cipher_size, strlen(client_probe.tls_cipher));
    check_equal(server_probe.tls_cipher_size, strlen(server_probe.tls_cipher));
    check_equal(
        cnet_tls_peer_certificate_sha256(&client, client_connection, peer_certificate_sha256),
        SALTS_OK);
    check_equal(strcmp(peer_certificate_sha256,
                       "e5bbec0e499dc100dbc2414e7a09c687e0efbe473816262c16ac85d1d7b671da"),
                0);
    check_equal(
        cnet_tls_peer_certificate_sha256(&server, server_probe.connection, peer_certificate_sha256),
        SALTS_ENOENT);
    check_equal(cnet_tls_server_end_point_binding(&client, client_connection, server_end_point,
                                                  sizeof(server_end_point), &server_end_point_size),
                SALTS_OK);
    check_equal(server_end_point_size, sizeof(expected_server_end_point));
    check_equal(
        memcmp(server_end_point, expected_server_end_point, sizeof(expected_server_end_point)), 0);
    server_end_point_size = 0u;
    check_equal(cnet_tls_server_end_point_binding(&server, server_probe.connection,
                                                  server_end_point, sizeof(server_end_point),
                                                  &server_end_point_size),
                SALTS_ENOENT);
    check_equal(server_end_point_size, (size_t)0u);
    check_equal(cnet_tls_export_channel_binding(&client, client_connection, client_channel_binding),
                SALTS_OK);
    check_equal(
        cnet_tls_export_channel_binding(&server, server_probe.connection, server_channel_binding),
        SALTS_OK);
    check_equal(
        memcmp(client_channel_binding, server_channel_binding, CNET_TLS_CHANNEL_BINDING_BYTES), 0);

    /* TLS records and receive callbacks are not aligned with one logical slicev send.
     * Retained vectors feed plaintext spans independently, so request enough bounded
     * receive demand for the complete byte stream without depending on record grouping. */
    check_equal(cnet_receive(&server, server_probe.connection, 4u), SALTS_OK);
    request_first = mem_get_buffer(mem_global(), 2u);
    request_second = mem_get_buffer(mem_global(), 2u);
    check_not_null(request_first);
    check_not_null(request_second);
    memcpy(mem_buffer_data(request_first), "pi", 2u);
    memcpy(mem_buffer_data(request_second), "ng", 2u);
    mem_set_used(request_first, 2u);
    mem_set_used(request_second, 2u);
    request_segments[0] = mem_slice(request_first, 0u, 2u);
    request_segments[1] = mem_slice(request_second, 0u, 2u);
    check_not_null(request_segments[0].buffer);
    check_not_null(request_segments[1].buffer);
    check_equal(mem_buffer_ref_count(request_first), UINT32_C(2));
    check_equal(mem_buffer_ref_count(request_second), UINT32_C(2));
    check_equal(cnet_send_slicev(&client, client_connection, request_segments, 2u), SALTS_OK);
    check_equal(mem_buffer_ref_count(request_first), UINT32_C(3));
    check_equal(mem_buffer_ref_count(request_second), UINT32_C(3));
    mem_slice_release(&request_segments[0]);
    mem_slice_release(&request_segments[1]);
    check_equal(mem_buffer_ref_count(request_first), UINT32_C(2));
    check_equal(mem_buffer_ref_count(request_second), UINT32_C(2));
    mem_buffer_release(request_first);
    mem_buffer_release(request_second);
    request_first = NULL;
    request_second = NULL;
    check_equal(cnet_tls_test_send_bytes(&client, client_connection, second_request,
                                         sizeof(second_request) - 1u),
                SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((server_probe.received_size < sizeof(combined_requests) - 1u || client_probe.sent < 2) &&
           cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_equal(server_probe.received_size, sizeof(combined_requests) - 1u);
    check_equal(memcmp(server_probe.received, combined_requests, sizeof(combined_requests) - 1u),
                0);
    check_equal(client_probe.sent, 2);

    /*
     * Switch one verified TLS plaintext receive to the explicit owned surface.
     * The old borrowed callback must remain silent for this admitted demand.
     */
    check_equal(cnet_set_receive_slice_handler(&client, client_connection,
                                               cnet_tls_network_receive_owned, &client_probe),
                SALTS_OK);
    check_equal(cnet_receive(&client, client_connection, 1u), SALTS_OK);
    check_equal(
        cnet_tls_test_send_bytes(&server, server_probe.connection, response, sizeof(response) - 1u),
        SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((client_probe.owned_received_size == 0u || server_probe.sent == 0) &&
           cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_equal(client_probe.received_size, (size_t)0u);
    check_equal(client_probe.owned_received_size, sizeof(response) - 1u);
    check_equal(client_probe.owned_kind, CNET_MESSAGE_BYTES);
    check_not_null(client_probe.owned_slice.buffer);
    check_true(mem_buffer_pool(client_probe.owned_slice.buffer) == mem_global());
    check_equal(memcmp(client_probe.owned_slice.data, response, sizeof(response) - 1u), 0);
    check_equal(server_probe.sent, 1);

    server_probe.received_size = 0u;
    memset(server_probe.received, 0, sizeof(server_probe.received));
    check_equal(cnet_receive(&server, server_probe.connection, 1u), SALTS_OK);
    final_buffer = mem_get_buffer(mem_global(), sizeof(final_request) - 1u);
    check_not_null(final_buffer);
    memcpy(mem_buffer_data(final_buffer), final_request, sizeof(final_request) - 1u);
    mem_set_used(final_buffer, sizeof(final_request) - 1u);
    check_equal(mem_buffer_ref_count(final_buffer), UINT32_C(1));
    check_equal(cnet_send_buffer_and_close(&client, client_connection, final_buffer), SALTS_OK);
    check_equal(mem_buffer_ref_count(final_buffer), UINT32_C(2));
    mem_buffer_release(final_buffer);
    final_buffer = NULL;
    check_equal(cnet_tls_test_send_bytes(&client, client_connection, request, sizeof(request) - 1u),
                SALTS_EBUSY);
    check_equal(cnet_receive(&client, client_connection, 1u), SALTS_EBUSY);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((!client_probe.terminal || !server_probe.terminal ||
            server_probe.received_size < sizeof(final_request) - 1u) &&
           cmeta_monotonic_ms() < deadline)
      check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server, &server_probe,
                                         &accepted),
                  SALTS_OK);
    check_equal(server_probe.received_size, sizeof(final_request) - 1u);
    check_equal(memcmp(server_probe.received, final_request, sizeof(final_request) - 1u), 0);
    check_equal(client_probe.sent, 3);
    check_true(client_probe.terminal);
    check_true(server_probe.terminal);
    check_false(client_probe.failed);
    check_false(server_probe.failed);
    check_equal(
        cnet_tls_peer_certificate_sha256(&client, client_connection, peer_certificate_sha256),
        SALTS_ENOENT);

    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_stop(&server, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_client_destroy(&server), SALTS_OK);

    /* Owned decrypted plaintext remains valid after both CNet clients are gone. */
    check_not_null(client_probe.owned_slice.buffer);
    check_equal(client_probe.owned_received_size, sizeof(response) - 1u);
    check_equal(memcmp(client_probe.owned_slice.data, response, sizeof(response) - 1u), 0);
    mem_slice_release(&client_probe.owned_slice);
    check_null(client_probe.owned_slice.buffer);

    check_equal(cnet_tls_server_destroy(&tls_server), SALTS_OK);
  }

  it("upgrades one negotiated plaintext connection in place and carries TLS bytes") {
    static const char plaintext_request[] = "STARTTLS";
    static const char plaintext_response[] = "READY";
    static const char secure_request[] = "secret";
    cnet_client client = {0};
    cnet_client server = {0};
    cnet_listener listener = {0};
    cnet_tls_server tls_server = {0};
    cnet_client_config client_config = cnet_tls_network_config();
    cnet_client_config server_config = cnet_tls_network_config();
    cnet_listener_config listener_config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_tls_network_probe client_probe = {.client = &client};
    cnet_tls_network_probe server_probe = {.client = &server};
    cnet_observer client_observer = {.on_state = cnet_tls_network_state,
                                     .on_receive = cnet_tls_network_receive,
                                     .user = &client_probe,
                                     .on_send = cnet_tls_network_send};
    cnet_observer server_observer = {.on_state = cnet_tls_network_state,
                                     .on_receive = cnet_tls_network_receive,
                                     .user = &server_probe,
                                     .on_send = cnet_tls_network_send};
    cnet_tls_server_config tls_server_config;
    cnet_tls_client_config tls_client_config;
    cnet_start_tls_options tls_options = CNET_START_TLS_OPTIONS_INIT;
    cnet_connect_options connect_options;
    cnet_connection client_connection = {0};
    cnet_connection server_connection = {0};
    const char *ca_path = CNET_TLS_TEST_IP_CA;
    const char *cert_path = CNET_TLS_TEST_IP_CERT;
    const char *key_path = CNET_TLS_TEST_IP_KEY;
    char uri[64];
    uint16_t port = 0u;
    uint64_t deadline;
    bool accepted = false;

    tls_server_config = (cnet_tls_server_config){.size = sizeof(tls_server_config),
                                                 .cert_file = cert_path,
                                                 .key_file = key_path,
                                                 .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
    tls_client_config = (cnet_tls_client_config){
        .size = sizeof(tls_client_config), .ca_file = ca_path, .server_name = "localhost"};
    tls_options.tls = &tls_client_config;

    check_equal(cnet_tls_server_init(&tls_server, &tls_server_config), SALTS_OK);
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    check_equal(cnet_client_init(&server, &server_config), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    connect_options = (cnet_connect_options){.uri = uri, .observer = client_observer};
    check_equal(cnet_connect(&client, &connect_options, &client_connection), SALTS_OK);

    deadline = cmeta_monotonic_ms() + 5000u;
    while ((!client_probe.connected || !server_probe.connected) &&
           cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      int ready = 0;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      if (!accepted) {
        check_equal(cnet_listener_wait(&listener, 0u, &ready), SALTS_OK);
        if (ready != 0) {
          check_equal(
              cnet_listener_accept(&listener, &server, &server_observer, &server_connection),
              SALTS_OK);
          server_probe.connection = server_connection;
          accepted = true;
        }
      }
      check_equal(cnet_client_poll(&server, 1u, &events), SALTS_OK);
    }
    check_true(accepted);
    check_equal(client_probe.connected_count, 1);
    check_equal(server_probe.connected_count, 1);

    check_equal(cnet_receive(&server, server_connection, 1u), SALTS_OK);
    check_equal(cnet_tls_test_send_bytes(&client, client_connection, plaintext_request,
                                         sizeof(plaintext_request) - 1u),
                SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((server_probe.received_size == 0u || client_probe.sent == 0) &&
           cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_client_poll(&server, 1u, &events), SALTS_OK);
    }
    check_equal(server_probe.received_size, sizeof(plaintext_request) - 1u);
    check_equal(memcmp(server_probe.received, plaintext_request, sizeof(plaintext_request) - 1u),
                0);

    client_probe.received_size = 0u;
    check_equal(cnet_receive(&client, client_connection, 1u), SALTS_OK);
    check_equal(cnet_tls_test_send_bytes(&server, server_connection, plaintext_response,
                                         sizeof(plaintext_response) - 1u),
                SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((client_probe.received_size == 0u || server_probe.sent == 0) &&
           cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_client_poll(&server, 1u, &events), SALTS_OK);
    }
    check_equal(client_probe.received_size, sizeof(plaintext_response) - 1u);
    check_equal(memcmp(client_probe.received, plaintext_response, sizeof(plaintext_response) - 1u),
                0);

    client_probe.received_size = 0u;
    server_probe.received_size = 0u;
    check_equal(cnet_start_tls_server(&server, server_connection, &tls_server), SALTS_OK);
    check_equal(cnet_start_tls(&client, client_connection, &tls_options), SALTS_OK);
    check_equal(cnet_tls_server_destroy(&tls_server), SALTS_OK);

    deadline = cmeta_monotonic_ms() + 5000u;
    while ((client_probe.connected_count != 2 || server_probe.connected_count != 2) &&
           !client_probe.terminal && !server_probe.terminal && cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_client_poll(&server, 1u, &events), SALTS_OK);
    }
    check_equal(client_probe.handshaking, 1);
    check_equal(server_probe.handshaking, 1);
    check_equal(client_probe.connected_count, 2);
    check_equal(server_probe.connected_count, 2);
    check_false(client_probe.failed);
    check_false(server_probe.failed);

    check_equal(cnet_receive(&server, server_connection, 1u), SALTS_OK);
    check_equal(cnet_tls_test_send_bytes(&client, client_connection, secure_request,
                                         sizeof(secure_request) - 1u),
                SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((server_probe.received_size == 0u || client_probe.sent < 2) && !client_probe.terminal &&
           !server_probe.terminal && cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_client_poll(&server, 1u, &events), SALTS_OK);
    }
    check_equal(client_probe.sent, 2);
    check_false(client_probe.terminal);
    check_false(server_probe.terminal);
    check_false(client_probe.failed);
    check_false(server_probe.failed);
    check_equal(server_probe.received_size, sizeof(secure_request) - 1u);
    check_equal(memcmp(server_probe.received, secure_request, sizeof(secure_request) - 1u), 0);

    check_equal(cnet_receive(&server, server_connection, 1u), SALTS_OK);
    check_equal(cnet_close(&client, client_connection), SALTS_OK);
    deadline = cmeta_monotonic_ms() + 5000u;
    while ((!client_probe.terminal || !server_probe.terminal) && cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_client_poll(&server, 1u, &events), SALTS_OK);
    }
    check_true(client_probe.terminal);
    check_true(server_probe.terminal);

    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_stop(&server, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_client_destroy(&server), SALTS_OK);
  }

  it("times out a silent peer in the handshake stage without downgrading") {
    cnet_client client = {0};
    cnet_client raw_server = {0};
    cnet_listener listener = {0};
    cnet_client_config client_config = cnet_tls_network_config();
    cnet_client_config server_config = cnet_tls_network_config();
    cnet_listener_config listener_config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_tls_client_config tls_config;
    cnet_tls_network_probe client_probe = {.client = &client};
    cnet_tls_network_probe server_probe = {.client = &raw_server};
    cnet_observer server_observer = {.on_state = cnet_tls_network_state,
                                     .on_receive = cnet_tls_network_receive,
                                     .user = &server_probe,
                                     .on_send = cnet_tls_network_send};
    cnet_connect_options options;
    cnet_connection client_connection = {0};
    cnet_connection server_connection = {0};
    const char *ca_path = CNET_TLS_TEST_IP_CA;
    char uri[64];
    uint16_t port = 0u;
    uint64_t deadline;
    bool accepted = false;

    client_config.tls_handshake_timeout_ms = 20u;
    server_config.tls_io_buffer_bytes = 0u;
    server_config.tls_handshake_timeout_ms = 0u;
    tls_config = (cnet_tls_client_config){
        .size = sizeof(tls_config), .ca_file = ca_path, .server_name = "localhost"};
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    check_equal(cnet_client_init(&raw_server, &server_config), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_tls_network_state,
                                                  .on_receive = cnet_tls_network_receive,
                                                  .user = &client_probe,
                                                  .on_send = cnet_tls_network_send},
                                     .tls = &tls_config};
    check_equal(cnet_connect(&client, &options, &client_connection), SALTS_OK);
    deadline = cmeta_monotonic_ms() + 2000u;
    while (!client_probe.terminal && cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      int ready = 0;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      if (!accepted) {
        check_equal(cnet_listener_wait(&listener, 0u, &ready), SALTS_OK);
        if (ready != 0) {
          check_equal(
              cnet_listener_accept(&listener, &raw_server, &server_observer, &server_connection),
              SALTS_OK);
          server_probe.connection = server_connection;
          accepted = true;
        }
      }
      check_equal(cnet_client_poll(&raw_server, 1u, &events), SALTS_OK);
    }
    check_true(accepted);
    check_true(client_probe.terminal);
    check_true(client_probe.failed);
    check_equal(client_probe.failure_status, SALTS_ETIMEDOUT);
    check_equal(strcmp(client_probe.failure_stage, "handshake"), 0);
    check_false(client_probe.connected);

    if (!server_probe.terminal) check_equal(cnet_close(&raw_server, server_connection), SALTS_OK);
    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_stop(&raw_server, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_client_destroy(&raw_server), SALTS_OK);
  }

  it("cancels a handshake without publishing a connection or failure") {
    cnet_client client = {0};
    cnet_client raw_server = {0};
    cnet_listener listener = {0};
    cnet_client_config client_config = cnet_tls_network_config();
    cnet_client_config server_config = cnet_tls_network_config();
    cnet_listener_config listener_config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_tls_client_config tls_config = {.size = sizeof(tls_config), .server_name = "localhost"};
    cnet_tls_network_probe client_probe = {.client = &client};
    cnet_tls_network_probe server_probe = {.client = &raw_server};
    cnet_observer server_observer = {.on_state = cnet_tls_network_state,
                                     .on_receive = cnet_tls_network_receive,
                                     .user = &server_probe,
                                     .on_send = cnet_tls_network_send};
    cnet_connect_options options;
    cnet_connection client_connection = {0};
    cnet_connection server_connection = {0};
    char uri[64];
    uint16_t port = 0u;
    uint64_t deadline;
    bool accepted = false;

    server_config.tls_io_buffer_bytes = 0u;
    server_config.tls_handshake_timeout_ms = 0u;
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    check_equal(cnet_client_init(&raw_server, &server_config), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_tls_network_state,
                                                  .on_receive = cnet_tls_network_receive,
                                                  .user = &client_probe,
                                                  .on_send = cnet_tls_network_send},
                                     .tls = &tls_config};
    check_equal(cnet_connect(&client, &options, &client_connection), SALTS_OK);

    deadline = cmeta_monotonic_ms() + 2000u;
    while (!accepted && cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      int ready = 0;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_listener_wait(&listener, 0u, &ready), SALTS_OK);
      if (ready != 0) {
        check_equal(
            cnet_listener_accept(&listener, &raw_server, &server_observer, &server_connection),
            SALTS_OK);
        server_probe.connection = server_connection;
        accepted = true;
      }
      check_equal(cnet_client_poll(&raw_server, 1u, &events), SALTS_OK);
    }
    check_true(accepted);
    check_false(client_probe.connected);
    check_equal(cnet_close(&client, client_connection), SALTS_OK);

    deadline = cmeta_monotonic_ms() + 2000u;
    while (!client_probe.terminal && cmeta_monotonic_ms() < deadline) {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 1u, &events), SALTS_OK);
      check_equal(cnet_client_poll(&raw_server, 1u, &events), SALTS_OK);
    }
    check_true(client_probe.terminal);
    check_false(client_probe.connected);
    check_false(client_probe.failed);

    if (!server_probe.terminal) check_equal(cnet_close(&raw_server, server_connection), SALTS_OK);
    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_stop(&raw_server, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_client_destroy(&raw_server), SALTS_OK);
  }
  group("Let's Encrypt-style P-384 issuer with P-256 ECDSA/SHA-384 leaf") {
    const cnet_tls_protocol_version tls_versions[] = {
        CNET_TLS_PROTOCOL_VERSION_1_2, CNET_TLS_PROTOCOL_VERSION_1_3};
    const char *tls_version_names[] = {"TLSv1.2", "TLSv1.3"};

    it("accepts signed P-256 leaf and sends authenticated TLS 1.2/1.3 application data") {
      static const unsigned char payload[] = "CNet P384 issuer SHA384 verified payload";
      size_t index;
      for (index = 0u; index < sizeof(tls_versions) / sizeof(tls_versions[0]); ++index) {
        cnet_tls_test_pair pair = {0};
        char negotiated[16] = {0};
        size_t negotiated_len = 0u;
        check_equal(cnet_tls_test_fixture_pair_init(
                        &pair, CNET_TLS_TEST_P384_CA, CNET_TLS_TEST_P384_LEAF,
                        CNET_TLS_TEST_P384_KEY, "localhost"), SALTS_OK);
        check_equal(cnet_tls_state_set_protocol_range(&pair.client,
                                                      tls_versions[index], tls_versions[index]),
                    SALTS_OK);
        check_equal(cnet_tls_state_set_protocol_range(&pair.server,
                                                      tls_versions[index], tls_versions[index]),
                    SALTS_OK);
        check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
        check_true(cnet_tls_state_handshake_complete(&pair.client));
        check_true(cnet_tls_state_handshake_complete(&pair.server));
        check_equal(cnet_tls_state_negotiated_version(
                        &pair.client, negotiated, sizeof(negotiated), &negotiated_len), SALTS_OK);
        check_equal(strcmp(negotiated, tls_version_names[index]), 0);
        check_equal(negotiated_len, strlen(tls_version_names[index]));
        check_equal(cnet_tls_test_round_trip(
                        &pair.client, &pair.server, payload, sizeof(payload) - 1u), SALTS_OK);
        cnet_tls_test_pair_destroy(&pair);
      }
    }

    it("accepts P-384/SHA-384 issuing CA through the public CNet NativeIO TCP TLS path") {
      static const char message[] = "P384-SHA384-OK";
      cnet_client client = {0};
      cnet_client server = {0};
      cnet_listener listener = {0};
      cnet_tls_server tls_server = {0};
      cnet_client_config client_config = cnet_tls_network_config();
      cnet_client_config server_config = cnet_tls_network_config();
      cnet_listener_config listener_config = {
          .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
      cnet_tls_server_config server_tls = {
          .size = sizeof(server_tls),
          .cert_file = CNET_TLS_TEST_P384_LEAF,
          .key_file = CNET_TLS_TEST_P384_KEY,
          .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
      cnet_tls_client_config client_tls = {
          .size = sizeof(client_tls),
          .ca_file = CNET_TLS_TEST_P384_CA,
          .server_name = "localhost"};
      cnet_tls_network_probe client_probe = {.client = &client};
      cnet_tls_network_probe server_probe = {.client = &server};
      cnet_connect_options connect_options;
      cnet_connection client_connection = {0};
      char uri[64] = {0};
      uint16_t port = 0u;
      uint64_t deadline = 0u;
      bool accepted = false;

      check_equal(cnet_tls_server_init(&tls_server, &server_tls), SALTS_OK);
      check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
      check_equal(cnet_client_init(&server, &server_config), SALTS_OK);
      check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
      check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
      check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u", (unsigned int)port), 0);
      connect_options = (cnet_connect_options){
          .uri = uri,
          .observer = {.on_state = cnet_tls_network_state,
                       .on_receive = cnet_tls_network_receive,
                       .on_send = cnet_tls_network_send,
                       .user = &client_probe},
          .tls = &client_tls};
      check_equal(cnet_connect(&client, &connect_options, &client_connection), SALTS_OK);

      deadline = cmeta_monotonic_ms() + 5000u;
      while ((!client_probe.connected || !server_probe.connected) &&
             !client_probe.terminal && !server_probe.terminal &&
             cmeta_monotonic_ms() < deadline)
        check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server,
                                           &server_probe, &accepted), SALTS_OK);
      check_true(accepted);
      check_true(client_probe.connected);
      check_true(server_probe.connected);
      check_false(client_probe.failed);
      check_false(server_probe.failed);
      check_equal(client_probe.tls_version_status, SALTS_OK);
      check_true(strcmp(client_probe.tls_version, "TLSv1.3") == 0 ||
                 strcmp(client_probe.tls_version, "TLSv1.2") == 0);
      check_equal(client_probe.tls_cipher_status, SALTS_OK);
      check_greater(client_probe.tls_cipher_size, (size_t)0u);

      check_equal(cnet_receive(&server, server_probe.connection, 1u), SALTS_OK);
      check_equal(cnet_tls_test_send_bytes(&client, client_connection, message,
                                           sizeof(message) - 1u), SALTS_OK);
      deadline = cmeta_monotonic_ms() + 5000u;
      while (server_probe.received_size < sizeof(message) - 1u &&
             !client_probe.terminal && !server_probe.terminal &&
             cmeta_monotonic_ms() < deadline)
        check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server,
                                           &server_probe, &accepted), SALTS_OK);
      check_false(client_probe.failed);
      check_false(server_probe.failed);
      check_equal(server_probe.received_size, sizeof(message) - 1u);
      check_equal(memcmp(server_probe.received, message, sizeof(message) - 1u), 0);

      check_equal(cnet_close(&client, client_connection), SALTS_OK);
      deadline = cmeta_monotonic_ms() + 5000u;
      while ((!client_probe.terminal || !server_probe.terminal) &&
             cmeta_monotonic_ms() < deadline)
        check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server,
                                           &server_probe, &accepted), SALTS_OK);
      check_true(client_probe.terminal);
      check_true(server_probe.terminal);
      check_equal(cnet_listener_close(&listener), SALTS_OK);
      check_equal(cnet_listener_destroy(&listener), SALTS_OK);
      check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
      check_equal(cnet_client_stop(&server, 5000u), SALTS_OK);
      check_equal(cnet_client_destroy(&client), SALTS_OK);
      check_equal(cnet_client_destroy(&server), SALTS_OK);
      check_equal(cnet_tls_server_destroy(&tls_server), SALTS_OK);
    }

    it("rejects wrong CA and wrong DNS over the public NativeIO TLS owner") {
      static const struct {
        const char *ca;
        const char *server_name;
      } failures[] = {
          {CNET_TLS_TEST_P384_WRONG_CA, "localhost"},
          {CNET_TLS_TEST_P384_CA, "wrong.example"}};
      size_t case_index;

      for (case_index = 0u; case_index < sizeof(failures) / sizeof(failures[0]);
           ++case_index) {
        cnet_client client = {0};
        cnet_client server = {0};
        cnet_listener listener = {0};
        cnet_tls_server tls_server = {0};
        cnet_client_config client_config = cnet_tls_network_config();
        cnet_client_config server_config = cnet_tls_network_config();
        cnet_listener_config listener_config = {
            .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
        cnet_tls_server_config server_tls = {
            .size = sizeof(server_tls),
            .cert_file = CNET_TLS_TEST_P384_LEAF,
            .key_file = CNET_TLS_TEST_P384_KEY,
            .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
        cnet_tls_client_config client_tls = {
            .size = sizeof(client_tls),
            .ca_file = failures[case_index].ca,
            .server_name = failures[case_index].server_name};
        cnet_tls_network_probe client_probe = {.client = &client};
        cnet_tls_network_probe server_probe = {.client = &server};
        cnet_connect_options options;
        cnet_connection client_connection = {0};
        char uri[64] = {0};
        uint16_t port = 0u;
        uint64_t deadline;
        bool accepted = false;

        check_equal(cnet_tls_server_init(&tls_server, &server_tls), SALTS_OK);
        check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
        check_equal(cnet_client_init(&server, &server_config), SALTS_OK);
        check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
        check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
        check_greater(snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u",
                               (unsigned int)port), 0);
        options = (cnet_connect_options){
            .uri = uri,
            .observer = {.on_state = cnet_tls_network_state,
                         .on_receive = cnet_tls_network_receive,
                         .on_send = cnet_tls_network_send,
                         .user = &client_probe},
            .tls = &client_tls};
        check_equal(cnet_connect(&client, &options, &client_connection), SALTS_OK);

        deadline = cmeta_monotonic_ms() + 5000u;
        while (!client_probe.terminal && cmeta_monotonic_ms() < deadline)
          check_equal(cnet_tls_network_drive(&client, &server, &listener, &tls_server,
                                             &server_probe, &accepted), SALTS_OK);

        /* Public Owner must never publish an authenticated state or deliver
         * plaintext on either certificate-trust or server-identity rejection. */
        check_true(accepted);
        /* A failed trust decision may omit the intermediate HANDSHAKING event. */
        check_true(client_probe.terminal);
        check_true(client_probe.failed);
        check_false(client_probe.connected);
        check_equal(client_probe.connected_count, 0);
        check_equal(client_probe.received_size, (size_t)0u);
        check_equal(server_probe.received_size, (size_t)0u);
        check_equal(client_probe.sent, 0);
        check_not_null(client_probe.failure_stage);
        if (client_probe.failure_stage != NULL)
          check_equal(strcmp(client_probe.failure_stage, "handshake"), 0);

        if (accepted && !server_probe.terminal)
          check_equal(cnet_close(&server, server_probe.connection), SALTS_OK);
        check_equal(cnet_listener_close(&listener), SALTS_OK);
        check_equal(cnet_listener_destroy(&listener), SALTS_OK);
        check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
        check_equal(cnet_client_stop(&server, 5000u), SALTS_OK);
        check_equal(cnet_client_destroy(&client), SALTS_OK);
        check_equal(cnet_client_destroy(&server), SALTS_OK);
        check_equal(cnet_tls_server_destroy(&tls_server), SALTS_OK);
      }
    }

    it("refuses a P-384 issuing CA outside the trusted roots") {
      cnet_tls_test_pair pair = {0};
      check_equal(cnet_tls_test_fixture_pair_init(
                      &pair, CNET_TLS_TEST_P384_WRONG_CA, CNET_TLS_TEST_P384_LEAF,
                      CNET_TLS_TEST_P384_KEY, "localhost"), SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
      check_false(cnet_tls_state_handshake_complete(&pair.client));
      cnet_tls_test_pair_destroy(&pair);
    }

    it("rejects server identity mismatch despite valid P-384 chain signature") {
      cnet_tls_test_pair pair = {0};
      check_equal(cnet_tls_test_fixture_pair_init(
                      &pair, CNET_TLS_TEST_P384_CA, CNET_TLS_TEST_P384_LEAF,
                      CNET_TLS_TEST_P384_KEY, "wrong.example"), SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
      check_false(cnet_tls_state_handshake_complete(&pair.client));
      cnet_tls_test_pair_destroy(&pair);
    }

    it("rejects an expired P-256 leaf signed by a still-valid P-384 CA") {
      cnet_tls_test_pair pair = {0};
      check_equal(cnet_tls_test_fixture_pair_init(
                      &pair, CNET_TLS_TEST_P384_CA, CNET_TLS_TEST_P384_EXPIRED,
                      CNET_TLS_TEST_P384_KEY, "localhost"), SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
      check_false(cnet_tls_state_handshake_complete(&pair.client));
      cnet_tls_test_pair_destroy(&pair);
    }

    it("rejects a tampered ECDSA/SHA-384 certificate signature") {
      cnet_tls_test_pair pair = {0};
      check_equal(cnet_tls_test_fixture_pair_init(
                      &pair, CNET_TLS_TEST_P384_CA, CNET_TLS_TEST_P384_TAMPERED,
                      CNET_TLS_TEST_P384_KEY, "localhost"), SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
      check_false(cnet_tls_state_handshake_complete(&pair.client));
      cnet_tls_test_pair_destroy(&pair);
    }

    it("rejects a leaf signed by a CA:FALSE intermediate") {
      cnet_tls_test_pair pair = {0};
      check_equal(cnet_tls_test_fixture_pair_init(
                      &pair, CNET_TLS_TEST_INVALID_CA_ROOT,
                      CNET_TLS_TEST_INVALID_CA_FULLCHAIN,
                      CNET_TLS_TEST_INVALID_CA_KEY, "localhost"), SALTS_OK);
      check_equal(cnet_tls_test_handshake(&pair), SALTS_ECONNABORTED);
      check_false(cnet_tls_state_handshake_complete(&pair.client));
      check_false(cnet_tls_state_handshake_complete(&pair.server));
      cnet_tls_test_pair_destroy(&pair);
    }
  }

  group("RSA-3072 certificate identity") {
    const cnet_tls_protocol_version tls_versions[] = {
        CNET_TLS_PROTOCOL_VERSION_1_2, CNET_TLS_PROTOCOL_VERSION_1_3};
    const char *tls_version_names[] = {"TLSv1.2", "TLSv1.3"};

    it("authenticates RSA-3072/SHA-256 and transfers TLS 1.2/1.3 application data") {
      static const unsigned char payload[] = "CNet RSA3072 verified payload";
      size_t index;
      for (index = 0u; index < sizeof(tls_versions) / sizeof(tls_versions[0]); ++index) {
        cnet_tls_test_pair pair = {0};
        char negotiated[16] = {0};
        size_t negotiated_len = 0u;
        check_equal(cnet_tls_test_fixture_pair_init(
                        &pair, CNET_TLS_TEST_RSA3072_CA, CNET_TLS_TEST_RSA3072_LEAF,
                        CNET_TLS_TEST_RSA3072_KEY, "localhost"), SALTS_OK);
        check_equal(cnet_tls_state_set_protocol_range(&pair.client, tls_versions[index],
                                                      tls_versions[index]), SALTS_OK);
        check_equal(cnet_tls_state_set_protocol_range(&pair.server, tls_versions[index],
                                                      tls_versions[index]), SALTS_OK);
        check_equal(cnet_tls_test_handshake(&pair), SALTS_OK);
        check_equal(cnet_tls_state_negotiated_version(
                        &pair.client, negotiated, sizeof(negotiated), &negotiated_len), SALTS_OK);
        check_equal(strcmp(negotiated, tls_version_names[index]), 0);
        check_equal(negotiated_len, strlen(tls_version_names[index]));
        check_equal(cnet_tls_test_round_trip(
                        &pair.client, &pair.server, payload, sizeof(payload) - 1u), SALTS_OK);
        cnet_tls_test_pair_destroy(&pair);
      }
    }

    it("rejects an RSASSA-PSS-signed X.509 leaf without provider fallback") {
      cnet_tls_server server = {0};
      cnet_tls_server_config config = {
          .size = sizeof(config),
          .cert_file = CNET_TLS_TEST_RSA3072_PSS_LEAF,
          .key_file = CNET_TLS_TEST_RSA3072_KEY,
          .client_auth = CNET_TLS_CLIENT_AUTH_NONE};
      check_equal(cnet_tls_server_init(&server, &config), SALTS_EIO);
      check_null(server.impl);
      check_equal(cnet_tls_server_destroy(&server), SALTS_OK);
    }
  }

}
