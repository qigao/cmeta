/* The interrupted-poll regression needs pthread_kill before libc is included. */
#ifndef _WIN32
#ifdef __APPLE__
/* Keep Darwin socket constants visible alongside the POSIX signal APIs. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "cnet_test_named_pipe.h"
#if defined(CNET_INTERNAL_PROFILING)
#include "cnet_client_internal.h"
#endif
#include "cnet_transport.h"
#include "tinytest.h"
#include <cnet/cnet.h>

#include <salts/clock.h>
#include <salts/thread.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET cnet_api_test_socket;
  #define CNET_API_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <netinet/in.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int cnet_api_test_socket;
  #define CNET_API_TEST_INVALID_SOCKET (-1)
#endif

enum { CNET_API_TEST_TIMEOUT_MS = 5000, CNET_API_TEST_BATCH_DATAGRAMS = 256 };

/* Lock the pre-1.8.10 public cnet_stream_peer ABI. */
_Static_assert(offsetof(cnet_stream_peer, family) == 0u,
               "cnet_stream_peer family ABI changed");
_Static_assert(offsetof(cnet_stream_peer, port) == 4u,
               "cnet_stream_peer port ABI changed");
_Static_assert(offsetof(cnet_stream_peer, scope_id) == 8u,
               "cnet_stream_peer scope_id ABI changed");
_Static_assert(offsetof(cnet_stream_peer, address) == 12u,
               "cnet_stream_peer address ABI changed");
_Static_assert(sizeof(cnet_stream_peer) == 28u,
               "cnet_stream_peer size ABI changed");


