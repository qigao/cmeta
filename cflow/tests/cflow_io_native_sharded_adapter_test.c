#include <cflow/io_native_sharded_adapter.h>
#include <cflow/executor.h>
#include <cflow/graph.h>
#include <cflow/io_publisher.h>
#include <cflow/lower.h>
#include <cflow/scheduler.h>

#include <error_codes.h>
#include <native_io_sharded.h>
#include <thread.h>

#include "tinytest.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET sharded_adapter_test_socket;
typedef int sharded_adapter_test_socklen;
  #define SHARDED_ADAPTER_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
typedef int sharded_adapter_test_socket;
typedef socklen_t sharded_adapter_test_socklen;
  #define SHARDED_ADAPTER_TEST_INVALID_SOCKET (-1)
#endif

enum {
  SHARDED_ADAPTER_TEST_TIMEOUT_MS = 5000,
  SHARDED_ADAPTER_TEST_CAPACITY = 4,
  SHARDED_ADAPTER_TEST_QUEUE_CAPACITY = 8,
  SHARDED_ADAPTER_TEST_DRIVE_STEPS = 32
};

typedef struct sharded_adapter_test_endpoint {
  native_io_sharded_endpoint endpoint;
  uintptr_t native_handle;
  int status;
} sharded_adapter_test_endpoint;

typedef struct sharded_adapter_test_observe {
  native_io_sharded_completion events[SHARDED_ADAPTER_TEST_CAPACITY];
  size_t count;
  int status;
} sharded_adapter_test_observe;

typedef struct sharded_adapter_test_token {
  native_io_sharded_operation operation;
  unsigned char buffer[8];
  int released;
} sharded_adapter_test_token;

typedef struct sharded_adapter_test_completion {
  cflow_io_request_id request_id;
  cflow_io_completion completion;
  size_t count;
} sharded_adapter_test_completion;

typedef struct sharded_adapter_test_actor {
  cflow_executor executor;
  cflow_io_actor actor;
  cflow_io_native_sharded_adapter adapter;
  sharded_adapter_test_completion completion;
} sharded_adapter_test_actor;

typedef struct sharded_adapter_test_same_driver {
  sharded_adapter_test_actor *fixture;
  native_io_sharded_stats before;
  native_io_sharded_stats after;
  cflow_io_run_result run;
} sharded_adapter_test_same_driver;

typedef struct sharded_adapter_test_gate {
  atomic_bool entered;
  atomic_bool release;
} sharded_adapter_test_gate;

typedef struct sharded_adapter_test_publisher_fixture {
  sharded_adapter_test_token token;
  size_t prepared;
  size_t encoded;
} sharded_adapter_test_publisher_fixture;

typedef struct sharded_adapter_test_sink {
  int value;
  size_t values;
  size_t errors;
  size_t done;
  const char *error;
} sharded_adapter_test_sink;

static int sharded_adapter_test_network_start(void) {
#if defined(_WIN32)
  WSADATA data;
  const int status = WSAStartup(MAKEWORD(2, 2), &data);
  return status == 0 ? SALTS_OK : -status;
#else
  return SALTS_OK;
#endif
}

static void sharded_adapter_test_network_stop(void) {
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

static int sharded_adapter_test_socket_error(void) {
#if defined(_WIN32)
  const int error = WSAGetLastError();
#else
  const int error = errno;
#endif
  return error == 0 ? SALTS_EIO : -error;
}

static void sharded_adapter_test_close_socket(
    sharded_adapter_test_socket socket_value) {
  if (socket_value == SHARDED_ADAPTER_TEST_INVALID_SOCKET)
    return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int sharded_adapter_test_set_nonblocking(
    sharded_adapter_test_socket socket_value) {
#if defined(_WIN32)
  u_long enabled = 1u;
  return ioctlsocket(socket_value, FIONBIO, &enabled) == 0
             ? SALTS_OK
             : sharded_adapter_test_socket_error();
#else
  const int flags = fcntl(socket_value, F_GETFL, 0);
  if (flags < 0)
    return sharded_adapter_test_socket_error();
  if ((flags & O_NONBLOCK) != 0)
    return SALTS_OK;
  return fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0
             ? SALTS_OK
             : sharded_adapter_test_socket_error();
#endif
}

static int sharded_adapter_test_set_nodelay(
    sharded_adapter_test_socket socket_value) {
  const int enabled = 1;
#if defined(_WIN32)
  return setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY,
                    (const char *)&enabled, (int)sizeof(enabled)) == 0
             ? SALTS_OK
             : sharded_adapter_test_socket_error();
#else
  return setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY,
                    &enabled, (socklen_t)sizeof(enabled)) == 0
             ? SALTS_OK
             : sharded_adapter_test_socket_error();
#endif
}

static int sharded_adapter_test_bind_loopback(
    sharded_adapter_test_socket socket_value,
    struct sockaddr_in *address) {
  sharded_adapter_test_socklen length =
      (sharded_adapter_test_socklen)sizeof(*address);

  memset(address, 0, sizeof(*address));
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address->sin_port = 0u;
  if (bind(socket_value, (const struct sockaddr *)address,
           (sharded_adapter_test_socklen)sizeof(*address)) != 0)
    return sharded_adapter_test_socket_error();
  if (getsockname(socket_value, (struct sockaddr *)address, &length) != 0)
    return sharded_adapter_test_socket_error();
  return SALTS_OK;
}

