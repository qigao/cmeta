/* Strict C11 installed SDK TLS consumer for #1196.
 * GmSSL is a private CNet implementation dependency: no provider includes,
 * vcpkg build-tree paths, alternate backends or certificate-check bypass.
 *
 * The in-tree cnet_tls_test pins TLS 1.2 and TLS 1.3 independently. This
 * installed public-ABI consumer verifies the installed CNet/NativeIO path and
 * records the negotiated version without relying on private TLS structures. */
#include <cnet/cnet.h>
#include <salts/error_codes.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      return 1; \
    } \
  } while (0)

typedef struct state_probe {
  cnet_client *client;
  cnet_connection connection;
  int connected;
  int handshaking;
  int terminal;
  int failed;
  int received;
  const char *failure_stage;
  char version[16];
} state_probe;

static native_io_backend_kind backend_kind(void)
{
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static cnet_client_config owner_config(void)
{
  cnet_client_config cfg = {0};
  cfg.backend = backend_kind();
  cfg.connection_capacity = 2u;
  cfg.command_capacity = 16u;
  cfg.request_capacity = 8u;
  cfg.completion_batch_capacity = 8u;
  cfg.event_capacity = 16u;
  cfg.max_send_bytes = 1024u;
  cfg.receive_buffer_bytes = 1024u;
  cfg.connect_timeout_ms = 5000u;
  cfg.read_timeout_ms = 5000u;
  cfg.write_timeout_ms = 5000u;
  cfg.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  cfg.tls_handshake_timeout_ms = 5000u;
  return cfg;
}

static void on_state(void *arg, cnet_connection connection,
                     cnet_connection_state state, const cnet_error *error)
{
  state_probe *probe = (state_probe *)arg;
  probe->connection = connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    size_t size = 0u;
    ++probe->connected;
    if (cnet_tls_negotiated_version(probe->client, connection,
                                     probe->version, sizeof(probe->version),
                                     &size) != SALTS_OK)
      probe->failed = 1;
    if (size == 0u || size >= sizeof(probe->version))
      probe->failed = 1;
  } else if (state == CNET_CONNECTION_TLS_HANDSHAKING) {
    ++probe->handshaking;
  } else if (state == CNET_CONNECTION_CLOSED ||
             state == CNET_CONNECTION_FAILED) {
    probe->terminal = 1;
    if (state == CNET_CONNECTION_FAILED || error != NULL) {
      probe->failed = 1;
      if (error != NULL) probe->failure_stage = error->stage;
    }
  }
}

static void on_receive(void *arg, cnet_connection connection,
                       const cnet_receive_view *view)
{
  state_probe *probe = (state_probe *)arg;
  (void)connection;
  (void)view;
  ++probe->received;
}