static int cnet_api_test_send_bytes(cnet_client *client, cnet_connection connection,
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

static int cnet_api_test_send_bytes_and_close(cnet_client *client,
                                              cnet_connection connection,
                                              const void *data, size_t size) {
  mem_buffer_t *buffer;
  int status;
  if (client == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  buffer = mem_get_buffer(mem_global(), size);
  if (buffer == NULL) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  status = cnet_send_buffer_and_close(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

typedef struct cnet_api_test_probe {
  cnet_client *client;
  atomic_int connected;
  atomic_int received;
  atomic_int terminal;
  atomic_int callback_stop_status;
  atomic_int callback_operation_status;
  atomic_int failed;
  atomic_int block_terminal;
  atomic_int release_terminal;
  cnet_message_kind expected_kind;
  unsigned char received_value;
} cnet_api_test_probe;

typedef struct cnet_api_test_batch_probe {
  atomic_int connected;
  atomic_int received;
  atomic_int terminal;
  atomic_int failed;
  unsigned char received_value;
} cnet_api_test_batch_probe;

typedef struct cnet_api_test_poll_probe {
  cnet_client *client;
  atomic_int connected;
  atomic_int terminal;
  atomic_int callback_thread_marker;
  atomic_int failed;
} cnet_api_test_poll_probe;

typedef struct cnet_api_test_listener_probe {
  atomic_int connected;
  atomic_int received;
  atomic_int sent;
  atomic_int terminal;
  atomic_int failed;
  int handshaking;
  int failure_status;
  const char *failure_stage;
  unsigned char received_value;
  size_t expected_send_size;
} cnet_api_test_listener_probe;

typedef struct cnet_api_test_wake_probe {
  cnet_client *client;
  atomic_int started;
  atomic_int finished;
  int status;
  size_t events;
  uint64_t elapsed_ms;
} cnet_api_test_wake_probe;

typedef struct cnet_api_test_owned_receive_probe {
  atomic_int connected;
  atomic_int owned_received;
  atomic_int borrowed_received;
  atomic_int terminal;
  atomic_int failed;
  mem_slice_t owned_slice;
  cnet_message_kind owned_kind;
  unsigned char borrowed_value;
} cnet_api_test_owned_receive_probe;

#if !defined(_WIN32)
typedef struct cnet_api_test_interrupt_probe {
  pthread_t target;
  atomic_int armed;
  atomic_int signal_status;
} cnet_api_test_interrupt_probe;
#endif

static SALTS_THREAD_LOCAL int cnet_api_test_thread_marker;

static void cnet_api_test_blocking_poll(void *user) {
  cnet_api_test_wake_probe *probe = (cnet_api_test_wake_probe *)user;
  const uint64_t started_ms = cmeta_monotonic_ms();
  atomic_store_explicit(&probe->started, 1, memory_order_release);
  probe->status = cnet_client_poll(probe->client, 1000u, &probe->events);
  probe->elapsed_ms = cmeta_monotonic_ms() - started_ms;
  atomic_store_explicit(&probe->finished, 1, memory_order_release);
}

#if !defined(_WIN32)
static void cnet_api_test_signal_handler(int signal_number) { (void)signal_number; }

static void cnet_api_test_interrupt_wait(void *user) {
  cnet_api_test_interrupt_probe *probe = (cnet_api_test_interrupt_probe *)user;
  while (atomic_load_explicit(&probe->armed, memory_order_acquire) == 0)
    cmeta_thread_yield();
  cmeta_sleep_ms(10u);
  atomic_store_explicit(&probe->signal_status, pthread_kill(probe->target, SIGUSR1),
                        memory_order_release);
}
#endif

static void cnet_api_test_close_socket(cnet_api_test_socket socket_value) {
  if (socket_value == CNET_API_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int cnet_api_test_set_receive_timeout(cnet_api_test_socket socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = CNET_API_TEST_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms)) == 0
             ? SALTS_OK
             : SALTS_EIO;
#else
  const struct timeval timeout = {CNET_API_TEST_TIMEOUT_MS / 1000,
                                  (CNET_API_TEST_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) ==
                 0
             ? SALTS_OK
             : SALTS_EIO;
#endif
}

static int cnet_api_test_listener(cnet_api_test_socket *out_listener, uint16_t *out_port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int length = (int)sizeof(address);
#else
  socklen_t length = (socklen_t)sizeof(address);
#endif
  *out_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*out_listener == CNET_API_TEST_INVALID_SOCKET) return SALTS_EIO;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(*out_listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(*out_listener, (struct sockaddr *)&address, &length) != 0 ||
      listen(*out_listener, 2) != 0) {
    cnet_api_test_close_socket(*out_listener);
    *out_listener = CNET_API_TEST_INVALID_SOCKET;
    return SALTS_EIO;
  }
  *out_port = ntohs(address.sin_port);
  return SALTS_OK;
}

static int cnet_api_test_wait_pipe_read(cnet_shared_test_named_pipe *pipe, void *data,
                                        size_t size) {
#if !defined(_WIN32)
  const uint64_t deadline = cmeta_monotonic_ms() + CNET_API_TEST_TIMEOUT_MS;
#endif
  for (;;) {
    const int status = cnet_shared_test_named_pipe_peer_read(pipe, data, size);
    if (status == SALTS_OK) return SALTS_OK;
#if defined(_WIN32)
    return status;
#else
    if (status != -EAGAIN && status != -EWOULDBLOCK && status != -EINTR) return status;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
    cmeta_thread_yield();
#endif
  }
}

static void cnet_api_test_state(void *user, cnet_connection connection, cnet_connection_state state,
                                const cnet_error *error) {
  cnet_api_test_probe *probe = (cnet_api_test_probe *)user;
  if (state == CNET_CONNECTION_CONNECTED) {
    atomic_store_explicit(&probe->callback_stop_status,
                          cnet_client_stop(probe->client, CNET_API_TEST_TIMEOUT_MS),
                          memory_order_release);
    atomic_store_explicit(&probe->callback_operation_status,
                          cnet_receive(probe->client, connection, 1u), memory_order_release);
    atomic_fetch_add_explicit(&probe->connected, 1, memory_order_release);
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    if (state == CNET_CONNECTION_FAILED || error != NULL)
      atomic_store_explicit(&probe->failed, 1, memory_order_release);
    atomic_fetch_add_explicit(&probe->terminal, 1, memory_order_release);
    while (atomic_load_explicit(&probe->block_terminal, memory_order_acquire) != 0 &&
           atomic_load_explicit(&probe->release_terminal, memory_order_acquire) == 0)
      cmeta_thread_yield();
  }
}

static void cnet_api_test_receive(void *user, cnet_connection connection,
                                  const cnet_receive_view *view) {
  cnet_api_test_probe *probe = (cnet_api_test_probe *)user;
  if (view->kind != probe->expected_kind || view->size != 1u)
    atomic_store_explicit(&probe->failed, 1, memory_order_release);
  else probe->received_value = *(const unsigned char *)view->data;
  atomic_store_explicit(&probe->callback_operation_status, cnet_close(probe->client, connection),
                        memory_order_release);
  atomic_fetch_add_explicit(&probe->received, 1, memory_order_release);
}

static void cnet_api_test_ignore_state(void *user, cnet_connection connection,
                                       cnet_connection_state state, const cnet_error *error) {
  (void)user;
  (void)connection;
  (void)state;
  (void)error;
}

static void cnet_api_test_count_state(void *user, cnet_connection connection,
                                      cnet_connection_state state, const cnet_error *error) {
  (void)connection;
  (void)state;
  (void)error;
  ++*(size_t *)user;
}

static void cnet_api_test_poll_state(void *user, cnet_connection connection,
                                     cnet_connection_state state, const cnet_error *error) {
  cnet_api_test_poll_probe *probe = (cnet_api_test_poll_probe *)user;
  if (state == CNET_CONNECTION_CONNECTED) {
    atomic_store_explicit(&probe->callback_thread_marker, cnet_api_test_thread_marker,
                          memory_order_release);
    if (cnet_close(probe->client, connection) != SALTS_OK)
      atomic_store_explicit(&probe->failed, 1, memory_order_release);
    atomic_store_explicit(&probe->connected, 1, memory_order_release);
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    if (state == CNET_CONNECTION_FAILED || error != NULL)
      atomic_store_explicit(&probe->failed, 1, memory_order_release);
    atomic_store_explicit(&probe->terminal, 1, memory_order_release);
  }
}

static int cnet_api_test_poll_until(cnet_client *client, atomic_int *value, int expected) {
  const uint64_t deadline = cmeta_monotonic_ms() + CNET_API_TEST_TIMEOUT_MS;
  while (atomic_load_explicit(value, memory_order_acquire) < expected) {
    size_t events = 0u;
    const int status = cnet_client_poll(client, 1u, &events);
    if (status != SALTS_OK) return status;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

static void cnet_api_test_batch_state(void *user, cnet_connection connection,
                                      cnet_connection_state state, const cnet_error *error) {
  cnet_api_test_batch_probe *probe = (cnet_api_test_batch_probe *)user;
  (void)connection;
  if (state == CNET_CONNECTION_CONNECTED)
    atomic_store_explicit(&probe->connected, 1, memory_order_release);
  else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    if (state == CNET_CONNECTION_FAILED || error != NULL)
      atomic_store_explicit(&probe->failed, 1, memory_order_release);
    atomic_store_explicit(&probe->terminal, 1, memory_order_release);
  }
}

static void cnet_api_test_batch_receive(void *user, cnet_connection connection,
                                        const cnet_receive_view *view) {
  cnet_api_test_batch_probe *probe = (cnet_api_test_batch_probe *)user;
  (void)connection;
  if (view->kind != CNET_MESSAGE_DATAGRAM || view->size != 1u)
    atomic_store_explicit(&probe->failed, 1, memory_order_release);
  else probe->received_value = *(const unsigned char *)view->data;
  atomic_fetch_add_explicit(&probe->received, 1, memory_order_release);
}

static void cnet_api_test_listener_state(void *user, cnet_connection connection,
                                         cnet_connection_state state, const cnet_error *error) {
  cnet_api_test_listener_probe *probe = (cnet_api_test_listener_probe *)user;
  (void)connection;
  if (state == CNET_CONNECTION_CONNECTED)
    atomic_store_explicit(&probe->connected, 1, memory_order_release);
  else if (state == CNET_CONNECTION_TLS_HANDSHAKING) ++probe->handshaking;
  else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    if (state == CNET_CONNECTION_FAILED || error != NULL) {
      if (error != NULL) {
        probe->failure_status = error->status;
        probe->failure_stage = error->stage;
      }
      atomic_store_explicit(&probe->failed, 1, memory_order_release);
    }
    atomic_store_explicit(&probe->terminal, 1, memory_order_release);
  }
}

static void cnet_api_test_listener_receive(void *user, cnet_connection connection,
                                           const cnet_receive_view *view) {
  cnet_api_test_listener_probe *probe = (cnet_api_test_listener_probe *)user;
  (void)connection;
  if (view == NULL || view->kind != CNET_MESSAGE_BYTES || view->size != 1u)
    atomic_store_explicit(&probe->failed, 1, memory_order_release);
  else probe->received_value = *(const unsigned char *)view->data;
  atomic_store_explicit(&probe->received, 1, memory_order_release);
}

static void cnet_api_test_owned_state(void *user, cnet_connection connection,
                                      cnet_connection_state state,
                                      const cnet_error *error) {
  cnet_api_test_owned_receive_probe *probe =
      (cnet_api_test_owned_receive_probe *)user;
  (void)connection;
  if (state == CNET_CONNECTION_CONNECTED) {
    atomic_store_explicit(&probe->connected, 1, memory_order_release);
  } else if (state == CNET_CONNECTION_CLOSED ||
             state == CNET_CONNECTION_FAILED) {
    if (state == CNET_CONNECTION_FAILED || error != NULL)
      atomic_store_explicit(&probe->failed, 1, memory_order_release);
    atomic_store_explicit(&probe->terminal, 1, memory_order_release);
  }
}

static void cnet_api_test_owned_borrowed_receive(
    void *user, cnet_connection connection, const cnet_receive_view *view) {
  cnet_api_test_owned_receive_probe *probe =
      (cnet_api_test_owned_receive_probe *)user;
  (void)connection;
  if (view == NULL || view->kind != CNET_MESSAGE_BYTES || view->size != 1u)
    atomic_store_explicit(&probe->failed, 1, memory_order_release);
  else
    probe->borrowed_value = *(const unsigned char *)view->data;
  atomic_fetch_add_explicit(&probe->borrowed_received, 1, memory_order_release);
}

static void cnet_api_test_owned_receive(void *user,
                                        cnet_connection connection,
                                        mem_slice_t slice,
                                        cnet_message_kind kind) {
  cnet_api_test_owned_receive_probe *probe =
      (cnet_api_test_owned_receive_probe *)user;
  (void)connection;
  if (slice.buffer == NULL || slice.data == NULL || slice.length == 0u ||
      kind != CNET_MESSAGE_BYTES || probe->owned_slice.buffer != NULL) {
    atomic_store_explicit(&probe->failed, 1, memory_order_release);
    mem_slice_release(&slice);
  } else {
    probe->owned_slice = slice;
    probe->owned_kind = kind;
  }
  atomic_fetch_add_explicit(&probe->owned_received, 1, memory_order_release);
}

static void cnet_api_test_listener_send(void *user, cnet_connection connection, size_t size) {
  cnet_api_test_listener_probe *probe = (cnet_api_test_listener_probe *)user;
  const size_t expected_size = probe->expected_send_size != 0u ? probe->expected_send_size : 1u;
  (void)connection;
  if (size != expected_size) atomic_store_explicit(&probe->failed, 1, memory_order_release);
  atomic_fetch_add_explicit(&probe->sent, 1, memory_order_release);
}

static cnet_client_config cnet_api_test_config(void) {
  const cnet_client_config config = {.backend =
#if defined(_WIN32)
                                         NATIVE_IO_BACKEND_IOCP,
#elif defined(__linux__)
                                         NATIVE_IO_BACKEND_EPOLL,
#else
                                         NATIVE_IO_BACKEND_KQUEUE,
#endif
                                     .connection_capacity = 2u,
                                     .command_capacity = 8u,
                                     .request_capacity = 4u,
                                     .completion_batch_capacity = 4u,
                                     .event_capacity = 8u,
                                     .max_send_bytes = 256u,
                                     .receive_buffer_bytes = 256u};
  return config;
}

spec("CNet public client API") {
  it("validates typed stream socket tuning before admission") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_stream_socket_options options = CNET_STREAM_SOCKET_OPTIONS_INIT;

    check_equal(cnet_stream_socket_options_validate(&options), SALTS_OK);
    options.size = 0u;
    check_equal(cnet_stream_socket_options_validate(&options), SALTS_EINVAL);
    options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    options.receive_buffer_bytes = (size_t)INT_MAX + 1u;
    check_equal(cnet_stream_socket_options_validate(&options), SALTS_ERANGE);
    options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    options.keepalive_idle_ms = 1000u;
    check_equal(cnet_stream_socket_options_validate(&options), SALTS_EINVAL);
    options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    options.linger_ms = 1000u;
    check_equal(cnet_stream_socket_options_validate(&options), SALTS_EINVAL);
    options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    options.nodelay = 2;
    check_equal(cnet_stream_socket_options_validate(&options), SALTS_EINVAL);

    options = (cnet_stream_socket_options)CNET_STREAM_SOCKET_OPTIONS_INIT;
    options.receive_buffer_bytes = 32768u;
    options.send_buffer_bytes = 32768u;
    options.keepalive = 1;
    options.linger = 1;
    options.nodelay = 1;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_set_stream_socket_options(&client, &options), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("validates versioned listener tuning") {
    cnet_listener listener = {0};
    cnet_client_config client_config = cnet_api_test_config();
    cnet_listener_config config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_listener_options options = CNET_LISTENER_OPTIONS_INIT;

    check_equal(cnet_listener_options_validate(&options), SALTS_OK);
    options.size = 0u;
    check_equal(cnet_listener_options_validate(&options), SALTS_EINVAL);
    options = (cnet_listener_options)CNET_LISTENER_OPTIONS_INIT;
    options.reuse_port = 2;
    check_equal(cnet_listener_options_validate(&options), SALTS_EINVAL);
    options = (cnet_listener_options)CNET_LISTENER_OPTIONS_INIT;
    check_equal(cnet_listener_init_ex(&listener, &config, &options), SALTS_OK);
    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
  }

  it("validates additive VSOCK listener configuration and rejects TCP listener misuse") {
    cnet_listener listener = {0};
    cnet_client client = {0};
    cnet_client_config client_config = cnet_api_test_config();
    cnet_listener_config tcp_config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_vsock_listener_config vsock_config = CNET_VSOCK_LISTENER_CONFIG_INIT;
    cnet_vsock_peer local = {17u, 19u};
    cnet_vsock_peer peer = {17u, 19u};
    cnet_connection connection = {17u, 19u};
    cnet_observer observer = {.on_state = cnet_api_test_ignore_state};

    vsock_config.backend = client_config.backend;
    vsock_config.cid = CNET_VSOCK_CID_ANY;
    vsock_config.port = CNET_VSOCK_PORT_ANY;
    vsock_config.backlog = 2u;
    check_equal(cnet_listener_init_vsock(NULL, &vsock_config), SALTS_EINVAL);
    check_equal(cnet_listener_init_vsock(&listener, NULL), SALTS_EINVAL);
    vsock_config.size = 0u;
    check_equal(cnet_listener_init_vsock(&listener, &vsock_config), SALTS_EINVAL);
    check_null(listener.impl);
    vsock_config = (cnet_vsock_listener_config)CNET_VSOCK_LISTENER_CONFIG_INIT;
    vsock_config.backend = client_config.backend;
    vsock_config.backlog = 0u;
    check_equal(cnet_listener_init_vsock(&listener, &vsock_config), SALTS_EINVAL);
    vsock_config.backlog = (size_t)INT_MAX + 1u;
    check_equal(cnet_listener_init_vsock(&listener, &vsock_config), SALTS_EINVAL);
    vsock_config.backlog = 2u;
    vsock_config.backend = (native_io_backend_kind)0;
    check_equal(cnet_listener_init_vsock(&listener, &vsock_config), SALTS_EINVAL);

#if !defined(__linux__)
    vsock_config.backend = client_config.backend;
    check_equal(cnet_listener_init_vsock(&listener, &vsock_config), SALTS_ENOTSUP);
    check_null(listener.impl);
#endif

    check_equal(cnet_listener_init(&listener, &tcp_config), SALTS_OK);
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    check_equal(cnet_listener_accept_vsock_peer(&listener, &client, &observer, NULL, &peer),
                SALTS_EINVAL);
    check_equal(peer.cid, 0u);
    check_equal(peer.port, 0u);
    peer = (cnet_vsock_peer){17u, 19u};
    check_equal(cnet_listener_vsock_local(&listener, &local), SALTS_ENOTSUP);
    check_equal(local.cid, 0u);
    check_equal(local.port, 0u);
    check_equal(cnet_listener_accept_vsock_peer(&listener, &client, &observer, &connection, &peer),
                SALTS_ENOTSUP);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
    check_equal(peer.cid, 0u);
    check_equal(peer.port, 0u);
    check_equal(cnet_listener_accept_vsock(&listener, &client, &observer, &connection),
                SALTS_ENOTSUP);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("validates reusable TLS client profile lifecycle") {
    cnet_tls_client client = {0};
    cnet_tls_client_config config = {.size = sizeof(config)};
    cnet_tls_client_config invalid = {0};

    check_equal(cnet_tls_client_init(NULL, &config), SALTS_EINVAL);
    check_equal(cnet_tls_client_init(&client, NULL), SALTS_EINVAL);
    check_equal(cnet_tls_client_init(&client, &invalid), SALTS_EINVAL);
    check_null(client.impl);
    check_equal(cnet_tls_client_init(&client, &config), SALTS_OK);
    check_not_null(client.impl);
    check_equal(cnet_tls_client_init(&client, &config), SALTS_EALREADY);
    check_equal(cnet_tls_client_destroy(NULL), SALTS_EINVAL);
    check_equal(cnet_tls_client_destroy(&client), SALTS_OK);
    check_null(client.impl);
    check_equal(cnet_tls_client_destroy(&client), SALTS_OK);
  }

  it("rejects ambiguous or plaintext reusable TLS profiles") {
    cnet_client client = {0};
    cnet_tls_client tls_client = {0};
    cnet_tls_client_config tls_config = {.size = sizeof(tls_config)};
    cnet_client_config config = cnet_api_test_config();
    cnet_connection connection = {17u, 19u};
    cnet_connect_options options = {.uri = "tcp://127.0.0.1:443",
                                    .observer = {.on_state = cnet_api_test_ignore_state},
                                    .tls_client = &tls_client};

    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.tls_handshake_timeout_ms = 1000u;
    check_equal(cnet_tls_client_init(&tls_client, &tls_config), SALTS_OK);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_connect(&client, &options, &connection), SALTS_EINVAL);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);

    options.uri = "tls://127.0.0.1:443";
    options.tls = &tls_config;
    check_equal(cnet_connect(&client, &options, &connection), SALTS_EINVAL);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);

    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(cnet_tls_client_destroy(&tls_client), SALTS_OK);
  }

  it("rejects malformed TLS server configuration without publishing a context") {
    cnet_tls_server server = {0};
    cnet_tls_server_config config = {.size = sizeof(config)};

    check_equal(cnet_tls_server_init(NULL, &config), SALTS_EINVAL);
    check_equal(cnet_tls_server_init(&server, NULL), SALTS_EINVAL);
    check_equal(cnet_tls_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);
    check_equal(cnet_tls_server_destroy(&server), SALTS_OK);
  }

  it("keeps TLS disabled when bounded TLS storage is not configured") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_connection connection = {17u, 19u};
    cnet_connect_options options = {.uri = "tls://localhost:443",
                                    .observer = {.on_state = cnet_api_test_ignore_state}};
    cnet_start_tls_options upgrade = CNET_START_TLS_OPTIONS_INIT;

    upgrade.server_name = "localhost";

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_start_tls(&client, connection, &upgrade), SALTS_ENOTSUP);
    check_equal(cnet_connect(&client, &options, &connection), SALTS_ENOTSUP);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("requires a complete bounded TLS client allocation policy") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();

    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    check_equal(cnet_client_init(&client, &config), SALTS_EINVAL);
    check_null(client.impl);

    config.tls_handshake_timeout_ms = 1000u;
    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES - 1u;
    check_equal(cnet_client_init(&client, &config), SALTS_EINVAL);
    check_null(client.impl);

    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("admits TLS upgrade only for a quiescent connected TCP stream") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket peer = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options connect_options;
    cnet_start_tls_options tls_options = CNET_START_TLS_OPTIONS_INIT;
    cnet_tls_client_config tls_config = {.size = sizeof(tls_config), .server_name = "localhost"};
    char uri[64];
    uint16_t port = 0u;
    const unsigned char value = 29u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.tls_handshake_timeout_ms = 20u;
    tls_options.tls = &tls_config;

    check_equal(cnet_start_tls(NULL, connection, &tls_options), SALTS_EINVAL);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    connect_options =
        (cnet_connect_options){.uri = uri,
                               .observer = {.on_state = cnet_api_test_listener_state,
                                            .on_receive = cnet_api_test_listener_receive,
                                            .on_send = cnet_api_test_listener_send,
                                            .user = &probe}};
    check_equal(cnet_connect(&client, &connect_options, &connection), SALTS_OK);
    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_EBUSY);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    peer = accept(listener, NULL, NULL);
    check_true(peer != CNET_API_TEST_INVALID_SOCKET);

    tls_options.size = 0u;
    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_EINVAL);
    tls_options = (cnet_start_tls_options)CNET_START_TLS_OPTIONS_INIT;
    tls_options.tls = &tls_config;

    check_equal(cnet_receive(&client, connection, 1u), SALTS_OK);
    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_EBUSY);
    check_equal(send(peer, (const char *)&value, (int)sizeof(value), 0), (int)sizeof(value));
    check_equal(cnet_api_test_poll_until(&client, &probe.received, 1), SALTS_OK);

    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_OK);
    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_EBUSY);
    check_equal(cnet_api_test_poll_until(&client, &probe.sent, 1), SALTS_OK);

    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_OK);
    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_EBUSY);
    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_EBUSY);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_EBUSY);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 1);
    check_equal(probe.handshaking, 1);
    check_equal(probe.failure_status, SALTS_ETIMEDOUT);
    check_not_null(probe.failure_stage);
    check_equal(strcmp(probe.failure_stage, "handshake"), 0);

    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(peer);
    cnet_api_test_close_socket(listener);
  }

  it("returns a portable address-in-use error for listener bind conflicts") {
    cnet_listener first = {0};
    cnet_listener second = {0};
    cnet_client_config client_config = cnet_api_test_config();
    cnet_listener_config config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    uint16_t port = 0u;

    check_equal(cnet_listener_init(&first, &config), SALTS_OK);
    check_equal(cnet_listener_port(&first, &port), SALTS_OK);
    config.port = port;
    check_equal(cnet_listener_init(&second, &config), SALTS_EADDRINUSE);
    check_null(second.impl);
    check_equal(cnet_listener_close(&first), SALTS_OK);
    check_equal(cnet_listener_destroy(&first), SALTS_OK);
  }