static int sharded_adapter_test_make_tcp_pair(
    sharded_adapter_test_socket sockets[2]) {
  sharded_adapter_test_socket listener =
      SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  struct sockaddr_in address;
  int status;

  sockets[0] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  sockets[1] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  listener = WSASocketW(
      AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0u, WSA_FLAG_OVERLAPPED);
#else
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
  if (listener == SHARDED_ADAPTER_TEST_INVALID_SOCKET)
    return sharded_adapter_test_socket_error();

  status = sharded_adapter_test_bind_loopback(listener, &address);
  if (status == SALTS_OK && listen(listener, 1) != 0)
    status = sharded_adapter_test_socket_error();
  if (status == SALTS_OK) {
#if defined(_WIN32)
    sockets[0] = WSASocketW(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0u, WSA_FLAG_OVERLAPPED);
#else
    sockets[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (sockets[0] == SHARDED_ADAPTER_TEST_INVALID_SOCKET)
      status = sharded_adapter_test_socket_error();
  }
  if (status == SALTS_OK &&
      connect(sockets[0], (const struct sockaddr *)&address,
              (sharded_adapter_test_socklen)sizeof(address)) != 0)
    status = sharded_adapter_test_socket_error();
  if (status == SALTS_OK) {
    sockets[1] = accept(listener, NULL, NULL);
    if (sockets[1] == SHARDED_ADAPTER_TEST_INVALID_SOCKET)
      status = sharded_adapter_test_socket_error();
  }
  sharded_adapter_test_close_socket(listener);

  for (size_t index = 0u; status == SALTS_OK && index < 2u; ++index) {
    status = sharded_adapter_test_set_nodelay(sockets[index]);
    if (status == SALTS_OK)
      status = sharded_adapter_test_set_nonblocking(sockets[index]);
  }

  if (status != SALTS_OK) {
    sharded_adapter_test_close_socket(sockets[0]);
    sharded_adapter_test_close_socket(sockets[1]);
    sockets[0] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
    sockets[1] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  }
  return status;
}

static int sharded_adapter_test_peer_send(
    sharded_adapter_test_socket socket_value,
    const unsigned char *data,
    size_t size) {
  size_t offset = 0u;

  while (offset < size) {
#if defined(_WIN32)
    const int sent =
        send(socket_value, (const char *)data + offset,
             (int)(size - offset), 0);
    if (sent == SOCKET_ERROR) {
      const int error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK) {
        cmeta_thread_yield();
        continue;
      }
      return -error;
    }
#else
    const ssize_t sent = send(
        socket_value, data + offset, size - offset,
#if defined(MSG_NOSIGNAL)
        MSG_NOSIGNAL
#else
        0
#endif
    );
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        cmeta_thread_yield();
        continue;
      }
      return -errno;
    }
#endif
    if (sent <= 0)
      return SALTS_EIO;
    offset += (size_t)sent;
  }
  return SALTS_OK;
}

static native_io_backend_kind sharded_adapter_test_backends(
    native_io_backend_kind backends[2],
    size_t *count) {
#if defined(_WIN32)
  backends[0] = NATIVE_IO_BACKEND_IOCP;
  *count = 1u;
#elif defined(__linux__)
  backends[0] = NATIVE_IO_BACKEND_EPOLL;
  backends[1] = NATIVE_IO_BACKEND_IO_URING;
  *count = 2u;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||     defined(__NetBSD__) || defined(__DragonFly__)
  backends[0] = NATIVE_IO_BACKEND_KQUEUE;
  *count = 1u;
#else
  (void)backends;
  *count = 0u;
#endif
  return *count != 0u ? backends[0] : (native_io_backend_kind)0;
}

static int sharded_adapter_test_runtime_create(
    native_io_backend_kind kind,
    size_t queue_capacity,
    native_io_sharded **out_runtime) {
  const native_io_sharded_config config = {
      2u,
      queue_capacity,
      {kind, 2u, SHARDED_ADAPTER_TEST_CAPACITY,
       SHARDED_ADAPTER_TEST_CAPACITY}};
  return native_io_sharded_create(&config, out_runtime);
}

static void sharded_adapter_test_attach(
    native_io_sharded_context *context, void *arg) {
  sharded_adapter_test_endpoint *state =
      (sharded_adapter_test_endpoint *)arg;
  state->status = native_io_sharded_context_attach_socket(
      context, state->native_handle, &state->endpoint);
}

static void sharded_adapter_test_release(
    native_io_sharded_context *context, void *arg) {
  sharded_adapter_test_endpoint *state =
      (sharded_adapter_test_endpoint *)arg;
  state->status =
      native_io_sharded_context_release_socket(context, state->endpoint);
  if (state->status == SALTS_OK)
    state->endpoint = (native_io_sharded_endpoint){0};
}

static void sharded_adapter_test_observe_task(
    native_io_sharded_context *context, void *arg) {
  sharded_adapter_test_observe *state =
      (sharded_adapter_test_observe *)arg;
  state->count = 0u;
  state->status = native_io_sharded_context_observe(
      context, state->events, SHARDED_ADAPTER_TEST_CAPACITY,
      SHARDED_ADAPTER_TEST_TIMEOUT_MS, &state->count);
}

static int sharded_adapter_test_attach_endpoint(
    native_io_sharded *runtime,
    sharded_adapter_test_socket socket_value,
    sharded_adapter_test_endpoint *endpoint) {
  native_io_sharded_task task;

  *endpoint = (sharded_adapter_test_endpoint){
      .native_handle = (uintptr_t)socket_value,
      .status = SALTS_EIO};
  task = (native_io_sharded_task){
      sharded_adapter_test_attach, NULL, NULL, endpoint};
  int status = native_io_sharded_submit_to(runtime, 1u, &task);
  if (status == SALTS_OK)
    status = native_io_sharded_wait(runtime);
  if (status == SALTS_OK)
    status = endpoint->status;
  return status;
}