static int run_case(const char *ca_file, const char *cert_file,
                    const char *key_file, const char *identity,
                    bool reject)
{
  cnet_client client = {0}, server = {0};
  cnet_listener listener = {0};
  cnet_tls_server tls_server = {0};
  cnet_client_config ccfg = owner_config(), scfg = owner_config();
  cnet_listener_config lcfg = {0};
  cnet_tls_server_config stls = {0};
  cnet_tls_client_config ctls = {0};
  cnet_connect_options options = {0};
  cnet_connection client_connection = {0};
  state_probe cp = {0}, sp = {0};
  uint16_t port = 0u;
  char url[64] = {0};
  bool accepted = false;
  size_t iteration;

  cp.client = &client;
  sp.client = &server;
  stls.size = sizeof(stls);
  stls.cert_file = cert_file;
  stls.key_file = key_file;
  stls.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
  ctls.size = sizeof(ctls);
  ctls.ca_file = ca_file;
  ctls.server_name = identity;
  lcfg.backend = ccfg.backend;
  lcfg.host = "127.0.0.1";
  lcfg.port = 0u;
  lcfg.backlog = 2u;

  REQUIRE(cnet_tls_server_init(&tls_server, &stls) == SALTS_OK);
  REQUIRE(cnet_client_init(&client, &ccfg) == SALTS_OK);
  REQUIRE(cnet_client_init(&server, &scfg) == SALTS_OK);
  REQUIRE(cnet_listener_init(&listener, &lcfg) == SALTS_OK);
  REQUIRE(cnet_listener_port(&listener, &port) == SALTS_OK);
  REQUIRE(port != 0u);
  REQUIRE(snprintf(url, sizeof(url), "tls://127.0.0.1:%u",
                   (unsigned int)port) > 0);

  options.uri = url;
  options.tls = &ctls;
  options.observer.on_state = on_state;
  options.observer.on_receive = on_receive;
  options.observer.user = &cp;
  REQUIRE(cnet_connect(&client, &options, &client_connection) == SALTS_OK);
  for (iteration = 0u; iteration < 15000u; ++iteration) {
    size_t events = 0u;
    int ready = 0;
    REQUIRE(cnet_client_poll(&client, 1u, &events) == SALTS_OK);
    if (!accepted) {
      REQUIRE(cnet_listener_wait(&listener, 0u, &ready) == SALTS_OK);
      if (ready != 0) {
        cnet_observer observer = {0};
        observer.on_state = on_state;
        observer.on_receive = on_receive;
        observer.user = &sp;
        REQUIRE(cnet_listener_accept_tls(&listener, &server, &tls_server,
                                          &observer, &sp.connection) == SALTS_OK);
        accepted = true;
      }
    }
    REQUIRE(cnet_client_poll(&server, 1u, &events) == SALTS_OK);
    if (reject ? cp.terminal : (cp.connected && sp.connected)) break;
    /* The server may report a failed alert/close before the client owner's
     * async terminal completion is delivered. Keep driving the client until
     * it reports its own failed handshake; no accept/retry/fallback is added. */
    if (!reject && (cp.terminal || sp.terminal)) break;
  }

  REQUIRE(accepted);
  if (reject) {
    if (!cp.terminal || !cp.failed || cp.connected)
      fprintf(stderr,
              "CNet installed TLS reject state: client terminal=%d failed=%d connected=%d "
              "server terminal=%d failed=%d iteration=%zu\n",
              cp.terminal, cp.failed, cp.connected, sp.terminal, sp.failed, iteration);
    REQUIRE(cp.terminal && cp.failed && !cp.connected);
    REQUIRE(cp.failure_stage != NULL);
    REQUIRE(strcmp(cp.failure_stage, "handshake") == 0);
  } else {
    REQUIRE(cp.connected == 1 && sp.connected == 1);
    REQUIRE(!cp.failed && !sp.failed);
    REQUIRE(strcmp(cp.version, sp.version) == 0);
    REQUIRE(strcmp(cp.version, "TLSv1.2") == 0 ||
            strcmp(cp.version, "TLSv1.3") == 0);
  }
  REQUIRE(cp.received == 0 && sp.received == 0);
  if (!cp.terminal)
    REQUIRE(cnet_close(&client, client_connection) == SALTS_OK);
  if (accepted && !sp.terminal)
    REQUIRE(cnet_close(&server, sp.connection) == SALTS_OK);

  REQUIRE(cnet_listener_close(&listener) == SALTS_OK);
  REQUIRE(cnet_listener_destroy(&listener) == SALTS_OK);
  REQUIRE(cnet_client_stop(&client, 5000u) == SALTS_OK);
  REQUIRE(cnet_client_stop(&server, 5000u) == SALTS_OK);
  REQUIRE(cnet_client_destroy(&client) == SALTS_OK);
  REQUIRE(cnet_client_destroy(&server) == SALTS_OK);
  REQUIRE(cnet_tls_server_destroy(&tls_server) == SALTS_OK);
  return 0;
}

static int require_server_config_rejected(const char *cert_file,
                                          const char *key_file,
                                          const char *case_name)
{
  cnet_tls_server server = {0};
  cnet_tls_server_config config = {0};
  int status;

  config.size = sizeof(config);
  config.cert_file = cert_file;
  config.key_file = key_file;
  config.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
  status = cnet_tls_server_init(&server, &config);
  if (status != SALTS_EIO) {
    fprintf(stderr, "%s: expected SALTS_EIO, got %d\n", case_name, status);
    if (server.impl != NULL) (void)cnet_tls_server_destroy(&server);
    return 1;
  }
  REQUIRE(server.impl == NULL);
  REQUIRE(cnet_tls_server_destroy(&server) == SALTS_OK);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc != 19) {
    fprintf(stderr,
            "usage: %s CA CERT KEY WRONG_CA EXPIRED TAMPERED "
            "CHAIN_CA CHAIN_FULL CHAIN_LEAF CHAIN_KEY MALFORMED "
            "MISMATCHED_KEY UNSUPPORTED_CERT UNSUPPORTED_KEY "
            "RSA3072_CA RSA3072_CERT RSA3072_KEY RSA3072_PSS_CERT\n",
            argv[0]);
    return 2;
  }
  if (run_case(argv[1], argv[2], argv[3], "localhost", false)) return 1;
  if (run_case(argv[4], argv[2], argv[3], "localhost", true)) return 1;
  if (run_case(argv[1], argv[2], argv[3], "wrong.example", true)) return 1;
  if (run_case(argv[1], argv[5], argv[3], "localhost", true)) return 1;
  if (run_case(argv[1], argv[6], argv[3], "localhost", true)) return 1;
  if (run_case(argv[7], argv[8], argv[10], "localhost", false)) return 1;
  if (run_case(argv[7], argv[9], argv[10], "localhost", true)) return 1;
  if (require_server_config_rejected(argv[11], argv[10], "malformed certificate"))
    return 1;
  if (require_server_config_rejected(argv[2], argv[12], "mismatched private key"))
    return 1;
  if (require_server_config_rejected(argv[13], argv[14], "unsupported Ed25519 key"))
    return 1;
  if (run_case(argv[15], argv[16], argv[17], "localhost", false)) return 1;
  if (require_server_config_rejected(argv[18], argv[17],
                                     "unsupported RSASSA-PSS certificate signature"))
    return 1;
  puts("installed C11 CNet X.509 TLS matrix: PASS");
  return 0;
}