#if !defined(_WIN32)
  it("retries an interrupted listener wait against the original deadline") {
    cnet_listener listener = {0};
    cnet_client_config client_config = cnet_api_test_config();
    cnet_listener_config config = {
        .backend = client_config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_api_test_interrupt_probe probe;
    cmeta_thread_t interrupter = {0};
    void (*previous_handler)(int);
    int ready = -1;

    atomic_init(&probe.armed, 0);
    atomic_init(&probe.signal_status, -1);
    probe.target = pthread_self();
    previous_handler = signal(SIGUSR1, cnet_api_test_signal_handler);
    check_true(previous_handler != SIG_ERR);
    check_equal(cnet_listener_init(&listener, &config), SALTS_OK);
    check_equal(cmeta_thread_create(&interrupter, cnet_api_test_interrupt_wait, &probe), SALTS_OK);
    atomic_store_explicit(&probe.armed, 1, memory_order_release);
    check_equal(cnet_listener_wait(&listener, 100u, &ready), SALTS_OK);
    check_equal(ready, 0);
    check_equal(cmeta_thread_join(&interrupter), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.signal_status, memory_order_acquire), 0);
    check_true(signal(SIGUSR1, previous_handler) != SIG_ERR);
    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
  }
#endif

  it("moves one detached accepted stream into exactly one final owner") {
    cnet_client client = {0};
    cnet_listener listener = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_stream_socket_options stream_options = CNET_STREAM_SOCKET_OPTIONS_INIT;
    cnet_listener_config listener_config = {
        .backend = config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket peer = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket peer_closed = CNET_API_TEST_INVALID_SOCKET;
    struct sockaddr_in address;
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    cnet_accepted_stream closed = CNET_ACCEPTED_STREAM_INIT;
    cnet_connection connection = {0};
    cnet_connection duplicate = {17u, 19u};
    cnet_observer observer = {.on_state = cnet_api_test_listener_state,
                              .on_receive = cnet_api_test_listener_receive,
                              .on_send = cnet_api_test_listener_send,
                              .user = &probe};
    uint16_t port = 0u;
    uint64_t nodelay = 0u;
    int ready = 0;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);

    check_equal(cnet_accepted_stream_close(&accepted), SALTS_EALREADY);
    check_equal(cnet_client_adopt_accepted(&client, &accepted, &observer, &duplicate),
                SALTS_EALREADY);
    check_equal(duplicate.slot, 0u);
    check_equal(duplicate.generation, 0u);

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    stream_options.nodelay = 1;
    check_equal(cnet_client_set_stream_socket_options(&client, &stream_options),
                SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_true(port != 0u);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check_true(peer != CNET_API_TEST_INVALID_SOCKET);
    check_equal(connect(peer, (const struct sockaddr *)&address,
                        (int)sizeof(address)),
                0);
    check_equal(cnet_listener_wait(&listener, CNET_API_TEST_TIMEOUT_MS, &ready),
                SALTS_OK);
    check_equal(ready, 1);
    check_equal(cnet_listener_accept_detached(&listener, &accepted), SALTS_OK);
    check_equal(accepted.peer.family, CNET_DATAGRAM_ADDRESS_IPV4);
    check_equal(accepted.peer.address[0], (uint8_t)127u);
    check_true(accepted.peer.port != 0u);

    /* A live move token cannot be overwritten by another accept. */
    check_equal(cnet_listener_accept_detached(&listener, &accepted), SALTS_EALREADY);

    check_equal(cnet_client_adopt_accepted(&client, &accepted, &observer, &connection),
                SALTS_OK);
    check_equal(cnet_accepted_stream_close(&accepted), SALTS_EALREADY);
    duplicate = (cnet_connection){17u, 19u};
    check_equal(cnet_client_adopt_accepted(&client, &accepted, &observer, &duplicate),
                SALTS_EALREADY);
    check_equal(duplicate.slot, 0u);
    check_equal(duplicate.generation, 0u);
    /* Copied peer metadata survives consumption of the socket owner. */
    check_equal(accepted.peer.family, CNET_DATAGRAM_ADDRESS_IPV4);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    check_equal(cnet_connection_tcp_option_get(
                    &client, connection, CNET_TCP_SOCKET_NODELAY, &nodelay),
                SALTS_OK);
    check_equal(nodelay, UINT64_C(1));

    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);

    peer_closed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check_true(peer_closed != CNET_API_TEST_INVALID_SOCKET);
    check_equal(connect(peer_closed, (const struct sockaddr *)&address,
                        (int)sizeof(address)),
                0);
    ready = 0;
    check_equal(cnet_listener_wait(&listener, CNET_API_TEST_TIMEOUT_MS, &ready),
                SALTS_OK);
    check_equal(ready, 1);
    check_equal(cnet_listener_accept_detached(&listener, &closed), SALTS_OK);
    check_equal(cnet_accepted_stream_close(&closed), SALTS_OK);
    check_equal(cnet_accepted_stream_close(&closed), SALTS_EALREADY);
    check_equal(closed.peer.family, CNET_DATAGRAM_ADDRESS_IPV4);

    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(peer_closed);
    cnet_api_test_close_socket(peer);
  }

  it("accepts a TCP connection and closes only after the final retained send") {
    cnet_client client = {0};
    cnet_listener listener = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_listener_config listener_config = {
        .backend = config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket peer = CNET_API_TEST_INVALID_SOCKET;
    struct sockaddr_in address;
    cnet_connection connection = {0};
    cnet_stream_peer stream_peer = {0};
    cnet_observer observer = {.on_state = cnet_api_test_listener_state,
                              .on_receive = cnet_api_test_listener_receive,
                              .on_send = cnet_api_test_listener_send,
                              .user = &probe};
    uint16_t port = 0u;
    unsigned char parsed_address[128];
    size_t parsed_address_length = 0u;
    int ready = 0;
    unsigned char received = 0u;
    char peer_certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY] = {0};
    uint8_t server_end_point[CNET_TLS_SERVER_END_POINT_MAX_BYTES] = {0};
    size_t server_end_point_size = 0u;
    const unsigned char request_value = 31u;
    const unsigned char response_value = 47u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    probe.received_value = 0u;

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_true(native_io_backend_kind_supported(listener_config.backend));
    check_equal(cnet_transport_parse_bind_address(listener_config.host, listener_config.port,
                                                  parsed_address, sizeof(parsed_address),
                                                  &parsed_address_length),
                SALTS_OK);
    check_true(parsed_address_length != 0u);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);
    check_true(port != 0u);

    peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check_true(peer != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(peer), SALTS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    check_equal(connect(peer, (const struct sockaddr *)&address, (int)sizeof(address)), 0);

    check_equal(cnet_listener_wait(&listener, CNET_API_TEST_TIMEOUT_MS, &ready), SALTS_OK);
    check_equal(ready, 1);
    check_equal(cnet_listener_accept_peer(&listener, &client, &observer, &connection, &stream_peer),
                SALTS_OK);
    {
      cnet_connection no_peer = {0};
      check_equal(cnet_listener_accept(&listener, &client, &observer, &no_peer), SALTS_ETIMEDOUT);
      check_equal(no_peer.slot, 0u);
      check_equal(no_peer.generation, 0u);
    }
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    check_equal(stream_peer.family, CNET_DATAGRAM_ADDRESS_IPV4);
    check_equal(stream_peer.address[0], (uint8_t)127u);
    check_equal(stream_peer.address[1], (uint8_t)0u);
    check_equal(stream_peer.address[2], (uint8_t)0u);
    check_equal(stream_peer.address[3], (uint8_t)1u);
    check_true(stream_peer.port != 0u);
    check_equal(cnet_tls_peer_certificate_sha256(&client, connection, peer_certificate_sha256),
                SALTS_ENOTSUP);
    check_equal(cnet_tls_server_end_point_binding(
                    &client, connection, server_end_point,
                    sizeof(server_end_point), &server_end_point_size),
                SALTS_ENOTSUP);
    check_equal(server_end_point_size, (size_t)0u);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_OK);
    check_equal(send(peer, (const char *)&request_value, (int)sizeof(request_value), 0),
                (int)sizeof(request_value));
    check_equal(cnet_api_test_poll_until(&client, &probe.received, 1), SALTS_OK);
    check_equal(probe.received_value, request_value);

    check_equal(cnet_api_test_send_bytes_and_close(&client, connection, &response_value, sizeof(response_value)),
                SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &response_value, sizeof(response_value)),
                SALTS_EBUSY);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_EBUSY);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(recv(peer, (char *)&received, (int)sizeof(received), 0), (int)sizeof(received));
    check_equal(received, response_value);
    check_equal(recv(peer, (char *)&received, (int)sizeof(received), 0), 0);
    check_equal(atomic_load_explicit(&probe.sent, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);

    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(peer);
  }

  it("keeps an owned receive slice valid across later progress and client destroy") {
    static const unsigned char owned_payload[] =
        "owned-receive-survives-client-destroy";
    static const unsigned char borrowed_payload = 0x5du;
    cnet_client client = {0};
    cnet_listener listener = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_listener_config listener_config = {
        .backend = config.backend, .host = "127.0.0.1", .port = 0u, .backlog = 2u};
    cnet_api_test_owned_receive_probe probe = {0};
    cnet_api_test_socket peer = CNET_API_TEST_INVALID_SOCKET;
    struct sockaddr_in address;
    cnet_connection connection = {0};
    cnet_observer observer = {.on_state = cnet_api_test_owned_state,
                              .on_receive = cnet_api_test_owned_borrowed_receive,
                              .user = &probe};
    uint16_t port = 0u;
    int ready = 0;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.owned_received, 0);
    atomic_init(&probe.borrowed_received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);

    check_equal(cnet_set_receive_slice_handler(NULL, connection,
                                               cnet_api_test_owned_receive,
                                               &probe),
                SALTS_EINVAL);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_listener_init(&listener, &listener_config), SALTS_OK);
    check_equal(cnet_listener_port(&listener, &port), SALTS_OK);

    peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check_true(peer != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(peer), SALTS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    check_equal(connect(peer, (const struct sockaddr *)&address,
                        (int)sizeof(address)),
                0);

    check_equal(cnet_listener_wait(&listener, CNET_API_TEST_TIMEOUT_MS, &ready),
                SALTS_OK);
    check_equal(ready, 1);
    check_equal(cnet_listener_accept(&listener, &client, &observer, &connection),
                SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);

    check_equal(cnet_set_receive_slice_handler(&client, connection,
                                               cnet_api_test_owned_receive,
                                               &probe),
                SALTS_OK);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_OK);
    /* Already-admitted demand cannot change lifetime contract in flight. */
    check_equal(cnet_set_receive_slice_handler(&client, connection, NULL, NULL),
                SALTS_EBUSY);

    check_equal(send(peer, (const char *)owned_payload,
                     (int)(sizeof(owned_payload) - 1u), 0),
                (int)(sizeof(owned_payload) - 1u));
    check_equal(cnet_api_test_poll_until(&client, &probe.owned_received, 1),
                SALTS_OK);
    check_equal(atomic_load_explicit(&probe.borrowed_received,
                                     memory_order_acquire),
                0);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_not_null(probe.owned_slice.buffer);
    check_not_null(probe.owned_slice.data);
    check_equal(probe.owned_slice.length, sizeof(owned_payload) - 1u);
    check_equal(probe.owned_kind, CNET_MESSAGE_BYTES);
    check_true(mem_buffer_pool(probe.owned_slice.buffer) == mem_global());
    check_equal(memcmp(probe.owned_slice.data, owned_payload,
                       probe.owned_slice.length),
                0);

    /*
     * Clear owned delivery only after demand settles; the next receive returns
     * to the original borrowed callback without disturbing the held owner.
     */
    check_equal(cnet_set_receive_slice_handler(&client, connection, NULL, NULL),
                SALTS_OK);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_OK);
    check_equal(send(peer, (const char *)&borrowed_payload,
                     (int)sizeof(borrowed_payload), 0),
                (int)sizeof(borrowed_payload));
    check_equal(cnet_api_test_poll_until(&client, &probe.borrowed_received, 1),
                SALTS_OK);
    check_equal(probe.borrowed_value, borrowed_payload);
    check_equal(memcmp(probe.owned_slice.data, owned_payload,
                       probe.owned_slice.length),
                0);

    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(memcmp(probe.owned_slice.data, owned_payload,
                       probe.owned_slice.length),
                0);

    check_equal(cnet_listener_close(&listener), SALTS_OK);
    check_equal(cnet_listener_destroy(&listener), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);

    /* The handed-off global-pool owner is independent of client lifetime. */
    check_equal(memcmp(probe.owned_slice.data, owned_payload,
                       probe.owned_slice.length),
                0);
    mem_slice_release(&probe.owned_slice);
    check_null(probe.owned_slice.buffer);
    cnet_api_test_close_socket(peer);
  }

  it("drains accepted non-TLS writes before the final send-and-close slot") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {.expected_send_size = 1u};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    const unsigned char first = 0x21u;
    const unsigned char second = 0x22u;
    const unsigned char final_value = 0x23u;
    unsigned char received[3] = {0};
    size_t received_size = 0u;
    char uri[64];
    uint16_t port = 0u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .on_receive = cnet_api_test_listener_receive,
                                                  .on_send = cnet_api_test_listener_send,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(accepted), SALTS_OK);

    check_equal(cnet_api_test_send_bytes(&client, connection, &first, sizeof(first)), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &second, sizeof(second)), SALTS_OK);
    check_equal(cnet_api_test_send_bytes_and_close(&client, connection, &final_value, sizeof(final_value)),
                SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &first, sizeof(first)), SALTS_EBUSY);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_EBUSY);

    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.sent, memory_order_acquire), 3);
    while (received_size < sizeof(received)) {
      const int got =
          recv(accepted, (char *)&received[received_size], (int)(sizeof(received) - received_size), 0);
      check_greater(got, 0);
      received_size += (size_t)got;
    }
    check_equal(received[0], first);
    check_equal(received[1], second);
    check_equal(received[2], final_value);
    {
      unsigned char eof_byte = 0u;
      check_equal(recv(accepted, (char *)&eof_byte, (int)sizeof(eof_byte), 0), 0);
    }
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("rejects new work as soon as close is admitted") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket peer = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options;
    cnet_start_tls_options tls_options = CNET_START_TLS_OPTIONS_INIT;
    char uri[64];
    uint16_t port = 0u;
    const unsigned char value = 37u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    probe.received_value = 0u;
    config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
    config.tls_handshake_timeout_ms = 1000u;
    tls_options.server_name = "localhost";
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .on_receive = cnet_api_test_listener_receive,
                                                  .on_send = cnet_api_test_listener_send,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    peer = accept(listener, NULL, NULL);
    check_true(peer != CNET_API_TEST_INVALID_SOCKET);

    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_close(&client, connection), SALTS_EALREADY);
    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_EBUSY);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_EBUSY);
    check_equal(cnet_start_tls(&client, connection, &tls_options), SALTS_EBUSY);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);

    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(peer);
    cnet_api_test_close_socket(listener);
  }

  it("returns zero progress when the caller polls an idle client") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    size_t events = SIZE_MAX;

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    check_equal(events, (size_t)0u);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