static int sharded_adapter_test_release_endpoint(
    native_io_sharded *runtime,
    sharded_adapter_test_endpoint *endpoint) {
  native_io_sharded_task task = {
      sharded_adapter_test_release, NULL, NULL, endpoint};
  int status = native_io_sharded_submit_to(runtime, 1u, &task);
  if (status == SALTS_OK)
    status = native_io_sharded_wait(runtime);
  if (status == SALTS_OK)
    status = endpoint->status;
  return status;
}

static int sharded_adapter_test_observe_owner(
    native_io_sharded *runtime,
    sharded_adapter_test_observe *observe) {
  native_io_sharded_task task = {
      sharded_adapter_test_observe_task, NULL, NULL, observe};
  observe->status = SALTS_EIO;
  observe->count = 0u;
  int status = native_io_sharded_submit_to(runtime, 1u, &task);
  if (status == SALTS_OK)
    status = native_io_sharded_wait(runtime);
  if (status == SALTS_OK)
    status = observe->status;
  return status;
}

static void sharded_adapter_test_token_release(void *user) {
  native_io_sharded_operation *operation =
      (native_io_sharded_operation *)user;
  sharded_adapter_test_token *token =
      (sharded_adapter_test_token *)operation;
  ++token->released;
}

static void sharded_adapter_test_completion_record(
    void *user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion) {
  sharded_adapter_test_completion *probe =
      (sharded_adapter_test_completion *)user;

  (void)lease_id;
  (void)operation_user;
  if (probe == NULL || completion == NULL)
    return;
  probe->request_id = request_id;
  probe->completion = *completion;
  ++probe->count;
}

static int sharded_adapter_test_actor_init(
    sharded_adapter_test_actor *fixture,
    native_io_sharded *runtime,
    size_t capacity) {
  cflow_io_native_sharded_adapter_config adapter_config = {
      runtime, capacity};
  cflow_io_actor_config actor_config;

  memset(fixture, 0, sizeof(*fixture));
  if (!cflow_executor_manual_init_with_capacity(
          &fixture->executor, capacity))
    return SALTS_ENOMEM;
  int status = cflow_io_native_sharded_adapter_init(
      &fixture->adapter, &adapter_config);
  if (status != SALTS_OK) {
    cflow_executor_destroy(&fixture->executor);
    return status;
  }

  memset(&actor_config, 0, sizeof(actor_config));
  actor_config.request_capacity = capacity;
  actor_config.command_capacity = capacity * 2u;
  actor_config.executor = &fixture->executor;
  actor_config.backend = cflow_io_native_sharded_adapter_actor_ops();
  actor_config.backend_user = &fixture->adapter;
  actor_config.completion = sharded_adapter_test_completion_record;
  actor_config.completion_user = &fixture->completion;
  status = cflow_io_actor_init(&fixture->actor, &actor_config);
  if (status != SALTS_OK) {
    (void)cflow_io_native_sharded_adapter_close(&fixture->adapter);
    (void)cflow_io_native_sharded_adapter_destroy(&fixture->adapter);
    cflow_executor_destroy(&fixture->executor);
  }
  return status;
}