#if defined(CNET_INTERNAL_PROFILING)
  it("collects internal poll stages only during an explicit diagnostic sample") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_client_poll_profile profile = {0};
    cnet_owner_trace_event trace_events[2] = {{0}};
    size_t events = SIZE_MAX;

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_profile_take(&client, &profile), SALTS_EBUSY);
    check_equal(cnet_client_profile_trace_bind(&client, trace_events, 2u),
                SALTS_EBUSY);
    check_equal(cnet_client_profile_begin(&client), SALTS_OK);
    check_equal(cnet_client_profile_trace_bind(&client, NULL, 2u),
                SALTS_EINVAL);
    check_equal(cnet_client_profile_trace_bind(&client, trace_events, 0u),
                SALTS_EINVAL);
    check_equal(cnet_client_profile_trace_bind(&client, trace_events, 2u),
                SALTS_OK);
    check_equal(cnet_client_profile_trace_bind(&client, trace_events, 2u),
                SALTS_EBUSY);
    check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    check_equal(events, (size_t)0u);
    check_equal(cnet_client_profile_take(&client, &profile), SALTS_OK);
    check_equal(profile.owner.owner_drive_calls, (uint64_t)1u);
    check_equal(profile.owner.receive_rearm_stage_calls, (uint64_t)1u);
    check_equal(profile.owner.receive_rearm_request_lifecycle_calls, (uint64_t)0u);
    check_equal(profile.owner.command_stage_calls, (uint64_t)1u);
    check_equal(profile.owner.command_request_lifecycle_calls, (uint64_t)0u);
    check_equal(profile.owner.request_lifecycle_calls, (uint64_t)0u);
    check_equal(profile.owner.request_start_calls, (uint64_t)0u);
    check_equal(profile.owner.request_resubmit_calls, (uint64_t)0u);
    check_equal(profile.owner.observe_calls, (uint64_t)1u);
    check_equal(profile.owner.request_completion_calls, (uint64_t)0u);
    check_equal(profile.owner.event_publish_calls, (uint64_t)0u);
    check_equal(profile.client_poll_calls, (uint64_t)1u);
    check_true(profile.client_poll_ns >= profile.owner.owner_drive_ns);
    check_equal(profile.dispatcher_prepare_calls, (uint64_t)0u);
    check_equal(profile.dispatcher_invoke_calls, (uint64_t)0u);
    check_equal(profile.dispatcher_observer_calls, (uint64_t)0u);
    check_equal(profile.dispatcher_release_calls, (uint64_t)0u);
    check_true(profile.owner.owner_drive_ns >= profile.owner.receive_rearm_stage_ns);
    check_equal(profile.owner.receive_rearm_request_lifecycle_ns, (uint64_t)0u);
    check_equal(profile.owner.command_request_lifecycle_ns, (uint64_t)0u);
    check_equal(profile.owner.request_resubmit_ns, (uint64_t)0u);
    check_equal(profile.owner.trace_event_count, (uint64_t)0u);
    check_equal(profile.owner.trace_dropped, (uint64_t)0u);
    check_true(profile.owner.owner_drive_ns >= profile.owner.command_stage_ns);
    check_true(profile.owner.owner_drive_ns >= profile.owner.request_lifecycle_ns);
    check_true(profile.owner.owner_drive_ns >= profile.owner.request_start_ns);
    check_true(profile.owner.owner_drive_ns >= profile.owner.observe_ns);
    check_equal(cnet_client_profile_take(&client, &profile), SALTS_EBUSY);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("bypasses the command queue for owner-issued receive admission") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    cnet_client_poll_profile profile = {0};
    char uri[64];
    uint16_t port = 0u;
    const unsigned char value = 61u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .on_receive = cnet_api_test_listener_receive,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);

    check_equal(cnet_client_profile_begin(&client), SALTS_OK);
    check_equal(cnet_receive(&client, connection, 1u), SALTS_OK);
    check_equal(cnet_client_profile_take(&client, &profile), SALTS_OK);
    check_equal(profile.owner.command_queue_publish_calls, (uint64_t)0u);
    check_equal(profile.owner.command_queue_payload_publish_calls, (uint64_t)0u);
    check_equal(profile.owner.command_queue_payload_copy_calls, (uint64_t)0u);
    check_equal(profile.owner.owner_drive_calls, (uint64_t)0u);
    check_equal(profile.client_poll_calls, (uint64_t)0u);

    check_equal(send(accepted, (const char *)&value, (int)sizeof(value), 0), (int)sizeof(value));
    check_equal(cnet_api_test_poll_until(&client, &probe.received, 1), SALTS_OK);
    check_equal(probe.received_value, value);
    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("bypasses the command queue for owner-issued non-TLS send admission") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    cnet_client_poll_profile profile = {0};
    char uri[64];
    uint16_t port = 0u;
    unsigned char value = 0x6du;
    unsigned char received = 0u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    probe.expected_send_size = 1u;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .on_send = cnet_api_test_listener_send,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(accepted), SALTS_OK);

    check_equal(cnet_client_profile_begin(&client), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_OK);
    value = 0u;
    check_equal(atomic_load_explicit(&probe.sent, memory_order_acquire), 0);
    check_equal(cnet_client_profile_take(&client, &profile), SALTS_OK);
    check_equal(profile.owner.command_queue_publish_calls, (uint64_t)0u);
    check_equal(profile.owner.command_queue_payload_publish_calls, (uint64_t)0u);
    check_equal(profile.owner.command_queue_payload_copy_calls, (uint64_t)0u);
    check_equal(profile.owner.owner_drive_calls, (uint64_t)0u);
    check_equal(profile.client_poll_calls, (uint64_t)0u);

    check_equal(cnet_api_test_poll_until(&client, &probe.sent, 1), SALTS_OK);
    check_equal(recv(accepted, (char *)&received, (int)sizeof(received), 0),
                (int)sizeof(received));
    check_equal(received, (unsigned char)0x6du);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("defers callbacks while bypassing the command queue for quiescent owner close") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {0};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    cnet_client_poll_profile profile = {0};
    char uri[64];
    uint16_t port = 0u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);

    check_equal(cnet_client_profile_begin(&client), SALTS_OK);
    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.terminal, memory_order_acquire), 0);
    check_equal(cnet_client_profile_take(&client, &profile), SALTS_OK);
    check_equal(profile.owner.command_queue_publish_calls, (uint64_t)0u);
    check_equal(profile.owner.owner_drive_calls, (uint64_t)0u);
    check_equal(profile.client_poll_calls, (uint64_t)0u);
    check_equal(cnet_close(&client, connection), SALTS_EALREADY);

    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }
#endif

  it("wakes an idle poll from another thread without publishing a callback") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_wake_probe probe = {.client = &client};
    cmeta_thread_t poller;

    atomic_init(&probe.started, 0);
    atomic_init(&probe.finished, 0);
    check_equal(cnet_client_wake(NULL), SALTS_EINVAL);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cmeta_thread_create(&poller, cnet_api_test_blocking_poll, &probe), SALTS_OK);
    while (atomic_load_explicit(&probe.started, memory_order_acquire) == 0)
      cmeta_thread_yield();
    cmeta_sleep_ms(20u);
    check_equal(cnet_client_wake(&client), SALTS_OK);
    check_equal(cmeta_thread_join(&poller), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.finished, memory_order_acquire), 1);
    check_equal(probe.status, SALTS_OK);
    check_equal(probe.events, (size_t)0u);
    check_less(probe.elapsed_ms, (uint64_t)900u);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_wake(&client), SALTS_ESHUTDOWN);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("wakes a poll with an outstanding receive without publishing a callback") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_probe state = {.client = &client};
    cnet_api_test_wake_probe probe = {.client = &client};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    cmeta_thread_t poller;
    char uri[64];
    uint16_t port = 0u;
    size_t events = 0u;

    atomic_init(&probe.started, 0);
    atomic_init(&probe.finished, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options.uri = uri;
    options.observer = (cnet_observer){.on_state = cnet_api_test_state,
                                     .on_receive = cnet_api_test_receive, .user = &state};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &state.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);
    check_equal(atomic_load(&state.callback_operation_status), SALTS_OK);
    check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    check_equal(cmeta_thread_create(&poller, cnet_api_test_blocking_poll, &probe), SALTS_OK);
    while (atomic_load_explicit(&probe.started, memory_order_acquire) == 0)
      cmeta_thread_yield();
    cmeta_sleep_ms(20u);
    check_equal(cnet_client_wake(&client), SALTS_OK);
    check_equal(cmeta_thread_join(&poller), SALTS_OK);
    check_equal(probe.status, SALTS_OK);
    check_equal(probe.events, 0u);
    check_less(probe.elapsed_ms, (uint64_t)900u);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("invokes connection callbacks on the polling thread") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_poll_probe probe = {.client = &client};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options;
    char uri[64];
    uint16_t port = 0u;

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    cnet_api_test_thread_marker = 73;
    options = (cnet_connect_options){
        .uri = uri, .observer = {.on_state = cnet_api_test_poll_state, .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.callback_thread_marker, memory_order_acquire), 73);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(listener);
  }

  it("initializes without a callback executor") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("does not reserve every command slot at the maximum send size") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();

    config.command_capacity = 256u;
    config.max_send_bytes = 256u * 1024u * 1024u;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("does not reserve every event slot or connection receive buffer at maximum size") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();

    config.connection_capacity = 128u;
    config.event_capacity = 256u;
    config.receive_buffer_bytes = 256u * 1024u * 1024u;
    config.event_buffer_bytes = config.receive_buffer_bytes;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("rejects invalid configuration without publishing a client") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();

    check_equal(cnet_client_init(NULL, &config), SALTS_EINVAL);
    check_equal(cnet_client_init(&client, NULL), SALTS_EINVAL);
    config.command_capacity = 0u;
    check_equal(cnet_client_init(&client, &config), SALTS_EINVAL);
    check_null(client.impl);

    config = cnet_api_test_config();
    config.command_buffer_bytes = 1u;
    check_equal(cnet_client_init(&client, &config), SALTS_EINVAL);
    check_null(client.impl);

    config = cnet_api_test_config();
    config.event_buffer_bytes = config.receive_buffer_bytes - 1u;
    check_equal(cnet_client_init(&client, &config), SALTS_EINVAL);
    check_null(client.impl);
  }

  it("decouples the control command byte budget from retained stream send size") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();

    config.max_send_bytes = 1024u * 1024u;
    config.command_buffer_bytes = 64u * 1024u;
    check_true(config.command_buffer_bytes < config.max_send_bytes);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_null(client.impl);
  }

  it("clears immediate connect failures and stops a quiescent client") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_connection connection = {17u, 19u};
    cnet_connect_options options = {.uri = "unknown://endpoint",
                                    .observer = {.on_state = cnet_api_test_ignore_state}};

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_not_null(client.impl);
    check_equal(cnet_connect(&client, &options, &connection), SALTS_ENOTSUP);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
    check_equal(cnet_client_destroy(&client), SALTS_EBUSY);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_null(client.impl);
  }

  group("network IP literal admission") {
    static cnet_client client;
    static size_t state_callbacks;

    before_each() {
      cnet_client_config config = cnet_api_test_config();
      config.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
      config.tls_handshake_timeout_ms = CNET_API_TEST_TIMEOUT_MS;
      client = (cnet_client){0};
      state_callbacks = 0u;
      check_equal(cnet_client_init(&client, &config), SALTS_OK);
    }

    after_each() {
      if (client.impl != NULL) {
        check_equal_warn(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
        check_equal_warn(cnet_client_destroy(&client), SALTS_OK);
      }
    }

    it("rejects bracketed names and IPv4 without publishing a connection or callback") {
      static const char *const inputs[] = {"tcp://[localhost]:80", "tls://[example.com]:443",
                                           "udp://[127.0.0.1]:9000"};
      cnet_connect_options options = {
          .observer = {.on_state = cnet_api_test_count_state, .user = &state_callbacks}};
      for (size_t index = 0u; index < sizeof(inputs) / sizeof(inputs[0]); ++index) {
        cnet_connection connection = {17u, 19u};
        size_t events = 1u;
        options.uri = inputs[index];
        check_equal(cnet_connect(&client, &options, &connection), SALTS_EINVAL);
        check_equal(connection.slot, 0u);
        check_equal(connection.generation, 0u);
        check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
        check_equal(events, 0u);
        check_equal(state_callbacks, 0u);
      }
    }

    it("rejects IPvFuture without publishing a connection or callback") {
      static const char *const inputs[] = {"tcp://[v1.localhost]:80", "tls://[vf.example]:443",
                                           "udp://[v1.loopback]:9000", "tcp://[V1.localhost]:80"};
      cnet_connect_options options = {
          .observer = {.on_state = cnet_api_test_count_state, .user = &state_callbacks}};
      for (size_t index = 0u; index < sizeof(inputs) / sizeof(inputs[0]); ++index) {
        cnet_connection connection = {17u, 19u};
        size_t events = 1u;
        options.uri = inputs[index];
        check_equal(cnet_connect(&client, &options, &connection), SALTS_ENOTSUP);
        check_equal(connection.slot, 0u);
        check_equal(connection.generation, 0u);
        check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
        check_equal(events, 0u);
        check_equal(state_callbacks, 0u);
      }
    }
  }

  it("rejects unsupported VSOCK without publishing a connection") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_connection connection = {17u, 19u};
    cnet_connect_options options = {.uri = "vsock://2:5000",
                                    .observer = {.on_state = cnet_api_test_ignore_state}};

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
#if defined(__linux__)
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_true(connection.slot != 0u);
    check_equal(cnet_close(&client, connection), SALTS_OK);
#else
    cnet_api_test_socket adopted = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check_equal(cnet_connect(&client, &options, &connection), SALTS_ENOTSUP);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
    check_true(adopted != CNET_API_TEST_INVALID_SOCKET);
    connection = (cnet_connection){17u, 19u};
    check_equal(cnet_client_adopt_vsock(&client, (uintptr_t)adopted, &options.observer,
                                       &connection),
                SALTS_ENOTSUP);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
  #if defined(_WIN32)
    check_equal(closesocket(adopted), SOCKET_ERROR);
    check_equal(WSAGetLastError(), WSAENOTSOCK);
  #else
    errno = 0;
    check_equal(close(adopted), -1);
    check_equal(errno, EBADF);
  #endif
#endif
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

#if defined(__linux__)
  it("drains an admitted VSOCK using client stop without explicit close") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_batch_probe probe = {0};
    cnet_connection connection = {17u, 19u};
    cnet_connect_options options = {
        .uri = "vsock://2:5000",
        .observer = {.on_state = cnet_api_test_batch_state, .user = &probe}};

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    /* No cnet_client_poll and NO user cnet_close: dispatcher drain must
     * create its own CLOSE and settle the real epoll AF_VSOCK connect,
     * including its generation-checked NativeIO cancel/completion. */
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_true(connection.slot != 0u);
    check_true(connection.generation != 0u);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.terminal, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&probe.received, memory_order_acquire), 0);
    /* The kernel may reject an unrouteable VSOCK peer first (FAILED), or
     * the canonical client close may win (CLOSED). Exactly one terminal
     * must be published and all NativeIO ownership retired either way. */
    check_true(atomic_load_explicit(&probe.failed, memory_order_acquire) <= 1);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }
#endif

  it("rejects TLS policy for a plaintext VSOCK URI") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_connection connection = {17u, 19u};
    const cnet_tls_client_config tls = {.size = sizeof(tls)};
    cnet_connect_options options = {.uri = "vsock://2:5000",
                                    .observer = {.on_state = cnet_api_test_ignore_state},
                                    .tls = &tls};

    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_connect(&client, &options, &connection), SALTS_EINVAL);
    check_equal(connection.slot, 0u);
    check_equal(connection.generation, 0u);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
  }

  it("copies options and supports reentrant TCP operations on the I/O owner") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_probe probe = {.client = &client, .expected_kind = CNET_MESSAGE_BYTES};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    char uri[64];
    uint16_t port = 0u;
    unsigned char outbound = 0u;
    unsigned char send_value = 41u;
    const unsigned char expected_outbound = 41u;
    const unsigned char expected_inbound = 73u;
    cnet_connect_options options;
    int stale_status = SALTS_OK;
    uint64_t stale_deadline;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.callback_stop_status, SALTS_OK);
    atomic_init(&probe.callback_operation_status, SALTS_EIO);
    atomic_init(&probe.failed, 0);
    atomic_init(&probe.block_terminal, 0);
    atomic_init(&probe.release_terminal, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    (void)snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_state,
                                                  .on_receive = cnet_api_test_receive,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    memset(uri, 'x', strlen(uri));
    options.observer = (cnet_observer){0};

    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);
    check_equal(atomic_load_explicit(&probe.callback_stop_status, memory_order_acquire),
                SALTS_EBUSY);
    check_equal(atomic_load_explicit(&probe.callback_operation_status, memory_order_acquire),
                SALTS_OK);

    check_equal(cnet_api_test_send_bytes(&client, connection, &send_value, sizeof(send_value)), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &send_value, sizeof(send_value)), SALTS_OK);
    send_value = 99u;
    {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    }
    check_equal(recv(accepted, (char *)&outbound, (int)sizeof(outbound), 0), (int)sizeof(outbound));
    check_equal(outbound, expected_outbound);
    check_equal(send(accepted, (const char *)&expected_inbound, (int)sizeof(expected_inbound), 0),
                (int)sizeof(expected_inbound));
    check_equal(cnet_api_test_poll_until(&client, &probe.received, 1), SALTS_OK);
    check_equal(probe.received_value, expected_inbound);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);

    stale_deadline = cmeta_monotonic_ms() + CNET_API_TEST_TIMEOUT_MS;
    do {
      stale_status = cnet_api_test_send_bytes(&client, connection, &expected_outbound, sizeof(expected_outbound));
      if (stale_status == SALTS_ENOENT) break;
      cmeta_thread_yield();
    } while (cmeta_monotonic_ms() < stale_deadline);
    check_equal(stale_status, SALTS_ENOENT);
    check_equal(cnet_set_receive_slice_handler(
                    &client, connection, cnet_api_test_owned_receive, NULL),
                SALTS_ENOENT);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("admits multiple bounded non-TLS writes and settles them in FIFO order") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {.expected_send_size = 1u};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    unsigned char received[3] = {0};
    const unsigned char first = 0x11u;
    const unsigned char second = 0x22u;
    const unsigned char third = 0x33u;
    char uri[64];
    uint16_t port = 0u;
    size_t received_size = 0u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .on_send = cnet_api_test_listener_send,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(accepted), SALTS_OK);

    check_equal(cnet_api_test_send_bytes(&client, connection, &first, sizeof(first)), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &second, sizeof(second)), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &third, sizeof(third)), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.sent, memory_order_acquire), 0);
    check_equal(cnet_api_test_poll_until(&client, &probe.sent, 3), SALTS_OK);

    while (received_size < sizeof(received)) {
      const int got =
          recv(accepted, (char *)&received[received_size], (int)(sizeof(received) - received_size), 0);
      check_greater(got, 0);
      received_size += (size_t)got;
    }
    check_equal(received[0], first);
    check_equal(received[1], second);
    check_equal(received[2], third);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);

    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("uses write-slot exhaustion as ordinary non-TLS send backpressure") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_listener_probe probe = {.expected_send_size = 1u};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options = {0};
    const unsigned char value = 0x41u;
    char uri[64];
    uint16_t port = 0u;
    size_t accepted_count = 0u;

    config.command_capacity = 2u;
    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.sent, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    check_greater(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port), 0);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_listener_state,
                                                  .on_send = cnet_api_test_listener_send,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);

    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_OK);
    ++accepted_count;
    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_OK);
    ++accepted_count;
    check_equal(cnet_api_test_send_bytes(&client, connection, &value, sizeof(value)), SALTS_ENOBUFS);

    check_equal(cnet_api_test_poll_until(&client, &probe.sent, (int)accepted_count), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("drains one live TCP connection before stopping") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_probe probe = {.client = &client, .expected_kind = CNET_MESSAGE_BYTES};
    cnet_api_test_socket listener = CNET_API_TEST_INVALID_SOCKET;
    cnet_api_test_socket accepted = CNET_API_TEST_INVALID_SOCKET;
    cnet_connection connection = {0};
    cnet_connect_options options;
    char uri[64];
    uint16_t port = 0u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.callback_stop_status, SALTS_OK);
    atomic_init(&probe.callback_operation_status, SALTS_EIO);
    atomic_init(&probe.failed, 0);
    atomic_init(&probe.block_terminal, 0);
    atomic_init(&probe.release_terminal, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    check_equal(cnet_api_test_listener(&listener, &port), SALTS_OK);
    (void)snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned int)port);
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_state,
                                                  .on_receive = cnet_api_test_receive,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.terminal, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(accepted);
    cnet_api_test_close_socket(listener);
  }

  it("preserves one connected UDP datagram as one public receive value") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_probe probe = {.client = &client, .expected_kind = CNET_MESSAGE_DATAGRAM};
    cnet_api_test_socket server = CNET_API_TEST_INVALID_SOCKET;
    struct sockaddr_in server_address;
    struct sockaddr_in client_address;