static void sharded_adapter_test_actor_finish(
    sharded_adapter_test_actor *fixture) {
  int status = cflow_io_actor_close(&fixture->actor);
  check_true(status == SALTS_OK || status == SALTS_EALREADY);
  for (size_t attempts = 0u;
       attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS &&
       !cflow_io_actor_is_quiescent(&fixture->actor);
       ++attempts) {
    (void)cflow_io_actor_run_ready(
        &fixture->actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
    (void)cflow_executor_run_ready(&fixture->executor);
  }
  check_true(cflow_io_actor_is_quiescent(&fixture->actor));
  check_equal(cflow_io_actor_destroy(&fixture->actor), SALTS_OK);
  status = cflow_io_native_sharded_adapter_close(&fixture->adapter);
  check_true(status == SALTS_OK || status == SALTS_EALREADY);
  check_equal(
      cflow_io_native_sharded_adapter_destroy(&fixture->adapter),
      SALTS_OK);
  check_true(cflow_executor_shutdown(&fixture->executor));
  cflow_executor_destroy(&fixture->executor);
}

static cflow_io_submit_result sharded_adapter_test_submit_recv(
    sharded_adapter_test_actor *fixture,
    sharded_adapter_test_endpoint *endpoint,
    sharded_adapter_test_token *token) {
  cflow_io_operation moved;

  memset(token, 0, sizeof(*token));
  token->operation = (native_io_sharded_operation){
      .kind = NATIVE_IO_OPERATION_STREAM_RECV,
      .endpoint = endpoint->endpoint,
      .buffer = token->buffer,
      .length = sizeof(token->buffer),
      .user_data = 0x481u};
  moved = (cflow_io_operation){
      &token->operation, sharded_adapter_test_token_release};
  return cflow_io_actor_try_submit(
      &fixture->actor, (cflow_io_lease_id)0x481u, &moved);
}

static void sharded_adapter_test_deliver_and_ack(
    sharded_adapter_test_actor *fixture,
    cflow_io_request_id request_id) {
  for (size_t attempts = 0u;
       attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS &&
       fixture->completion.count == 0u;
       ++attempts) {
    (void)cflow_io_actor_run_ready(
        &fixture->actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
    (void)cflow_executor_run_ready(&fixture->executor);
  }
  check_equal(fixture->completion.count, (size_t)1u);
  check_equal(fixture->completion.request_id, request_id);
  check_equal(
      cflow_io_actor_acknowledge(&fixture->actor, request_id),
      CFLOW_IO_ACK_RELEASED);
}

static void sharded_adapter_test_same_driver_with_stats(
    native_io_sharded_context *context, void *arg) {
  sharded_adapter_test_same_driver *driver =
      (sharded_adapter_test_same_driver *)arg;
  cflow_io_native_sharded_adapter_stats adapter_stats;

  (void)context;
  memset(&adapter_stats, 0, sizeof(adapter_stats));
  if (!cflow_io_native_sharded_adapter_get_stats(
          &driver->fixture->adapter, &adapter_stats)) {
    driver->run = (cflow_io_run_result){
        CFLOW_IO_RUN_INVALID_ARGUMENT, 0u};
    return;
  }
  driver->before = adapter_stats.native;
  driver->run = cflow_io_actor_run_ready(
      &driver->fixture->actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
  memset(&adapter_stats, 0, sizeof(adapter_stats));
  if (!cflow_io_native_sharded_adapter_get_stats(
          &driver->fixture->adapter, &adapter_stats)) {
    driver->run = (cflow_io_run_result){
        CFLOW_IO_RUN_INVALID_ARGUMENT, 0u};
    return;
  }
  driver->after = adapter_stats.native;
}

static void sharded_adapter_test_gate_run(
    native_io_sharded_context *context, void *arg) {
  sharded_adapter_test_gate *gate =
      (sharded_adapter_test_gate *)arg;
  (void)context;
  atomic_store(&gate->entered, true);
  while (!atomic_load(&gate->release))
    cmeta_thread_yield();
}

static void sharded_adapter_test_noop(
    native_io_sharded_context *context, void *arg) {
  (void)context;
  (void)arg;
}

static void sharded_adapter_test_run_one_backend(
    native_io_backend_kind kind,
    bool same_owner) {
  native_io_sharded *runtime = NULL;
  sharded_adapter_test_socket sockets[2];
  sharded_adapter_test_endpoint endpoint;
  sharded_adapter_test_actor fixture;
  sharded_adapter_test_token token;
  sharded_adapter_test_observe observe = {0};
  native_io_sharded_stats before =
      NATIVE_IO_SHARDED_STATS_V1_INITIALIZER;
  native_io_sharded_stats after =
      NATIVE_IO_SHARDED_STATS_V1_INITIALIZER;
  cflow_io_submit_result submitted;
  int status;

  status = sharded_adapter_test_runtime_create(
      kind, SHARDED_ADAPTER_TEST_QUEUE_CAPACITY, &runtime);
  if (status == SALTS_ENOTSUP) {
    check_equal(status, SALTS_ENOTSUP);
    return;
  }
  check_equal(status, SALTS_OK);
  check_equal(sharded_adapter_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(
      sharded_adapter_test_attach_endpoint(runtime, sockets[1], &endpoint),
      SALTS_OK);
  check_equal(
      native_io_sharded_endpoint_owner_shard(endpoint.endpoint),
      (size_t)1u);
  check_equal(
      sharded_adapter_test_actor_init(
          &fixture, runtime, SHARDED_ADAPTER_TEST_CAPACITY),
      SALTS_OK);

  submitted =
      sharded_adapter_test_submit_recv(&fixture, &endpoint, &token);
  check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
  check_not_equal(submitted.request_id, (cflow_io_request_id)0u);
  check_equal(token.released, 0);

  if (same_owner) {
    sharded_adapter_test_same_driver driver = {
        .fixture = &fixture,
        .before = NATIVE_IO_SHARDED_STATS_V1_INITIALIZER,
        .after = NATIVE_IO_SHARDED_STATS_V1_INITIALIZER};
    native_io_sharded_task task = {
        sharded_adapter_test_same_driver_with_stats,
        NULL, NULL, &driver};
    check_equal(
        native_io_sharded_submit_to(runtime, 1u, &task), SALTS_OK);
    check_equal(native_io_sharded_wait(runtime), SALTS_OK);
    check_true(
        driver.run.status == CFLOW_IO_RUN_PROGRESSED ||
        driver.run.status == CFLOW_IO_RUN_IDLE);
    check_equal(
        driver.after.queued_dispatches,
        driver.before.queued_dispatches);
    check_equal(
        driver.after.same_shard_direct_tasks,
        driver.before.same_shard_direct_tasks + 1u);
  } else {
    check_true(native_io_sharded_get_stats(runtime, &before));
    {
      const cflow_io_run_result run = cflow_io_actor_run_ready(
          &fixture.actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
      check_true(
          run.status == CFLOW_IO_RUN_PROGRESSED ||
          run.status == CFLOW_IO_RUN_IDLE);
    }
    check_equal(native_io_sharded_wait(runtime), SALTS_OK);
    check_true(native_io_sharded_get_stats(runtime, &after));
    check_equal(
        after.same_shard_direct_tasks,
        before.same_shard_direct_tasks);
    check_equal(
        after.queued_dispatches,
        before.queued_dispatches + 1u);
  }

  check_equal(
      sharded_adapter_test_peer_send(
          sockets[0], (const unsigned char *)"abcdefgh",
          sizeof(token.buffer)),
      SALTS_OK);
  check_equal(sharded_adapter_test_observe_owner(runtime, &observe), SALTS_OK);
  check_true(observe.count >= 1u);
  {
    native_io_sharded_stats completion_before =
        NATIVE_IO_SHARDED_STATS_V1_INITIALIZER;
    native_io_sharded_stats completion_after =
        NATIVE_IO_SHARDED_STATS_V1_INITIALIZER;

    check_true(native_io_sharded_get_stats(runtime, &completion_before));
    sharded_adapter_test_deliver_and_ack(
        &fixture, submitted.request_id);
    check_true(native_io_sharded_get_stats(runtime, &completion_after));
    check_equal(
        completion_after.queued_dispatches,
        completion_before.queued_dispatches);
    check_equal(
        completion_after.same_shard_direct_tasks,
        completion_before.same_shard_direct_tasks);
  }
  check_equal(
      fixture.completion.completion.kind,
      CFLOW_IO_COMPLETION_OK);
  check_equal(
      fixture.completion.completion.bytes,
      sizeof(token.buffer));
  check_equal(
      memcmp(token.buffer, "abcdefgh", sizeof(token.buffer)), 0);
  check_equal(token.released, 1);

  {
    cflow_io_native_sharded_adapter_stats stats = {0};
    check_true(cflow_io_native_sharded_adapter_get_stats(
        &fixture.adapter, &stats));
    check_equal(stats.active_bridges, (size_t)0u);
    check_equal(stats.raw_admissions, (uint64_t)1u);
    check_equal(stats.raw_admission_failures, (uint64_t)0u);
    check_equal(stats.terminal_completions, (uint64_t)1u);
    check_equal(stats.stale_actor_completions, (uint64_t)0u);
  }

  sharded_adapter_test_actor_finish(&fixture);
  sharded_adapter_test_close_socket(sockets[1]);
  sockets[1] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(
      sharded_adapter_test_release_endpoint(runtime, &endpoint),
      SALTS_OK);
  sharded_adapter_test_close_socket(sockets[0]);
  sockets[0] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(native_io_sharded_destroy(runtime), SALTS_OK);
}

static void sharded_adapter_test_run_full_backend(
    native_io_backend_kind kind) {
  native_io_sharded *runtime = NULL;
  sharded_adapter_test_socket sockets[2];
  sharded_adapter_test_endpoint endpoint;
  sharded_adapter_test_actor fixture;
  sharded_adapter_test_token token;
  sharded_adapter_test_gate gate;
  native_io_sharded_task gate_task;
  native_io_sharded_task queued_task;
  cflow_io_submit_result submitted;
  int status;

  status = sharded_adapter_test_runtime_create(kind, 1u, &runtime);
  if (status == SALTS_ENOTSUP) {
    check_equal(status, SALTS_ENOTSUP);
    return;
  }
  check_equal(status, SALTS_OK);
  check_equal(sharded_adapter_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(
      sharded_adapter_test_attach_endpoint(runtime, sockets[1], &endpoint),
      SALTS_OK);
  check_equal(
      sharded_adapter_test_actor_init(&fixture, runtime, 1u),
      SALTS_OK);

  atomic_init(&gate.entered, false);
  atomic_init(&gate.release, false);
  gate_task = (native_io_sharded_task){
      sharded_adapter_test_gate_run, NULL, NULL, &gate};
  queued_task = (native_io_sharded_task){
      sharded_adapter_test_noop, NULL, NULL, NULL};
  check_equal(
      native_io_sharded_try_submit_to(runtime, 1u, &gate_task),
      SALTS_OK);
  while (!atomic_load(&gate.entered))
    cmeta_thread_yield();
  check_equal(
      native_io_sharded_try_submit_to(runtime, 1u, &queued_task),
      SALTS_OK);

  submitted =
      sharded_adapter_test_submit_recv(&fixture, &endpoint, &token);
  check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
  {
    const cflow_io_run_result run = cflow_io_actor_run_ready(
        &fixture.actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
    check_true(
        run.status == CFLOW_IO_RUN_PROGRESSED ||
        run.status == CFLOW_IO_RUN_IDLE);
  }

  for (size_t attempts = 0u;
       attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS &&
       fixture.completion.count == 0u;
       ++attempts) {
    (void)cflow_io_actor_run_ready(
        &fixture.actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
    (void)cflow_executor_run_ready(&fixture.executor);
  }
  check_equal(fixture.completion.count, (size_t)1u);
  check_equal(
      fixture.completion.completion.kind,
      CFLOW_IO_COMPLETION_FAILED);
  check_equal(
      fixture.completion.completion.error,
      SALTS_ENOBUFS);
  check_equal(
      cflow_io_actor_acknowledge(
          &fixture.actor, submitted.request_id),
      CFLOW_IO_ACK_RELEASED);
  check_equal(token.released, 1);

  atomic_store(&gate.release, true);
  check_equal(native_io_sharded_wait(runtime), SALTS_OK);

  {
    cflow_io_native_sharded_adapter_stats stats = {0};
    check_true(cflow_io_native_sharded_adapter_get_stats(
        &fixture.adapter, &stats));
    check_equal(stats.active_bridges, (size_t)0u);
    check_equal(stats.accepted_routes, (uint64_t)0u);
    check_equal(stats.terminal_completions, (uint64_t)0u);
  }

  sharded_adapter_test_actor_finish(&fixture);
  sharded_adapter_test_close_socket(sockets[1]);
  sockets[1] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(
      sharded_adapter_test_release_endpoint(runtime, &endpoint),
      SALTS_OK);
  sharded_adapter_test_close_socket(sockets[0]);
  sockets[0] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(native_io_sharded_destroy(runtime), SALTS_OK);
}


static void sharded_adapter_test_run_cancel_backend(
    native_io_backend_kind kind) {
  native_io_sharded *runtime = NULL;
  sharded_adapter_test_socket sockets[2];
  sharded_adapter_test_endpoint endpoint;
  sharded_adapter_test_actor fixture;
  sharded_adapter_test_token token;
  sharded_adapter_test_observe observe = {0};
  cflow_io_submit_result submitted;
  int status;

  status = sharded_adapter_test_runtime_create(
      kind, SHARDED_ADAPTER_TEST_QUEUE_CAPACITY, &runtime);
  if (status == SALTS_ENOTSUP) {
    check_equal(status, SALTS_ENOTSUP);
    return;
  }
  check_equal(status, SALTS_OK);
  check_equal(sharded_adapter_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(
      sharded_adapter_test_attach_endpoint(runtime, sockets[1], &endpoint),
      SALTS_OK);
  check_equal(
      sharded_adapter_test_actor_init(
          &fixture, runtime, SHARDED_ADAPTER_TEST_CAPACITY),
      SALTS_OK);

  submitted =
      sharded_adapter_test_submit_recv(&fixture, &endpoint, &token);
  check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
  {
    const cflow_io_run_result run = cflow_io_actor_run_ready(
        &fixture.actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
    check_true(
        run.status == CFLOW_IO_RUN_PROGRESSED ||
        run.status == CFLOW_IO_RUN_IDLE);
  }
  check_equal(native_io_sharded_wait(runtime), SALTS_OK);

  check_equal(
      cflow_io_actor_try_cancel(
          &fixture.actor, submitted.request_id),
      CFLOW_IO_CANCEL_ACCEPTED);
  {
    const cflow_io_run_result run = cflow_io_actor_run_ready(
        &fixture.actor, SHARDED_ADAPTER_TEST_DRIVE_STEPS);
    check_true(
        run.status == CFLOW_IO_RUN_PROGRESSED ||
        run.status == CFLOW_IO_RUN_IDLE);
  }
  check_equal(native_io_sharded_wait(runtime), SALTS_OK);
  check_equal(sharded_adapter_test_observe_owner(runtime, &observe), SALTS_OK);
  check_true(observe.count >= 1u);

  sharded_adapter_test_deliver_and_ack(
      &fixture, submitted.request_id);
  check_equal(
      fixture.completion.completion.kind,
      CFLOW_IO_COMPLETION_CANCELLED);
  check_equal(token.released, 1);

  {
    cflow_io_native_sharded_adapter_stats stats = {0};
    check_true(cflow_io_native_sharded_adapter_get_stats(
        &fixture.adapter, &stats));
    check_equal(stats.active_bridges, (size_t)0u);
    check_equal(stats.raw_admissions, (uint64_t)1u);
    check_equal(stats.terminal_completions, (uint64_t)1u);
    check_equal(stats.cancel_routes, (uint64_t)1u);
    check_equal(stats.cancel_route_rejections, (uint64_t)0u);
    check_equal(stats.native_cancel_errors, (uint64_t)0u);
  }

  sharded_adapter_test_actor_finish(&fixture);
  sharded_adapter_test_close_socket(sockets[1]);
  sockets[1] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(
      sharded_adapter_test_release_endpoint(runtime, &endpoint),
      SALTS_OK);
  sharded_adapter_test_close_socket(sockets[0]);
  sockets[0] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(native_io_sharded_destroy(runtime), SALTS_OK);
}


static cflow_io_publisher_prepare_status
sharded_adapter_test_publisher_prepare(
    void *user, cflow_io_operation *operation, const char **error) {
  sharded_adapter_test_publisher_fixture *fixture =
      (sharded_adapter_test_publisher_fixture *)user;

  (void)error;
  if (fixture->prepared != 0u)
    return CFLOW_IO_PUBLISHER_PREPARE_DONE;
  operation->user = &fixture->token.operation;
  operation->release = sharded_adapter_test_token_release;
  ++fixture->prepared;
  return CFLOW_IO_PUBLISHER_PREPARE_OPERATION;
}

static cflow_read_status sharded_adapter_test_publisher_encode(
    void *user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion,
    void *out_value,
    const char **error) {
  static const char failed[] =
      "NativeIO Sharded Publisher received a non-success completion";
  sharded_adapter_test_publisher_fixture *fixture =
      (sharded_adapter_test_publisher_fixture *)user;

  (void)request_id;
  (void)lease_id;
  (void)operation_user;
  if (completion->kind != CFLOW_IO_COMPLETION_OK) {
    *error = failed;
    return CFLOW_READ_ERROR;
  }
  *(int *)out_value = (int)completion->bytes;
  ++fixture->encoded;
  return CFLOW_READ_VALUE;
}

static bool sharded_adapter_test_sink_value(
    void *user, const cmeta_type_desc *type, const void *value) {
  sharded_adapter_test_sink *sink =
      (sharded_adapter_test_sink *)user;

  (void)type;
  sink->value = *(const int *)value;
  ++sink->values;
  return true;
}

static void sharded_adapter_test_sink_error(
    void *user, const char *message) {
  sharded_adapter_test_sink *sink =
      (sharded_adapter_test_sink *)user;
  ++sink->errors;
  sink->error = message;
}

static void sharded_adapter_test_sink_done(void *user) {
  sharded_adapter_test_sink *sink =
      (sharded_adapter_test_sink *)user;
  ++sink->done;
}

static int sharded_adapter_test_publisher_drive(
    cflow_io_publisher_owner *owner,
    cflow_scheduler *scheduler,
    size_t *progressed) {
  size_t owner_progress = 0u;
  const int status = cflow_io_publisher_owner_run_ready(
      owner, SHARDED_ADAPTER_TEST_DRIVE_STEPS, &owner_progress);
  if (status != SALTS_OK)
    return status;
  if (progressed != NULL)
    *progressed += owner_progress;
  (void)cflow_scheduler_run_until_idle(scheduler, 0u);
  return SALTS_OK;
}

static void sharded_adapter_test_run_publisher_backend(
    native_io_backend_kind kind, bool close_pending) {
  native_io_sharded *runtime = NULL;
  sharded_adapter_test_socket sockets[2];
  sharded_adapter_test_endpoint endpoint;
  cflow_io_native_sharded_adapter adapter = {0};
  cflow_io_publisher_owner owner = {0};
  cflow_publisher source = {0};
  cflow_graph surface = {0};
  cflow_graph normalized = {0};
  cflow_scheduler scheduler = {0};
  cflow_subscription run = {0};
  sharded_adapter_test_publisher_fixture fixture = {0};
  sharded_adapter_test_sink sink_state = {0};
  cflow_subscriber_callbacks sink_callbacks = {
      sharded_adapter_test_sink_value,
      sharded_adapter_test_sink_error,
      sharded_adapter_test_sink_done,
      &sink_state};
  cflow_subscriber sink =
      cflow_subscriber_from_callbacks(&sink_callbacks);
  cflow_io_native_sharded_adapter_config adapter_config;
  cflow_io_publisher_config source_config = {0};
  sharded_adapter_test_observe observe = {0};
  bool adapter_initialized = false;
  bool endpoint_attached = false;
  bool graph_initialized = false;
  bool normalized_initialized = false;
  bool scheduler_initialized = false;
  bool subscription_open = false;
  int status;

  status = sharded_adapter_test_runtime_create(
      kind, SHARDED_ADAPTER_TEST_QUEUE_CAPACITY, &runtime);
  if (status == SALTS_ENOTSUP) {
    check_equal(status, SALTS_ENOTSUP);
    return;
  }
  check_equal(status, SALTS_OK);
  check_equal(sharded_adapter_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(
      sharded_adapter_test_attach_endpoint(runtime, sockets[1], &endpoint),
      SALTS_OK);
  endpoint_attached = true;

  adapter_config = (cflow_io_native_sharded_adapter_config){
      runtime, SHARDED_ADAPTER_TEST_CAPACITY};
  check_equal(
      cflow_io_native_sharded_adapter_init(
          &adapter, &adapter_config),
      SALTS_OK);
  adapter_initialized = true;

  fixture.token.operation = (native_io_sharded_operation){
      .kind = NATIVE_IO_OPERATION_STREAM_RECV,
      .endpoint = endpoint.endpoint,
      .buffer = fixture.token.buffer,
      .length = sizeof(fixture.token.buffer),
      .user_data = 0x4815u};

  source_config.name = "native-io-sharded-publisher";
  source_config.type = &cmeta_type_int;
  source_config.backend =
      cflow_io_native_sharded_adapter_actor_ops();
  source_config.backend_user = &adapter;
  source_config.prepare =
      sharded_adapter_test_publisher_prepare;
  source_config.encode =
      sharded_adapter_test_publisher_encode;
  source_config.user = &fixture;

  cflow_graph_init(&surface, &cmeta_type_int);
  graph_initialized = true;
  check_true(cflow_graph_normalize(&normalized, &surface));
  normalized_initialized = true;
  check_true(cflow_scheduler_test_init(&scheduler));
  scheduler_initialized = true;
  check_equal(
      cflow_publisher_from_io_actor(
          &source, &owner, &source_config),
      SALTS_OK);
  check_true(cflow_subscribe(
      &run, &normalized, &source, &scheduler, &sink));
  subscription_open = true;

  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  {
    cflow_io_native_sharded_adapter_stats stats = {0};
    check_equal(fixture.prepared, (size_t)0u);
    check_true(cflow_io_native_sharded_adapter_get_stats(
        &adapter, &stats));
    check_equal(stats.accepted_routes, (uint64_t)0u);
  }

  check_true(cflow_subscription_request(&run, 1u));
  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  {
    size_t progressed = 0u;
    check_equal(
        sharded_adapter_test_publisher_drive(
            &owner, &scheduler, &progressed),
        SALTS_OK);
    check_greater(progressed, (size_t)0u);
  }
  check_equal(native_io_sharded_wait(runtime), SALTS_OK);
  check_equal(fixture.prepared, (size_t)1u);
  check_equal(fixture.token.released, 0);

  {
    cflow_io_native_sharded_adapter_stats stats = {0};
    check_true(cflow_io_native_sharded_adapter_get_stats(
        &adapter, &stats));
    check_equal(stats.accepted_routes, (uint64_t)1u);
    check_equal(stats.raw_admissions, (uint64_t)1u);
    check_equal(stats.active_bridges, (size_t)1u);
  }

  if (close_pending) {
    cflow_subscription_close(&run);
    subscription_open = false;

    for (size_t attempts = 0u;
         attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS;
         ++attempts) {
      size_t progressed = 0u;
      check_equal(
          sharded_adapter_test_publisher_drive(
              &owner, &scheduler, &progressed),
          SALTS_OK);
      if (progressed == 0u)
        break;
    }
    check_equal(native_io_sharded_wait(runtime), SALTS_OK);
    check_equal(
        sharded_adapter_test_observe_owner(runtime, &observe),
        SALTS_OK);
  } else {
    check_equal(
        sharded_adapter_test_peer_send(
            sockets[0], (const unsigned char *)"abcdefgh",
            sizeof(fixture.token.buffer)),
        SALTS_OK);
    check_equal(
        sharded_adapter_test_observe_owner(runtime, &observe),
        SALTS_OK);
  }
  check_true(observe.count >= 1u);

  for (size_t attempts = 0u;
       attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS &&
       !cflow_io_publisher_owner_is_quiescent(&owner);
       ++attempts) {
    check_equal(
        sharded_adapter_test_publisher_drive(
            &owner, &scheduler, NULL),
        SALTS_OK);
  }

  if (close_pending) {
    check_true(cflow_io_publisher_owner_is_quiescent(&owner));
    check_equal(fixture.encoded, (size_t)0u);
    check_equal(sink_state.values, (size_t)0u);
    check_equal(fixture.token.released, 1);
  } else {
    for (size_t attempts = 0u;
         attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS &&
         sink_state.values == 0u;
         ++attempts) {
      check_equal(
          sharded_adapter_test_publisher_drive(
              &owner, &scheduler, NULL),
          SALTS_OK);
    }
    check_equal(fixture.encoded, (size_t)1u);
    check_equal(sink_state.values, (size_t)1u);
    check_equal(sink_state.value, (int)sizeof(fixture.token.buffer));
    check_equal(sink_state.errors, (size_t)0u);
    check_equal(fixture.token.released, 1);

    cflow_subscription_close(&run);
    subscription_open = false;
    for (size_t attempts = 0u;
         attempts < SHARDED_ADAPTER_TEST_DRIVE_STEPS &&
         !cflow_io_publisher_owner_is_quiescent(&owner);
         ++attempts) {
      check_equal(
          sharded_adapter_test_publisher_drive(
              &owner, &scheduler, NULL),
          SALTS_OK);
    }
    check_true(cflow_io_publisher_owner_is_quiescent(&owner));
  }

  check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);

  if (adapter_initialized) {
    status = cflow_io_native_sharded_adapter_close(&adapter);
    check_true(status == SALTS_OK || status == SALTS_EALREADY);
    check_equal(
        cflow_io_native_sharded_adapter_destroy(&adapter),
        SALTS_OK);
    adapter_initialized = false;
  }
  if (subscription_open)
    cflow_subscription_close(&run);
  if (scheduler_initialized)
    cflow_scheduler_destroy(&scheduler);
  if (normalized_initialized)
    cflow_graph_destroy(&normalized);
  if (graph_initialized)
    cflow_graph_destroy(&surface);

  sharded_adapter_test_close_socket(sockets[1]);
  sockets[1] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  if (endpoint_attached)
    check_equal(
        sharded_adapter_test_release_endpoint(runtime, &endpoint),
        SALTS_OK);
  sharded_adapter_test_close_socket(sockets[0]);
  sockets[0] = SHARDED_ADAPTER_TEST_INVALID_SOCKET;
  check_equal(native_io_sharded_destroy(runtime), SALTS_OK);
}

spec("CFlow NativeIO Sharded IO Actor bridge") {
  it("preserves same-owner direct and cross-owner bounded routing") {
    native_io_backend_kind backends[2];
    size_t count = 0u;

    check_equal(sharded_adapter_test_network_start(), SALTS_OK);
    (void)sharded_adapter_test_backends(backends, &count);
    for (size_t index = 0u; index < count; ++index) {
      sharded_adapter_test_run_one_backend(backends[index], true);
      sharded_adapter_test_run_one_backend(backends[index], false);
    }
    sharded_adapter_test_network_stop();
  }

  it("turns cross-owner queue saturation into explicit Actor failure") {
    native_io_backend_kind backends[2];
    size_t count = 0u;

    check_equal(sharded_adapter_test_network_start(), SALTS_OK);
    (void)sharded_adapter_test_backends(backends, &count);
    for (size_t index = 0u; index < count; ++index)
      sharded_adapter_test_run_full_backend(backends[index]);
    sharded_adapter_test_network_stop();
  }

  it("keeps cancellation non-terminal until owner observe") {
    native_io_backend_kind backends[2];
    size_t count = 0u;

    check_equal(sharded_adapter_test_network_start(), SALTS_OK);
    (void)sharded_adapter_test_backends(backends, &count);
    for (size_t index = 0u; index < count; ++index)
      sharded_adapter_test_run_cancel_backend(backends[index]);
    sharded_adapter_test_network_stop();
  }

  it("maps Reactive demand onto the same Sharded Actor backend") {
    native_io_backend_kind backends[2];
    size_t count = 0u;

    check_equal(sharded_adapter_test_network_start(), SALTS_OK);
    (void)sharded_adapter_test_backends(backends, &count);
    for (size_t index = 0u; index < count; ++index)
      sharded_adapter_test_run_publisher_backend(
          backends[index], false);
    sharded_adapter_test_network_stop();
  }

  it("drains a pending Reactive request through NativeIO terminal on close") {
    native_io_backend_kind backends[2];
    size_t count = 0u;

    check_equal(sharded_adapter_test_network_start(), SALTS_OK);
    (void)sharded_adapter_test_backends(backends, &count);
    for (size_t index = 0u; index < count; ++index)
      sharded_adapter_test_run_publisher_backend(
          backends[index], true);
    sharded_adapter_test_network_stop();
  }
}