#if defined(_WIN32)
    int server_length = (int)sizeof(server_address);
    int client_length = (int)sizeof(client_address);
#else
    socklen_t server_length = (socklen_t)sizeof(server_address);
    socklen_t client_length = (socklen_t)sizeof(client_address);
#endif
    cnet_connection connection = {0};
    cnet_connect_options options;
    char uri[64];
    unsigned char outbound = 0u;
    const unsigned char expected_outbound = 29u;
    const unsigned char expected_inbound = 97u;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.callback_stop_status, SALTS_OK);
    atomic_init(&probe.callback_operation_status, SALTS_EIO);
    atomic_init(&probe.failed, 0);
    atomic_init(&probe.block_terminal, 0);
    atomic_init(&probe.release_terminal, 0);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check_true(server != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(server), SALTS_OK);
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check_equal(bind(server, (const struct sockaddr *)&server_address, (int)sizeof(server_address)),
                0);
    check_equal(getsockname(server, (struct sockaddr *)&server_address, &server_length), 0);
    (void)snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u",
                   (unsigned int)ntohs(server_address.sin_port));
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_state,
                                                  .on_receive = cnet_api_test_receive,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &expected_outbound, sizeof(expected_outbound)),
                SALTS_OK);
    {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    }
    check_equal(recvfrom(server, (char *)&outbound, (int)sizeof(outbound), 0,
                         (struct sockaddr *)&client_address, &client_length),
                (int)sizeof(outbound));
    check_equal(outbound, expected_outbound);
    check_equal(sendto(server, (const char *)&expected_inbound, (int)sizeof(expected_inbound), 0,
                       (const struct sockaddr *)&client_address, client_length),
                (int)sizeof(expected_inbound));
    check_equal(cnet_api_test_poll_until(&client, &probe.received, 1), SALTS_OK);
    check_equal(probe.received_value, expected_inbound);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(server);
  }

  it("delivers every UDP datagram from one bounded receive demand") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_batch_probe probe;
    cnet_api_test_socket server = CNET_API_TEST_INVALID_SOCKET;
    struct sockaddr_in server_address;
    struct sockaddr_in client_address;
#if defined(_WIN32)
    int server_length = (int)sizeof(server_address);
    int client_length = (int)sizeof(client_address);
#else
    socklen_t server_length = (socklen_t)sizeof(server_address);
    socklen_t client_length = (socklen_t)sizeof(client_address);
#endif
    cnet_connection connection = {0};
    cnet_connect_options options;
    char uri[64];
    int exchange_status = SALTS_OK;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.failed, 0);
    probe.received_value = 0u;
    config.connection_capacity = 64u;
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check_true(server != CNET_API_TEST_INVALID_SOCKET);
    check_equal(cnet_api_test_set_receive_timeout(server), SALTS_OK);
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check_equal(bind(server, (const struct sockaddr *)&server_address, (int)sizeof(server_address)),
                0);
    check_equal(getsockname(server, (struct sockaddr *)&server_address, &server_length), 0);
    (void)snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u",
                   (unsigned int)ntohs(server_address.sin_port));
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_batch_state,
                                                  .on_receive = cnet_api_test_batch_receive,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    check_equal(cnet_receive(&client, connection, CNET_API_TEST_BATCH_DATAGRAMS), SALTS_OK);

    for (int index = 0; exchange_status == SALTS_OK && index < CNET_API_TEST_BATCH_DATAGRAMS;
         ++index) {
      unsigned char outbound = 0u;
      const unsigned char expected = (unsigned char)(index + 1);
      exchange_status = cnet_api_test_send_bytes(&client, connection, &expected, sizeof(expected));
      if (exchange_status == SALTS_OK) {
        size_t events = 0u;
        exchange_status = cnet_client_poll(&client, 0u, &events);
      }
      if (exchange_status == SALTS_OK) {
        const int received = recvfrom(server, (char *)&outbound, (int)sizeof(outbound), 0,
                                      (struct sockaddr *)&client_address, &client_length);
        if (received != (int)sizeof(outbound) || outbound != expected) exchange_status = SALTS_EIO;
      }
      if (exchange_status == SALTS_OK) {
        const int sent = sendto(server, (const char *)&expected, (int)sizeof(expected), 0,
                                (const struct sockaddr *)&client_address, client_length);
        if (sent != (int)sizeof(expected)) exchange_status = SALTS_EIO;
      }
      if (exchange_status == SALTS_OK)
        exchange_status = cnet_api_test_poll_until(&client, &probe.received, index + 1);
      if (exchange_status == SALTS_OK &&
          (probe.received_value != expected ||
           atomic_load_explicit(&probe.failed, memory_order_acquire) != 0))
        exchange_status = SALTS_EIO;
    }
    check_equal(exchange_status, SALTS_OK);

    check_equal(cnet_close(&client, connection), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_api_test_close_socket(server);
  }

  it("uses the same public byte API for a platform Pipe endpoint") {
    cnet_client client = {0};
    cnet_client_config config = cnet_api_test_config();
    cnet_api_test_probe probe = {.client = &client, .expected_kind = CNET_MESSAGE_BYTES};
    cnet_shared_test_named_pipe pipe;
    cnet_connection connection = {0};
    cnet_connect_options options;
    char uri[CNET_URI_PATH_CAPACITY + 8u];
    unsigned char outbound = 0u;
    const unsigned char expected_outbound = 53u;
    const unsigned char expected_inbound = 89u;
    int uri_length;

    atomic_init(&probe.connected, 0);
    atomic_init(&probe.received, 0);
    atomic_init(&probe.terminal, 0);
    atomic_init(&probe.callback_stop_status, SALTS_OK);
    atomic_init(&probe.callback_operation_status, SALTS_EIO);
    atomic_init(&probe.failed, 0);
    atomic_init(&probe.block_terminal, 0);
    atomic_init(&probe.release_terminal, 0);
    check_equal(cnet_shared_test_named_pipe_start(&pipe), SALTS_OK);
    check_equal(cnet_client_init(&client, &config), SALTS_OK);
    uri_length = snprintf(uri, sizeof(uri), "pipe://%s", pipe.name);
    check_true(uri_length > 0 && (size_t)uri_length < sizeof(uri));
    options = (cnet_connect_options){.uri = uri,
                                     .observer = {.on_state = cnet_api_test_state,
                                                  .on_receive = cnet_api_test_receive,
                                                  .user = &probe}};
    check_equal(cnet_connect(&client, &options, &connection), SALTS_OK);
    {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    }
    check_equal(cnet_shared_test_named_pipe_finish(&pipe), SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.connected, 1), SALTS_OK);
    check_equal(cnet_api_test_send_bytes(&client, connection, &expected_outbound, sizeof(expected_outbound)),
                SALTS_OK);
    {
      size_t events = 0u;
      check_equal(cnet_client_poll(&client, 0u, &events), SALTS_OK);
    }
    check_equal(cnet_api_test_wait_pipe_read(&pipe, &outbound, sizeof(outbound)), SALTS_OK);
    check_equal(outbound, expected_outbound);
    check_equal(
        cnet_shared_test_named_pipe_peer_write(&pipe, &expected_inbound, sizeof(expected_inbound)),
        SALTS_OK);
    check_equal(cnet_api_test_poll_until(&client, &probe.received, 1), SALTS_OK);
    check_equal(probe.received_value, expected_inbound);
    check_equal(cnet_api_test_poll_until(&client, &probe.terminal, 1), SALTS_OK);
    check_equal(atomic_load_explicit(&probe.failed, memory_order_acquire), 0);
    check_equal(cnet_client_stop(&client, CNET_API_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    cnet_shared_test_named_pipe_close(&pipe);
  }
}
