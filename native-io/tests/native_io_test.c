#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200809L
#endif

#include <salts/error_codes.h>
#include <salts/native_io.h>
#include <salts/native_io_ace_token.h>
#include <salts/thread.h>
#include <salts/clock.h>
#include <coro.h>

#include "tinytest.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>

typedef SOCKET native_io_test_socket;
typedef int native_io_test_socklen;
  #define NATIVE_IO_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <unistd.h>

typedef int native_io_test_socket;
typedef socklen_t native_io_test_socklen;
  #define NATIVE_IO_TEST_INVALID_SOCKET (-1)
#endif

NATIVE_IO_ACE_TOKEN_TYPE(native_io_test_ace_read, unsigned char);

enum {
  NATIVE_IO_TEST_ENDPOINT_CAPACITY = 2,
  NATIVE_IO_TEST_REQUEST_CAPACITY = 3,
  NATIVE_IO_TEST_BATCH_CAPACITY = 3,
  NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY = 4096,
  NATIVE_IO_TEST_TIMEOUT_MS = 5000,
  NATIVE_IO_TEST_MAX_BACKENDS = 2
};

static int native_io_test_last_error(void) {
#if defined(_WIN32)
  return -(int)WSAGetLastError();
#else
  return -errno;
#endif
}

static void native_io_test_close_socket(native_io_test_socket socket_value) {
  if (socket_value == NATIVE_IO_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static size_t native_io_test_backends(native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS]) {
#if defined(_WIN32)
  backends[0] = NATIVE_IO_BACKEND_IOCP;
  return 1u;
#elif defined(__linux__)
  backends[0] = NATIVE_IO_BACKEND_EPOLL;
  backends[1] = NATIVE_IO_BACKEND_IO_URING;
  return 2u;
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
       defined(__DragonFly__)) && \
    UINTPTR_MAX > UINT32_MAX
  backends[0] = NATIVE_IO_BACKEND_KQUEUE;
  return 1u;
#else
  (void)backends;
  return 0u;
#endif
}

#if !defined(_WIN32)
static size_t
native_io_test_readiness_backends(native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS]) {
#if defined(__linux__)
  backends[0] = NATIVE_IO_BACKEND_EPOLL;
  return 1u;
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
       defined(__DragonFly__)) && \
    UINTPTR_MAX > UINT32_MAX
  backends[0] = NATIVE_IO_BACKEND_KQUEUE;
  return 1u;
#else
  (void)backends;
  return 0u;
#endif
}

static int native_io_test_observe_all(native_io_backend *backend,
                                      native_io_completion *events, size_t expected);

static int native_io_test_make_pipe(int descriptors[2], bool nonblocking) {
  int flags;
  if (pipe(descriptors) != 0) return -errno;
  if (!nonblocking) return SALTS_OK;
  flags = fcntl(descriptors[0], F_GETFL, 0);
  if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0) goto failed;
  flags = fcntl(descriptors[1], F_GETFL, 0);
  if (flags < 0 || fcntl(descriptors[1], F_SETFL, flags | O_NONBLOCK) != 0) goto failed;
  return SALTS_OK;

failed:
  flags = errno;
  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  descriptors[0] = -1;
  descriptors[1] = -1;
  return -flags;
}

static void native_io_test_pipe_round_trip(native_io_backend_kind kind, bool nonblocking) {
  static const unsigned char payload[] = {0x41u, 0x42u, 0x43u, 0x44u};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 2u, 2u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoints[2] = {0};
  native_io_endpoint duplicate = {1u, 1u};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  native_io_test_ace_read token = {0};
  native_io_completion stale;
  unsigned char *settled_context = NULL;
  unsigned char received[sizeof(payload)] = {0};
  const uint32_t flags = NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE;
  native_io_operation operations[2];

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, nonblocking), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0], flags,
                                           &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0], flags,
                                           &duplicate),
              SALTS_EALREADY);
  check_false(native_io_endpoint_valid(duplicate));
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1], flags,
                                           &endpoints[1]),
              SALTS_OK);
  operations[0] = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                       .endpoint = endpoints[0],
                                       .buffer = received,
                                       .length = sizeof(received),
                                       .user_data = 41u};
  operations[1] = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_WRITE,
                                       .endpoint = endpoints[1],
                                       .buffer = (void *)payload,
                                       .length = sizeof(payload),
                                       .user_data = 42u};
  check_equal(native_io_backend_submit(&backend, &operations[0], &requests[0]), SALTS_OK);
  check_equal(native_io_test_ace_read_bind(&token, requests[0], endpoints[0],
                                          operations[0].user_data, received), SALTS_OK);
  check_equal(native_io_test_ace_read_bind(&token, requests[0], endpoints[0],
                                          operations[0].user_data, received), SALTS_EBUSY);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[0]), SALTS_EBUSY);
  check_equal(native_io_backend_submit(&backend, &operations[1], &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  check_equal(events[0].kind, NATIVE_IO_COMPLETION_OK);
  check_equal(events[1].kind, NATIVE_IO_COMPLETION_OK);
  check_equal(memcmp(received, payload, sizeof(payload)), 0);
  /* Completion order is backend-dependent; match the authoritative request. */
  size_t read_index = 0u;
  if (events[0].request.slot != requests[0].slot ||
      events[0].request.generation != requests[0].generation)
    read_index = 1u;
  stale = events[read_index];
  ++stale.request.generation;
  check_equal(native_io_test_ace_read_settle(&token, &stale, &settled_context),
              SALTS_ENOENT);
  check_null(settled_context);
  check_equal(native_io_test_ace_read_settle(&token, &events[read_index], &settled_context),
              SALTS_OK);
  check_true(settled_context == received);
  check_equal(native_io_test_ace_read_settle(&token, &events[0], &settled_context),
              SALTS_EALREADY);
  check_null(settled_context);

  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[0]), SALTS_OK);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[1]), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_pipe_vector_round_trip(native_io_backend_kind kind,
                                                  bool nonblocking) {
  unsigned char first[] = {0x71u, 0x72u};
  unsigned char second[] = {0x73u, 0x74u};
  unsigned char expected[] = {0x71u, 0x72u, 0x73u, 0x74u};
  native_io_buffer_span spans[2] = {{first, sizeof(first)}, {second, sizeof(second)}};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 2u, 2u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoints[2] = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[sizeof(expected)] = {0};
  native_io_operation read_operation;
  native_io_vector_operation write_operation;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, nonblocking), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[1]),
              SALTS_OK);
  check_true(native_io_backend_endpoint_supports_vector_write(&backend, endpoints[1]));
  read_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                        .endpoint = endpoints[0],
                                        .buffer = received,
                                        .length = sizeof(received),
                                        .user_data = 71u};
  write_operation = (native_io_vector_operation){NATIVE_IO_OPERATION_PIPE_WRITE, endpoints[1],
                                                 spans, 2u, 72u};
  check_equal(native_io_backend_submit(&backend, &read_operation, &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit_vector(&backend, &write_operation, &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  check_equal(memcmp(received, expected, sizeof(expected)), 0);

  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[0]), SALTS_OK);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[1]), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}


static int native_io_test_fill_pipe(int descriptor, size_t *out_bytes) {
  unsigned char filler[NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY];
  size_t total = 0u;
  memset(filler, 0xceu, sizeof(filler));
  for (;;) {
    ssize_t written = write(descriptor, filler, sizeof(filler));
    if (written > 0) {
      total += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      *out_bytes = total;
      return SALTS_OK;
    }
    return written < 0 ? -errno : SALTS_EIO;
  }
}

static int native_io_test_read_exact_fd(int descriptor, void *buffer, size_t length) {
  size_t offset = 0u;
  while (offset < length) {
    ssize_t received = read(descriptor, (unsigned char *)buffer + offset, length - offset);
    if (received > 0) {
      offset += (size_t)received;
      continue;
    }
    if (received < 0 && errno == EINTR) continue;
    return received < 0 ? -errno : SALTS_EIO;
  }
  return SALTS_OK;
}

static void native_io_test_vector_pipe_fifo(native_io_backend_kind kind, bool nonblocking) {
  unsigned char first_a = 0x11u;
  unsigned char first_b = 0x12u;
  unsigned char second_a = 0x21u;
  unsigned char second_b = 0x22u;
  unsigned char expected[4] = {first_a, first_b, second_a, second_b};
  native_io_buffer_span first_spans[2] = {{&first_a, 1u}, {&first_b, 1u}};
  native_io_buffer_span second_spans[2] = {{&second_a, 1u}, {&second_b, 1u}};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 2u, 2u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoint = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[4] = {0};
  native_io_vector_operation writes[2];
  bool saw_first = false;
  bool saw_second = false;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, nonblocking), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  writes[0] = (native_io_vector_operation){
      NATIVE_IO_OPERATION_PIPE_WRITE, endpoint, first_spans, 2u, 101u};
  writes[1] = (native_io_vector_operation){
      NATIVE_IO_OPERATION_PIPE_WRITE, endpoint, second_spans, 2u, 102u};
  check_equal(native_io_backend_submit_vector(&backend, &writes[0], &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit_vector(&backend, &writes[1], &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  for (size_t index = 0u; index < 2u; ++index) {
    check_equal(events[index].kind, NATIVE_IO_COMPLETION_OK);
    if (events[index].user_data == 101u) saw_first = true;
    if (events[index].user_data == 102u) saw_second = true;
  }
  check_true(saw_first);
  check_true(saw_second);
  check_equal(native_io_test_read_exact_fd(descriptors[0], received, sizeof(received)), SALTS_OK);
  check_equal(memcmp(received, expected, sizeof(expected)), 0);

  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_readiness_vector_cancel(native_io_backend_kind kind) {
  unsigned char first = 0x31u;
  unsigned char second = 0x32u;
  native_io_buffer_span spans[2] = {{&first, 1u}, {&second, 1u}};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  native_io_test_ace_read act = {0};
  unsigned char *context = NULL;
  native_io_vector_operation operation;
  size_t filled = 0u;
  size_t count = 0u;

  check_equal(native_io_test_make_pipe(descriptors, true), SALTS_OK);
  check_equal(native_io_test_fill_pipe(descriptors[1], &filled), SALTS_OK);
  check_greater(filled, 0u);
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  operation = (native_io_vector_operation){
      NATIVE_IO_OPERATION_PIPE_WRITE, endpoint, spans, 2u, 111u};
  check_equal(native_io_backend_submit_vector(&backend, &operation, &request), SALTS_OK);
  /* ACT binds the already admitted request; cancel does not settle it. */
  check_equal(native_io_test_ace_read_bind(&act, request, endpoint,
                                          operation.user_data, &first), SALTS_OK);
  check_equal(native_io_backend_cancel(&backend, request), SALTS_OK);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_EBUSY);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_CANCELLED);
  check_equal(event.status, SALTS_ECANCELED);
  check_equal(event.user_data, (uintptr_t)111u);
  check_equal(native_io_test_ace_read_settle(&act, &event, &context), SALTS_OK);
  check_true(context == &first);
  check_equal(native_io_test_ace_read_settle(&act, &event, &context), SALTS_EALREADY);
  check_null(context);

  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_readiness_vector_partial_prefix(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  native_io_vector_operation operation;
  native_io_buffer_span spans[2];
  unsigned char discard[NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY];
  unsigned char *payload = NULL;
  unsigned char *observed = NULL;
  long pipe_buf_value;
  size_t pipe_buf;
  size_t total;
  size_t filled = 0u;
  size_t drained = 0u;

  check_equal(native_io_test_make_pipe(descriptors, true), SALTS_OK);
  pipe_buf_value = fpathconf(descriptors[1], _PC_PIPE_BUF);
  check_greater(pipe_buf_value, 0);
  pipe_buf = (size_t)pipe_buf_value;
  check_true(pipe_buf <= (size_t)UINT32_MAX / 2u);
  total = pipe_buf * 2u;
  payload = (unsigned char *)malloc(total);
  observed = (unsigned char *)malloc(total);
  check_not_null(payload);
  check_not_null(observed);
  if (payload == NULL || observed == NULL) goto cleanup;
  memset(payload, 0x41, pipe_buf);
  memset(payload + pipe_buf, 0x42, pipe_buf);
  spans[0] = (native_io_buffer_span){payload, pipe_buf};
  spans[1] = (native_io_buffer_span){payload + pipe_buf, pipe_buf};

  check_equal(native_io_test_fill_pipe(descriptors[1], &filled), SALTS_OK);
  check_greater_equal(filled, pipe_buf);
  while (drained < pipe_buf) {
    const size_t remaining = pipe_buf - drained;
    const size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
    ssize_t got = read(descriptors[0], discard, chunk);
    if (got < 0 && errno == EINTR) continue;
    check_greater(got, 0);
    if (got <= 0) goto cleanup;
    drained += (size_t)got;
  }

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  operation = (native_io_vector_operation){
      NATIVE_IO_OPERATION_PIPE_WRITE, endpoint, spans, 2u, 121u};
  check_equal(native_io_backend_submit_vector(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, &event, 1u), SALTS_OK);
  check_equal(event.kind, NATIVE_IO_COMPLETION_OK);
  check_equal(event.user_data, (uintptr_t)121u);
  check_greater(event.bytes, 0u);
  check_less(event.bytes, total);
  check_true(spans[0].data == payload);
  check_equal(spans[0].length, pipe_buf);
  check_true(spans[1].data == payload + pipe_buf);
  check_equal(spans[1].length, pipe_buf);

  {
    size_t filler_remaining = filled - drained;
    while (filler_remaining != 0u) {
      const size_t chunk =
          filler_remaining < sizeof(discard) ? filler_remaining : sizeof(discard);
      check_equal(native_io_test_read_exact_fd(descriptors[0], discard, chunk), SALTS_OK);
      filler_remaining -= chunk;
    }
  }
  check_equal(native_io_test_read_exact_fd(descriptors[0], observed, event.bytes), SALTS_OK);
  check_equal(memcmp(observed, payload, event.bytes), 0);

cleanup:
  if (native_io_endpoint_valid(endpoint)) {
    (void)close(descriptors[1]);
    descriptors[1] = -1;
    check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  }
  if (descriptors[0] >= 0) (void)close(descriptors[0]);
  if (descriptors[1] >= 0) (void)close(descriptors[1]);
  free(observed);
  free(payload);
}

#if defined(__linux__)
static void native_io_test_uring_vector_cancel_queued(void) {
  unsigned char first_a = 0x51u;
  unsigned char first_b = 0x52u;
  unsigned char second_a = 0x61u;
  unsigned char second_b = 0x62u;
  native_io_buffer_span first_spans[2] = {{&first_a, 1u}, {&first_b, 1u}};
  native_io_buffer_span second_spans[2] = {{&second_a, 1u}, {&second_b, 1u}};
  native_io_backend backend = {0};
  const native_io_backend_config config = {NATIVE_IO_BACKEND_IO_URING, 1u, 2u, 2u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoint = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  native_io_vector_operation writes[2];
  bool saw_cancelled = false;
  bool saw_first = false;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, false), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  writes[0] = (native_io_vector_operation){
      NATIVE_IO_OPERATION_PIPE_WRITE, endpoint, first_spans, 2u, 131u};
  writes[1] = (native_io_vector_operation){
      NATIVE_IO_OPERATION_PIPE_WRITE, endpoint, second_spans, 2u, 132u};
  check_equal(native_io_backend_submit_vector(&backend, &writes[0], &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit_vector(&backend, &writes[1], &requests[1]), SALTS_OK);
  check_equal(native_io_backend_cancel(&backend, requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  for (size_t index = 0u; index < 2u; ++index) {
    if (events[index].user_data == 131u) {
      saw_first = true;
      check_equal(events[index].kind, NATIVE_IO_COMPLETION_OK);
    } else if (events[index].user_data == 132u) {
      saw_cancelled = true;
      check_equal(events[index].kind, NATIVE_IO_COMPLETION_CANCELLED);
      check_equal(events[index].status, SALTS_ECANCELED);
    }
  }
  check_true(saw_first);
  check_true(saw_cancelled);

  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}
#endif

static void native_io_test_pipe_eof_and_reuse(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  int first[2] = {-1, -1};
  int second[2] = {-1, -1};
  native_io_endpoint old_endpoint = {0};
  native_io_endpoint new_endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  unsigned char byte = 0u;
  size_t count = 0u;
  native_io_operation operation;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(first, true), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)first[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &old_endpoint),
              SALTS_OK);
  (void)close(first[1]);
  first[1] = -1;
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                   .endpoint = old_endpoint,
                                   .buffer = &byte,
                                   .length = sizeof(byte),
                                   .user_data = 51u};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_EOF);
  check_equal(event.status, SALTS_EOF);
  (void)close(first[0]);
  first[0] = -1;
  check_equal(native_io_backend_release_pipe(&backend, old_endpoint), SALTS_OK);
  check_equal(native_io_backend_release_pipe(&backend, old_endpoint), SALTS_ENOENT);

  check_equal(native_io_test_make_pipe(second, true), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)second[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &new_endpoint),
              SALTS_OK);
  check_equal(new_endpoint.slot, old_endpoint.slot);
  check_not_equal(new_endpoint.generation, old_endpoint.generation);
  operation.endpoint = old_endpoint;
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_ENOENT);
  check_false(native_io_request_valid(request));
  (void)close(second[0]);
  (void)close(second[1]);
  check_equal(native_io_backend_release_pipe(&backend, new_endpoint), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_pipe_fifo_and_cancel(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 4u, 4u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoints[2] = {0};
  native_io_request reads[3] = {0};
  native_io_request write_request = {0};
  native_io_completion events[4] = {0};
  unsigned char received[3] = {0};
  unsigned char payload[2] = {0x61u, 0x62u};
  native_io_operation read_operation;
  native_io_operation write_operation;
  size_t read_completion_index = 0u;
  uintptr_t read_order[2] = {0u, 0u};

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, true), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[1]),
              SALTS_OK);
  read_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                        .endpoint = endpoints[0],
                                        .buffer = &received[0],
                                        .length = 1u,
                                        .user_data = 61u};
  check_equal(native_io_backend_submit(&backend, &read_operation, &reads[0]), SALTS_OK);
  read_operation.buffer = &received[1];
  read_operation.user_data = 62u;
  check_equal(native_io_backend_submit(&backend, &read_operation, &reads[1]), SALTS_OK);
  read_operation.buffer = &received[2];
  read_operation.user_data = 63u;
  check_equal(native_io_backend_submit(&backend, &read_operation, &reads[2]), SALTS_OK);
  check_equal(native_io_backend_cancel(&backend, reads[1]), SALTS_OK);
  write_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_WRITE,
                                         .endpoint = endpoints[1],
                                         .buffer = payload,
                                         .length = sizeof(payload),
                                         .user_data = 64u};
  check_equal(native_io_backend_submit(&backend, &write_operation, &write_request), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 4u), SALTS_OK);
  for (size_t index = 0u; index < 4u; ++index) {
    if (events[index].user_data == 62u)
      check_equal(events[index].kind, NATIVE_IO_COMPLETION_CANCELLED);
    if (events[index].user_data == 61u || events[index].user_data == 63u)
      read_order[read_completion_index++] = events[index].user_data;
  }
  check_equal(read_completion_index, 2u);
  check_equal(read_order[0], (uintptr_t)61u);
  check_equal(read_order[1], (uintptr_t)63u);
  check_equal(received[0], payload[0]);
  check_equal(received[2], payload[1]);

  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[0]), SALTS_OK);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[1]), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_readiness_pipe_capacity_and_broken_peer(
    native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoint = {0};
  native_io_endpoint rejected = {1u, 1u};
  native_io_request request = {0};
  unsigned char byte = 0x71u;
  native_io_operation operation;
  sigset_t pending;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, true), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &rejected),
              SALTS_ENOBUFS);
  check_false(native_io_endpoint_valid(rejected));
  (void)close(descriptors[0]);
  descriptors[0] = -1;
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_WRITE,
                                   .endpoint = endpoint,
                                   .buffer = &byte,
                                   .length = sizeof(byte)};
  check_equal(native_io_backend_submit(&backend, &operation, &request), -EPIPE);
  check_false(native_io_request_valid(request));
  check_equal(sigpending(&pending), 0);
  check_equal(sigismember(&pending, SIGPIPE), 0);
  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

#if defined(__linux__)
static void native_io_test_io_uring_pipe_broken_peer(void) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {NATIVE_IO_BACKEND_IO_URING, 1u, 1u, 1u};
  int descriptors[2] = {-1, -1};
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  unsigned char byte = 0x71u;
  size_t count = 0u;
  native_io_operation operation;
  sigset_t pending;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_pipe(descriptors, false), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  (void)close(descriptors[0]);
  descriptors[0] = -1;
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_WRITE,
                                   .endpoint = endpoint,
                                   .buffer = &byte,
                                   .length = sizeof(byte)};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS,
                                       &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_FAILED);
  check_equal(event.status, -EPIPE);
  check_equal(event.native_status, (uint32_t)EPIPE);
  check_equal(sigpending(&pending), 0);
  check_equal(sigismember(&pending, SIGPIPE), 0);

  (void)close(descriptors[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}
#endif
#endif

static int native_io_test_bind_loopback(native_io_test_socket socket_value,
                                        struct sockaddr_in *address) {
  native_io_test_socklen address_length = (native_io_test_socklen)sizeof(*address);
  memset(address, 0, sizeof(*address));
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address->sin_port = 0;
  if (bind(socket_value, (const struct sockaddr *)address,
           (native_io_test_socklen)sizeof(*address)) != 0)
    return native_io_test_last_error();
  if (getsockname(socket_value, (struct sockaddr *)address, &address_length) != 0)
    return native_io_test_last_error();
  return SALTS_OK;
}

static int native_io_test_make_tcp_pair(native_io_test_socket sockets[2]) {
  native_io_test_socket listener = NATIVE_IO_TEST_INVALID_SOCKET;
  struct sockaddr_in address;
  int status;

  sockets[0] = NATIVE_IO_TEST_INVALID_SOCKET;
  sockets[1] = NATIVE_IO_TEST_INVALID_SOCKET;
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == NATIVE_IO_TEST_INVALID_SOCKET) return native_io_test_last_error();
  status = native_io_test_bind_loopback(listener, &address);
  if (status == SALTS_OK && listen(listener, 1) != 0) status = native_io_test_last_error();
  if (status == SALTS_OK) {
    sockets[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockets[0] == NATIVE_IO_TEST_INVALID_SOCKET) status = native_io_test_last_error();
  }
  if (status == SALTS_OK && connect(sockets[0], (const struct sockaddr *)&address,
                                    (native_io_test_socklen)sizeof(address)) != 0)
    status = native_io_test_last_error();
  if (status == SALTS_OK) {
    sockets[1] = accept(listener, NULL, NULL);
    if (sockets[1] == NATIVE_IO_TEST_INVALID_SOCKET) status = native_io_test_last_error();
  }
  native_io_test_close_socket(listener);
  if (status != SALTS_OK) {
    native_io_test_close_socket(sockets[0]);
    native_io_test_close_socket(sockets[1]);
    sockets[0] = NATIVE_IO_TEST_INVALID_SOCKET;
    sockets[1] = NATIVE_IO_TEST_INVALID_SOCKET;
  }
  return status;
}

static int native_io_test_make_tcp_connect_fixture(native_io_test_socket *listener,
                                                   native_io_test_socket *client,
                                                   struct sockaddr_in *address) {
  int status;
  *listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  *client = NATIVE_IO_TEST_INVALID_SOCKET;
  if (*listener == NATIVE_IO_TEST_INVALID_SOCKET) return native_io_test_last_error();
  status = native_io_test_bind_loopback(*listener, address);
  if (status == SALTS_OK && listen(*listener, 1) != 0) status = native_io_test_last_error();
  if (status == SALTS_OK) {
#if defined(_WIN32)
    *client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0u, WSA_FLAG_OVERLAPPED);
#else
    int flags;
    *client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*client != NATIVE_IO_TEST_INVALID_SOCKET) {
      flags = fcntl(*client, F_GETFL, 0);
      if (flags < 0 || fcntl(*client, F_SETFL, flags | O_NONBLOCK) != 0)
        status = native_io_test_last_error();
    }
#endif
    if (*client == NATIVE_IO_TEST_INVALID_SOCKET) status = native_io_test_last_error();
  }
  if (status != SALTS_OK) {
    native_io_test_close_socket(*listener);
    native_io_test_close_socket(*client);
    *listener = NATIVE_IO_TEST_INVALID_SOCKET;
    *client = NATIVE_IO_TEST_INVALID_SOCKET;
  }
  return status;
}

static int native_io_test_make_udp_pair(native_io_test_socket sockets[2],
                                        struct sockaddr_in addresses[2]) {
  int status = SALTS_OK;
  sockets[0] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockets[0] == NATIVE_IO_TEST_INVALID_SOCKET) return native_io_test_last_error();
  sockets[1] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockets[1] == NATIVE_IO_TEST_INVALID_SOCKET) status = native_io_test_last_error();
  if (status == SALTS_OK) status = native_io_test_bind_loopback(sockets[0], &addresses[0]);
  if (status == SALTS_OK) status = native_io_test_bind_loopback(sockets[1], &addresses[1]);
  if (status != SALTS_OK) {
    native_io_test_close_socket(sockets[0]);
    native_io_test_close_socket(sockets[1]);
    sockets[0] = NATIVE_IO_TEST_INVALID_SOCKET;
    sockets[1] = NATIVE_IO_TEST_INVALID_SOCKET;
  }
  return status;
}

static int native_io_test_observe_all(native_io_backend *backend, native_io_completion *events,
                                      size_t expected) {
  size_t total = 0u;
  while (total < expected) {
    size_t count = 0u;
    int status = native_io_backend_observe(backend, events + total, expected - total,
                                          NATIVE_IO_TEST_TIMEOUT_MS, &count);
    if (status != SALTS_OK) return status;
    total += count;
  }
  return SALTS_OK;
}

typedef struct native_io_test_wake_owner {
  native_io_backend backend;
  native_io_backend_kind kind;
  atomic_int stage;
  int init_status;
  int first_status;
  size_t first_count;
  int zero_status;
  size_t zero_count;
  int second_status;
  size_t second_count;
  int close_status;
  int destroy_status;
} native_io_test_wake_owner;

static void native_io_test_wake_owner_run(void *user) {
  native_io_test_wake_owner *owner = (native_io_test_wake_owner *)user;
  const native_io_backend_config config = {owner->kind, 1u, 1u, 1u};
  native_io_completion event = {0};

  owner->init_status = native_io_backend_init(&owner->backend, &config);
  if (owner->init_status != SALTS_OK) {
    atomic_store_explicit(&owner->stage, 3, memory_order_release);
    return;
  }

  /*
   * Keep the owner out of observe until the controller has issued the whole
   * coalescing burst. Otherwise the first wake can be consumed between
   * sequential controller calls and a later call legitimately becomes a
   * second wake generation.
   */
  atomic_store_explicit(&owner->stage, 1, memory_order_release);
  while (atomic_load_explicit(&owner->stage, memory_order_acquire) == 1)
    cmeta_thread_yield();

  owner->first_status =
      native_io_backend_observe(&owner->backend, &event, 1u, UINT32_MAX, &owner->first_count);
  owner->zero_count = SIZE_MAX;
  owner->zero_status =
      native_io_backend_observe(&owner->backend, &event, 1u, 0u, &owner->zero_count);

  atomic_store_explicit(&owner->stage, 2, memory_order_release);
  owner->second_status =
      native_io_backend_observe(&owner->backend, &event, 1u, UINT32_MAX, &owner->second_count);

  owner->close_status = native_io_backend_close(&owner->backend);
  atomic_store_explicit(&owner->stage, 3, memory_order_release);
  while (atomic_load_explicit(&owner->stage, memory_order_acquire) == 3) cmeta_thread_yield();
  owner->destroy_status = native_io_backend_destroy(&owner->backend);
  atomic_store_explicit(&owner->stage, 5, memory_order_release);
}

static void native_io_test_wake_coalesces(native_io_backend_kind kind) {
  native_io_test_wake_owner owner = {
      .kind = kind,
      .init_status = SALTS_EIO,
      .first_status = SALTS_EIO,
      .first_count = SIZE_MAX,
      .zero_status = SALTS_EIO,
      .zero_count = SIZE_MAX,
      .second_status = SALTS_EIO,
      .second_count = SIZE_MAX,
      .close_status = SALTS_EIO,
      .destroy_status = SALTS_EIO};
  cmeta_thread_t thread = NULL;

  atomic_init(&owner.stage, 0);
  check_equal(cmeta_thread_create(&thread, native_io_test_wake_owner_run, &owner), SALTS_OK);
  while (atomic_load_explicit(&owner.stage, memory_order_acquire) == 0) cmeta_thread_yield();

  if (atomic_load_explicit(&owner.stage, memory_order_acquire) == 1) {
    check_equal(native_io_backend_wake(&owner.backend), SALTS_OK);
    check_equal(native_io_backend_wake(&owner.backend), SALTS_OK);
    check_equal(native_io_backend_wake(&owner.backend), SALTS_OK);
    atomic_store_explicit(&owner.stage, 6, memory_order_release);
    while (atomic_load_explicit(&owner.stage, memory_order_acquire) == 6)
      cmeta_thread_yield();
  }

  if (atomic_load_explicit(&owner.stage, memory_order_acquire) == 2) {
    check_equal(native_io_backend_wake(&owner.backend), SALTS_OK);
    while (atomic_load_explicit(&owner.stage, memory_order_acquire) == 2)
      cmeta_thread_yield();
  }

  if (atomic_load_explicit(&owner.stage, memory_order_acquire) == 3) {
    check_equal(native_io_backend_wake(&owner.backend), SALTS_ESHUTDOWN);
    atomic_store_explicit(&owner.stage, 4, memory_order_release);
  }

  check_equal(cmeta_thread_join(&thread), SALTS_OK);
  check_equal(owner.init_status, SALTS_OK);
  check_equal(owner.first_status, SALTS_OK);
  check_equal(owner.first_count, 0u);
  check_equal(owner.zero_status, SALTS_ETIMEDOUT);
  check_equal(owner.zero_count, 0u);
  check_equal(owner.second_status, SALTS_OK);
  check_equal(owner.second_count, 0u);
  check_equal(owner.close_status, SALTS_OK);
  check_equal(owner.destroy_status, SALTS_OK);
  check_equal(native_io_backend_wake(&owner.backend), SALTS_EINVAL);
}

static void native_io_test_close_endpoint(native_io_backend *backend, native_io_endpoint endpoint,
                                          native_io_test_socket socket_value) {
  native_io_test_close_socket(socket_value);
  check_equal(native_io_backend_release_socket(backend, endpoint), SALTS_OK);
}

static void native_io_test_prepared_fifo(native_io_backend_kind kind) {
  enum { batch_capacity = 4, reuse_rounds = 32 };
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, batch_capacity, batch_capacity};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  const unsigned char payload[] = {0x41u, 0x42u, 0x43u};
  /* Readiness/uring queue later lane entries locally. IOCP posts every receive
   * to Winsock; its native cancellation isolation is not a staged-lane contract. */
  const bool cancel_middle = kind != NATIVE_IO_BACKEND_IOCP;
  const size_t send_size = cancel_middle ? sizeof(payload) - 1u : sizeof(payload);

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  for (size_t i = 0u; i < 2u; ++i)
    check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[i], &endpoints[i]),
                SALTS_OK);
  for (size_t round = 0u; round < reuse_rounds; ++round) {
    unsigned char received[3] = {0};
    native_io_request requests[batch_capacity] = {0};
    native_io_request rejected = {0};
    native_io_completion events[batch_capacity] = {0};
    unsigned seen = 0u;
    native_io_operation operation = {.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                     .endpoint = endpoints[1], .length = 1u};
    for (size_t i = 0u; i < 3u; ++i) {
      operation.buffer = &received[i];
      operation.user_data = i;
      check_equal(native_io_backend_prepare(&backend, &operation, &requests[i]), SALTS_OK);
    }
    /* A queued cancellation must neither consume bytes nor free its borrow early. */
    if (cancel_middle) check_equal(native_io_backend_cancel(&backend, requests[1]), SALTS_OK);
    operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_SEND,
                                      .endpoint = endpoints[0], .buffer = (void *)payload,
                                      .length = send_size, .user_data = 3u};
    check_equal(native_io_backend_prepare(&backend, &operation, &requests[3]), SALTS_OK);
    check_equal(native_io_backend_prepare(&backend, &operation, &rejected), SALTS_ENOBUFS);
    check_false(native_io_request_valid(rejected));
    check_equal(native_io_backend_release_socket(&backend, endpoints[1]), SALTS_EBUSY);
    if (round + 1u == reuse_rounds) {
      check_equal(native_io_backend_close(&backend), SALTS_OK);
      check_equal(native_io_backend_prepare(&backend, &operation, &rejected), SALTS_ESHUTDOWN);
    }
    if (round % 2u == 0u) check_equal(native_io_backend_flush(&backend), SALTS_OK);
    check_equal(native_io_test_observe_all(&backend, events, batch_capacity), SALTS_OK);
    for (size_t i = 0u; i < batch_capacity; ++i) {
      const size_t tag = events[i].user_data;
      check_less(tag, (size_t)batch_capacity);
      check_equal(seen & (1u << tag), 0u);
      seen |= 1u << tag;
      check_equal(events[i].request.slot, requests[tag].slot);
      check_equal(events[i].request.generation, requests[tag].generation);
      check_equal(events[i].kind, cancel_middle && tag == 1u ? NATIVE_IO_COMPLETION_CANCELLED
                                          : NATIVE_IO_COMPLETION_OK);
      check_equal(events[i].bytes, cancel_middle && tag == 1u ? 0u : tag == 3u ? send_size : 1u);
      check_equal(native_io_backend_cancel(&backend, requests[tag]), SALTS_ENOENT);
    }
    check_equal(seen, (1u << batch_capacity) - 1u);
    check_equal(received[0], payload[0]);
    check_equal(received[1], cancel_middle ? 0u : payload[1]);
    check_equal(received[2], payload[cancel_middle ? 1u : 2u]);
    check_equal(native_io_backend_flush(&backend), SALTS_OK);
  }
  for (size_t i = 0u; i < 2u; ++i)
    native_io_test_close_endpoint(&backend, endpoints[i], sockets[i]);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

#if defined(__linux__)
static void native_io_test_epoll_pending_fifo(void) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {NATIVE_IO_BACKEND_EPOLL, 1u, 2u, 2u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[2] = {0};
  const unsigned char payload[] = {0x41u, 0x42u};
  size_t count = 0u;
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint), SALTS_OK);
  native_io_operation operation = {.kind = NATIVE_IO_OPERATION_TCP_RECV, .endpoint = endpoint,
                                   .buffer = &received[0], .length = 1u};
  check_equal(native_io_backend_prepare(&backend, &operation, &requests[0]), SALTS_OK);
  check_equal(send(sockets[0], payload, sizeof(payload), 0), (int)sizeof(payload));
  operation.buffer = &received[1];
  check_equal(native_io_backend_prepare(&backend, &operation, &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  check_equal(memcmp(received, payload, sizeof(payload)), 0);
  /* Consume an edge while no request exists, then read across request boundaries.
   * New heads must try I/O even though the persistent registration has no new edge. */
  check_equal(send(sockets[0], payload, sizeof(payload), 0), (int)sizeof(payload));
  check_equal(native_io_backend_observe(&backend, events, 2u, 0u, &count), SALTS_ETIMEDOUT);
  for (size_t i = 0u; i < 2u; ++i) {
    operation.buffer = &received[i];
    check_equal(native_io_backend_prepare(&backend, &operation, &requests[i]), SALTS_OK);
    check_equal(native_io_test_observe_all(&backend, events, 1u), SALTS_OK);
    check_equal(events[0].kind, NATIVE_IO_COMPLETION_OK);
    check_equal(received[i], payload[i]);
  }
  native_io_test_close_socket(sockets[0]);
  native_io_test_close_socket(sockets[1]);
  const int replacement = open("/dev/null", O_RDONLY);
  check_greater_equal(replacement, 0);
  if (replacement != sockets[1]) check_equal(dup2(replacement, sockets[1]), sockets[1]);
  check_equal(native_io_backend_release_socket(&backend, endpoint), SALTS_OK);
  check_equal(close(sockets[1]), 0);
  if (replacement != sockets[1]) check_equal(close(replacement), 0);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_uring_cancel_prepared_head(void) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {NATIVE_IO_BACKEND_IO_URING, 1u, 2u, 2u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  const unsigned char payload[] = {0x41u, 0x42u};
  unsigned char received[2] = {0};
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoint), SALTS_OK);
  native_io_operation operation = {.kind = NATIVE_IO_OPERATION_TCP_SEND, .endpoint = endpoint,
                                   .length = 1u};
  for (size_t i = 0u; i < 2u; ++i) {
    operation.buffer = (void *)&payload[i];
    operation.user_data = i;
    check_equal(native_io_backend_prepare(&backend, &operation, &requests[i]), SALTS_OK);
  }
  check_equal(recv(sockets[1], received, sizeof(received), MSG_DONTWAIT), -1);
  check_true(errno == EAGAIN || errno == EWOULDBLOCK);
  check_equal(native_io_backend_cancel(&backend, requests[0]), SALTS_OK);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_flush(&backend), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  for (size_t i = 0u; i < 2u; ++i)
    check_equal(events[i].kind, events[i].user_data == 0u ? NATIVE_IO_COMPLETION_CANCELLED
                                                       : NATIVE_IO_COMPLETION_OK);
  check_equal(recv(sockets[1], received, sizeof(received), 0), 1);
  check_equal(received[0], payload[1]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[0]);
  native_io_test_close_socket(sockets[1]);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}
#endif

static void native_io_test_round_trip_tcp(native_io_backend_kind kind) {
  static const unsigned char payload[] = {0x31u, 0x32u, 0x33u, 0x34u};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, NATIVE_IO_TEST_ENDPOINT_CAPACITY,
                                          NATIVE_IO_TEST_REQUEST_CAPACITY,
                                          NATIVE_IO_TEST_BATCH_CAPACITY};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[sizeof(payload)] = {0};
  native_io_operation operations[2];
  bool saw_send = false;
  bool saw_receive = false;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  operations[0] = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                       .endpoint = endpoints[1],
                                       .buffer = received,
                                       .length = sizeof(received),
                                       .user_data = 11u};
  operations[1] = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_SEND,
                                       .endpoint = endpoints[0],
                                       .buffer = (void *)payload,
                                       .length = sizeof(payload),
                                       .user_data = 12u};
  check_equal(native_io_backend_submit(&backend, &operations[0], &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operations[1], &requests[1]), SALTS_OK);
  check_true(native_io_request_valid(requests[0]));
  check_true(native_io_request_valid(requests[1]));
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);

  for (size_t index = 0u; index < 2u; ++index) {
    check_equal(events[index].kind, NATIVE_IO_COMPLETION_OK);
    check_equal(events[index].status, SALTS_OK);
    check_equal(events[index].bytes, sizeof(payload));
    if (events[index].user_data == 11u) saw_receive = true;
    else if (events[index].user_data == 12u) saw_send = true;
  }
  check_true(saw_receive);
  check_true(saw_send);
  check_equal(memcmp(received, payload, sizeof(payload)), 0);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  check_null(backend.impl);
}

static void native_io_test_round_trip_tcp_vector(native_io_backend_kind kind) {
  unsigned char first[] = {0x81u, 0x82u};
  unsigned char second[] = {0x83u, 0x84u};
  unsigned char expected[] = {0x81u, 0x82u, 0x83u, 0x84u};
  native_io_buffer_span spans[2] = {{first, sizeof(first)}, {second, sizeof(second)}};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, NATIVE_IO_TEST_ENDPOINT_CAPACITY,
                                          NATIVE_IO_TEST_REQUEST_CAPACITY,
                                          NATIVE_IO_TEST_BATCH_CAPACITY};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[sizeof(expected)] = {0};
  native_io_operation receive_operation;
  native_io_vector_operation send_operation;
  bool saw_send = false;
  bool saw_receive = false;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  check_true(native_io_backend_endpoint_supports_vector_write(&backend, endpoints[0]));
  receive_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_STREAM_RECV,
                                           .endpoint = endpoints[1],
                                           .buffer = received,
                                           .length = sizeof(received),
                                           .user_data = 81u};
  send_operation = (native_io_vector_operation){NATIVE_IO_OPERATION_STREAM_SEND, endpoints[0],
                                                spans, 2u, 82u};
  check_equal(native_io_backend_submit(&backend, &receive_operation, &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit_vector(&backend, &send_operation, &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  for (size_t index = 0u; index < 2u; ++index) {
    check_equal(events[index].kind, NATIVE_IO_COMPLETION_OK);
    check_equal(events[index].status, SALTS_OK);
    check_equal(events[index].bytes, sizeof(expected));
    if (events[index].user_data == 81u) saw_receive = true;
    else if (events[index].user_data == 82u) saw_send = true;
  }
  check_true(saw_receive);
  check_true(saw_send);
  check_equal(memcmp(received, expected, sizeof(expected)), 0);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_tcp_connect(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket listener = NATIVE_IO_TEST_INVALID_SOCKET;
  native_io_test_socket client = NATIVE_IO_TEST_INVALID_SOCKET;
  native_io_test_socket accepted = NATIVE_IO_TEST_INVALID_SOCKET;
  struct sockaddr_in address;
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_request rejected_request = {0};
  native_io_completion event = {0};
  unsigned char receive_byte = 0u;
  native_io_operation operation;
  native_io_operation receive_operation;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_connect_fixture(&listener, &client, &address), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)client, &endpoint), SALTS_OK);
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_CONNECT,
                                   .endpoint = endpoint,
                                   .user_data = 13u,
                                   .address = &address,
                                   .address_capacity = sizeof(address),
                                   .address_length = sizeof(address)};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operation, &rejected_request), SALTS_EALREADY);
  check_false(native_io_request_valid(rejected_request));
  receive_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                            .endpoint = endpoint,
                                            .buffer = &receive_byte,
                                            .length = sizeof(receive_byte)};
  check_equal(native_io_backend_submit(&backend, &receive_operation, &rejected_request),
              SALTS_EBUSY);
  check_false(native_io_request_valid(rejected_request));
  check_equal(native_io_test_observe_all(&backend, &event, 1u), SALTS_OK);
  check_equal(event.kind, NATIVE_IO_COMPLETION_OK);
  check_equal(event.status, SALTS_OK);
  check_equal(event.bytes, 0u);
  check_equal(event.user_data, 13u);
  accepted = accept(listener, NULL, NULL);
  check_true(accepted != NATIVE_IO_TEST_INVALID_SOCKET);

  native_io_test_close_socket(accepted);
  native_io_test_close_socket(listener);
  native_io_test_close_endpoint(&backend, endpoint, client);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_tcp_connect_refused(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket listener = NATIVE_IO_TEST_INVALID_SOCKET;
  native_io_test_socket client = NATIVE_IO_TEST_INVALID_SOCKET;
  struct sockaddr_in address;
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  native_io_operation operation;
  size_t count = SIZE_MAX;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_connect_fixture(&listener, &client, &address), SALTS_OK);
  native_io_test_close_socket(listener);
  listener = NATIVE_IO_TEST_INVALID_SOCKET;
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)client, &endpoint), SALTS_OK);
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_CONNECT,
                                   .endpoint = endpoint,
                                   .user_data = 14u,
                                   .address = &address,
                                   .address_capacity = sizeof(address),
                                   .address_length = sizeof(address)};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, &event, 1u), SALTS_OK);
  check_equal(event.kind, NATIVE_IO_COMPLETION_FAILED);
  check_true(event.status < SALTS_OK);
  check_true(event.native_status != 0u);
  check_equal(event.user_data, 14u);
  check_equal(native_io_backend_observe(&backend, &event, 1u, 0u, &count), SALTS_ETIMEDOUT);
  check_equal(count, 0u);

  native_io_test_close_endpoint(&backend, endpoint, client);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_fifo_tcp_receives(native_io_backend_kind kind) {
  static const unsigned char payload[] = {0x51u, 0x52u};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 3u, 3u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_request requests[3] = {0};
  native_io_completion events[3] = {0};
  unsigned char received[2] = {0};
  native_io_operation operations[3];

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  operations[0] = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                       .endpoint = endpoints[1],
                                       .buffer = &received[0],
                                       .length = 1u,
                                       .user_data = 51u};
  operations[1] = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                       .endpoint = endpoints[1],
                                       .buffer = &received[1],
                                       .length = 1u,
                                       .user_data = 52u};
  operations[2] = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_SEND,
                                       .endpoint = endpoints[0],
                                       .buffer = (void *)payload,
                                       .length = sizeof(payload),
                                       .user_data = 53u};
  check_equal(native_io_backend_submit(&backend, &operations[0], &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operations[1], &requests[1]), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operations[2], &requests[2]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 3u), SALTS_OK);
  check_equal(received[0], payload[0]);
  check_equal(received[1], payload[1]);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_cancel_pending(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  unsigned char byte = 0u;
  native_io_operation operation;
  size_t count = 0u;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint), SALTS_OK);
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                   .endpoint = endpoint,
                                   .buffer = &byte,
                                   .length = sizeof(byte),
                                   .user_data = 21u};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_cancel(&backend, request), SALTS_OK);
  check_equal(native_io_backend_release_socket(&backend, endpoint), SALTS_EBUSY);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_CANCELLED);
  check_equal(event.status, SALTS_ECANCELED);
  check_equal(event.user_data, 21u);

  native_io_test_close_socket(sockets[0]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_cancel_same_lane(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 2u, 2u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char bytes[2] = {0};
  native_io_operation operations[2];
  int cancel_status[2];
  bool saw_first = false;
  bool saw_second = false;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint), SALTS_OK);
  for (size_t index = 0u; index < 2u; ++index) {
    operations[index] = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                             .endpoint = endpoint,
                                             .buffer = &bytes[index],
                                             .length = 1u,
                                             .user_data = 61u + index};
    check_equal(native_io_backend_submit(&backend, &operations[index], &requests[index]), SALTS_OK);
  }
  cancel_status[1] = native_io_backend_cancel(&backend, requests[1]);
  cancel_status[0] = native_io_backend_cancel(&backend, requests[0]);
  check_true(cancel_status[1] == SALTS_OK || cancel_status[1] == SALTS_EALREADY);
  check_true(cancel_status[0] == SALTS_OK || cancel_status[0] == SALTS_EALREADY);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  for (size_t index = 0u; index < 2u; ++index) {
    check_equal(events[index].kind, NATIVE_IO_COMPLETION_CANCELLED);
    check_equal(events[index].status, SALTS_ECANCELED);
    if (events[index].user_data == 61u) saw_first = true;
    else if (events[index].user_data == 62u) saw_second = true;
  }
  check_true(saw_first);
  check_true(saw_second);

  native_io_test_close_socket(sockets[0]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_round_trip_udp(native_io_backend_kind kind) {
  static const unsigned char payload[] = {0x41u, 0x42u, 0x43u};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 2u, 2u};
  native_io_test_socket sockets[2];
  struct sockaddr_in addresses[2];
  struct sockaddr_storage peer_address;
  native_io_endpoint endpoints[2] = {0};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[sizeof(payload)] = {0};
  native_io_operation receive_operation;
  native_io_operation send_operation;
  bool saw_receive = false;
  bool saw_send = false;

  memset(&peer_address, 0, sizeof(peer_address));
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_udp_pair(sockets, addresses), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  receive_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_UDP_RECV_FROM,
                                           .endpoint = endpoints[1],
                                           .buffer = received,
                                           .length = sizeof(received),
                                           .user_data = 41u,
                                           .address = &peer_address,
                                           .address_capacity = sizeof(peer_address)};
  send_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_UDP_SEND_TO,
                                        .endpoint = endpoints[0],
                                        .buffer = (void *)payload,
                                        .length = sizeof(payload),
                                        .user_data = 42u,
                                        .address = &addresses[1],
                                        .address_capacity = sizeof(addresses[1]),
                                        .address_length = sizeof(addresses[1])};
  check_equal(native_io_backend_submit(&backend, &receive_operation, &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &send_operation, &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);

  for (size_t index = 0u; index < 2u; ++index) {
    check_equal(events[index].kind, NATIVE_IO_COMPLETION_OK);
    check_equal(events[index].bytes, sizeof(payload));
    if (events[index].user_data == 41u) {
      const struct sockaddr_in *peer = (const struct sockaddr_in *)&peer_address;
      saw_receive = true;
      check_equal(events[index].address_length, sizeof(*peer));
      check_equal(peer->sin_family, AF_INET);
      check_equal(peer->sin_port, addresses[0].sin_port);
    } else if (events[index].user_data == 42u) {
      saw_send = true;
      check_equal(events[index].address_length, 0u);
    }
  }
  check_true(saw_receive);
  check_true(saw_send);
  check_equal(memcmp(received, payload, sizeof(payload)), 0);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_udp_truncation(native_io_backend_kind kind, bool connected) {
  enum { RECEIVE_BYTES = 8 };
  unsigned char payload[RECEIVE_BYTES + 1] = {0};
  unsigned char received[RECEIVE_BYTES] = {0};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket sockets[2];
  struct sockaddr_in addresses[2];
  struct sockaddr_storage peer = {0};
  native_io_endpoint endpoint = {0};
  native_io_operation operation = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  size_t count = 0u;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_udp_pair(sockets, addresses), SALTS_OK);
  if (connected)
    check_equal(connect(sockets[1], (struct sockaddr *)&addresses[0], sizeof(addresses[0])), 0);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint), SALTS_OK);
  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_UDP_RECV_FROM,
                                    .endpoint = endpoint, .buffer = received,
                                    .length = sizeof(received),
                                    .address = connected ? NULL : &peer,
                                    .address_capacity = connected ? 0u : sizeof(peer)};
  for (size_t bytes = RECEIVE_BYTES; bytes <= sizeof(payload); ++bytes) {
    check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
    check_equal(sendto(sockets[0], (const char *)payload, (int)bytes, 0,
                       (struct sockaddr *)&addresses[1], sizeof(addresses[1])), (int)bytes);
    check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
                SALTS_OK);
    check_equal(count, 1u);
    if (bytes == RECEIVE_BYTES) {
      check_equal(event.kind, NATIVE_IO_COMPLETION_OK);
      check_equal(event.bytes, sizeof(received));
    } else {
      check_equal(event.kind, NATIVE_IO_COMPLETION_FAILED);
#if defined(_WIN32)
      check_true(event.status == -(int)WSAEMSGSIZE || event.status == -(int)ERROR_MORE_DATA);
#else
      check_equal(event.status, -EMSGSIZE);
#endif
    }
  }
  native_io_test_close_socket(sockets[0]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

#if !defined(_WIN32)
enum { NATIVE_IO_TEST_SIGNAL_INTERVAL_MS = 10, NATIVE_IO_TEST_SIGNAL_DURATION_MS = 600,
       NATIVE_IO_TEST_INTERRUPTED_TIMEOUT_MS = 100, NATIVE_IO_TEST_TIMEOUT_TOLERANCE_MS = 400 };
static volatile sig_atomic_t native_io_test_signal_count;
typedef struct native_io_test_interrupts {
  pthread_t owner;
  int status;
} native_io_test_interrupts;

static void native_io_test_count_signal(int signal_number) {
  (void)signal_number;
  ++native_io_test_signal_count;
}

static void native_io_test_interrupt_owner(void *user) {
  native_io_test_interrupts *probe = (native_io_test_interrupts *)user;
  const uint64_t started_ms = cmeta_monotonic_ms();
  do {
    cmeta_sleep_ms(NATIVE_IO_TEST_SIGNAL_INTERVAL_MS);
    probe->status = pthread_kill(probe->owner, SIGUSR1);
    if (probe->status != 0) return;
  } while (cmeta_monotonic_ms() - started_ms < NATIVE_IO_TEST_SIGNAL_DURATION_MS);
}

static void native_io_test_interrupted_timeout(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_completion event = {0};
  native_io_test_interrupts probe = {.owner = pthread_self()};
  struct sigaction action = {0}, previous = {0};
  sigset_t signals, previous_mask;
  cmeta_thread_t thread;
  size_t count = SIZE_MAX;
  uint64_t started_ms, elapsed_ms;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  action.sa_handler = native_io_test_count_signal;
  sigemptyset(&action.sa_mask);
  sigemptyset(&signals);
  sigaddset(&signals, SIGUSR1);
  native_io_test_signal_count = 0;
  check_equal(sigaction(SIGUSR1, &action, &previous), 0);
  check_equal(pthread_sigmask(SIG_UNBLOCK, &signals, &previous_mask), 0);
  check_equal(cmeta_thread_create(&thread, native_io_test_interrupt_owner, &probe), SALTS_OK);
  started_ms = cmeta_monotonic_ms();
  check_equal(native_io_backend_observe(&backend, &event, 1u,
                                       NATIVE_IO_TEST_INTERRUPTED_TIMEOUT_MS, &count), SALTS_ETIMEDOUT);
  elapsed_ms = cmeta_monotonic_ms() - started_ms;
  check_equal(cmeta_thread_join(&thread), SALTS_OK);
  check_equal(pthread_sigmask(SIG_SETMASK, &previous_mask, NULL), 0);
  check_equal(sigaction(SIGUSR1, &previous, NULL), 0);
  check_equal(probe.status, 0);
  check_greater(native_io_test_signal_count, 1);
  check_equal(count, 0u);
  check_greater_equal(elapsed_ms, (uint64_t)NATIVE_IO_TEST_INTERRUPTED_TIMEOUT_MS);
  check_less(elapsed_ms, (uint64_t)NATIVE_IO_TEST_TIMEOUT_TOLERANCE_MS);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}
#endif

static void native_io_test_capacity_and_close(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_endpoint rejected_endpoint = {0};
  native_io_request request = {0};
  native_io_request rejected_request = {0};
  native_io_completion event = {0};
  native_io_backend_stats stats = {0};
  unsigned char byte = 0u;
  native_io_operation operation;
  size_t count = 99u;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &rejected_endpoint),
              SALTS_EALREADY);
  check_false(native_io_endpoint_valid(rejected_endpoint));
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &rejected_endpoint),
              SALTS_ENOBUFS);
  check_equal(native_io_backend_observe(&backend, &event, 1u, 0u, &count), SALTS_ETIMEDOUT);
  check_equal(count, 0u);

  operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                   .endpoint = endpoint,
                                   .buffer = &byte,
                                   .length = sizeof(byte),
                                   .user_data = 31u};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operation, &rejected_request), SALTS_ENOBUFS);
  check_false(native_io_request_valid(rejected_request));
  check_true(native_io_backend_get_stats(&backend, &stats));
  check_equal(stats.endpoint_count, 1u);
  check_equal(stats.active_requests, 1u);
  check_equal(stats.rejected_full, 1u);

  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operation, &rejected_request), SALTS_ESHUTDOWN);
  check_equal(native_io_backend_destroy(&backend), SALTS_EBUSY);
  check_equal(native_io_backend_cancel(&backend, request), SALTS_OK);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_CANCELLED);
  check_equal(native_io_backend_cancel(&backend, request), SALTS_ENOENT);

  native_io_test_close_socket(sockets[0]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[1]);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

typedef struct native_io_test_coroutine_receive_state {
  native_io_endpoint endpoint;
  native_io_completion completion;
  unsigned char received;
  int await_status;
  size_t entered;
  size_t resumed;
  bool prepared;
} native_io_test_coroutine_receive_state;

static void native_io_test_coroutine_receive(native_io_coroutine *coroutine, void *user_data) {
  native_io_test_coroutine_receive_state *state =
      (native_io_test_coroutine_receive_state *)user_data;
  const native_io_operation operation = {.kind = NATIVE_IO_OPERATION_TCP_RECV,
                                         .endpoint = state->endpoint,
                                         .buffer = &state->received,
                                         .length = sizeof(state->received),
                                         .user_data = 81u};
  ++state->entered;
  state->await_status = state->prepared
      ? native_io_coroutine_await_prepared(coroutine, &operation, &state->completion)
      : native_io_coroutine_await(coroutine, &operation, &state->completion);
  ++state->resumed;
}

static void native_io_test_coroutine_completion_resume(native_io_backend_kind kind, bool prepared) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 2u, 2u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_coroutine_task task = {0};
  native_io_test_coroutine_receive_state state = {0};
  native_io_request send_request = {0};
  native_io_completion events[2] = {0};
  const unsigned char sent = 0x5au;
  native_io_operation send_operation = {.kind = NATIVE_IO_OPERATION_TCP_SEND,
                                        .buffer = (void *)&sent,
                                        .length = sizeof(sent),
                                        .user_data = 82u};
  size_t direct_completions = 0u;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  state.endpoint = endpoints[1];
  state.prepared = prepared;
  check_equal(
      native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_receive, &state, &task),
      SALTS_OK);
  check_true(native_io_coroutine_task_valid(task));
  check_equal(state.entered, 1u);
  check_equal(state.resumed, 0u);

  send_operation.endpoint = endpoints[0];
  check_equal(native_io_backend_submit(&backend, &send_operation, &send_request), SALTS_OK);
  for (size_t attempt = 0u; attempt < 4u && state.resumed == 0u; ++attempt) {
    size_t count = 0u;
    check_equal(native_io_backend_observe(&backend, events, 2u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
                SALTS_OK);
    direct_completions += count;
  }

  check_equal(state.await_status, SALTS_OK);
  check_equal(state.resumed, 1u);
  check_equal(state.completion.kind, NATIVE_IO_COMPLETION_OK);
  check_equal(state.completion.user_data, (uintptr_t)81u);
  check_equal(state.received, sent);
  check_equal(direct_completions, 1u);
  check_equal(native_io_backend_cancel_coroutine(&backend, task), SALTS_ENOENT);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

typedef struct native_io_test_coroutine_vector_send_state {
  native_io_endpoint endpoint;
  native_io_completion completion;
  unsigned char first[2];
  unsigned char second[2];
  int await_status;
  size_t entered;
  size_t resumed;
} native_io_test_coroutine_vector_send_state;

static void native_io_test_coroutine_vector_send(native_io_coroutine *coroutine, void *user_data) {
  native_io_test_coroutine_vector_send_state *state =
      (native_io_test_coroutine_vector_send_state *)user_data;
  native_io_buffer_span spans[2] = {{state->first, sizeof(state->first)},
                                    {state->second, sizeof(state->second)}};
  native_io_vector_operation operation = {
      NATIVE_IO_OPERATION_STREAM_SEND, state->endpoint, spans, 2u, 83u};
  ++state->entered;
  state->await_status =
      native_io_coroutine_await_vector(coroutine, &operation, &state->completion);
  ++state->resumed;
}

static void native_io_test_coroutine_vector_completion_resume(native_io_backend_kind kind) {
  static const unsigned char expected[] = {0x91u, 0x92u, 0x93u, 0x94u};
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 2u, 2u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_request receive_request = {0};
  native_io_coroutine_task task = {0};
  native_io_completion events[2] = {0};
  unsigned char received[sizeof(expected)] = {0};
  native_io_operation receive_operation;
  native_io_test_coroutine_vector_send_state state = {
      .first = {0x91u, 0x92u}, .second = {0x93u, 0x94u}};
  size_t direct_completions = 0u;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  receive_operation = (native_io_operation){.kind = NATIVE_IO_OPERATION_STREAM_RECV,
                                           .endpoint = endpoints[1],
                                           .buffer = received,
                                           .length = sizeof(received),
                                           .user_data = 84u};
  check_equal(native_io_backend_submit(&backend, &receive_operation, &receive_request), SALTS_OK);
  state.endpoint = endpoints[0];
  check_equal(native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_vector_send,
                                                &state, &task),
              SALTS_OK);
  check_equal(state.entered, 1u);
  check_equal(state.resumed, 0u);

  for (size_t attempt = 0u;
       attempt < 4u && (state.resumed == 0u || direct_completions == 0u); ++attempt) {
    size_t count = 0u;
    check_equal(native_io_backend_observe(&backend, events, 2u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
                SALTS_OK);
    direct_completions += count;
  }

  check_equal(state.await_status, SALTS_OK);
  check_equal(state.resumed, 1u);
  check_equal(state.completion.kind, NATIVE_IO_COMPLETION_OK);
  check_equal(state.completion.status, SALTS_OK);
  check_equal(state.completion.bytes, sizeof(expected));
  check_equal(state.completion.user_data, (uintptr_t)83u);
  check_equal(direct_completions, 1u);
  check_equal(memcmp(received, expected, sizeof(expected)), 0);
  check_equal(native_io_backend_cancel_coroutine(&backend, task), SALTS_ENOENT);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

enum { NATIVE_IO_TEST_COROUTINE_LOOP_COUNT = 8 };

typedef struct native_io_test_coroutine_loop_state {
  native_io_endpoint endpoint;
  native_io_operation_kind kind;
  unsigned char bytes[NATIVE_IO_TEST_COROUTINE_LOOP_COUNT];
  size_t completed;
  int await_status;
  bool done;
} native_io_test_coroutine_loop_state;

static void native_io_test_coroutine_loop(native_io_coroutine *coroutine, void *user_data) {
  native_io_test_coroutine_loop_state *state = (native_io_test_coroutine_loop_state *)user_data;
  state->await_status = SALTS_OK;
  for (size_t index = 0u;
       state->await_status == SALTS_OK && index < NATIVE_IO_TEST_COROUTINE_LOOP_COUNT; ++index) {
    native_io_completion completion = {0};
    const native_io_operation operation = {.kind = state->kind,
                                           .endpoint = state->endpoint,
                                           .buffer = &state->bytes[index],
                                           .length = 1u,
                                           .user_data = 90u + index};
    state->await_status = native_io_coroutine_await(coroutine, &operation, &completion);
    if (state->await_status != SALTS_OK) break;
    if (completion.kind != NATIVE_IO_COMPLETION_OK || completion.status != SALTS_OK ||
        completion.bytes != 1u || completion.user_data != 90u + index) {
      state->await_status = SALTS_EPROTO;
      break;
    }
    ++state->completed;
  }
  state->done = true;
}

static void native_io_test_coroutine_reawait_batch(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 3u, 3u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_coroutine_task receive_task = {0};
  native_io_coroutine_task send_task = {0};
  native_io_test_coroutine_loop_state receive = {0};
  native_io_test_coroutine_loop_state send = {0};
  native_io_completion events[3] = {0};
  size_t direct_completions = 0u;
  int observe_status = SALTS_OK;

  memset(send.bytes, 0x5a, sizeof(send.bytes));
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoints[1]),
              SALTS_OK);
  receive.endpoint = endpoints[1];
  receive.kind = NATIVE_IO_OPERATION_TCP_RECV;
  send.endpoint = endpoints[0];
  send.kind = NATIVE_IO_OPERATION_TCP_SEND;
  check_equal(native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_loop, &receive,
                                                &receive_task),
              SALTS_OK);
  check_equal(
      native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_loop, &send, &send_task),
      SALTS_OK);

  for (size_t attempt = 0u; observe_status == SALTS_OK && (!receive.done || !send.done) &&
                            attempt < NATIVE_IO_TEST_COROUTINE_LOOP_COUNT * 4u;
       ++attempt) {
    size_t count = 0u;
    observe_status =
        native_io_backend_observe(&backend, events, 3u, NATIVE_IO_TEST_TIMEOUT_MS, &count);
    direct_completions += count;
  }

  check_equal(observe_status, SALTS_OK);
  check_true(receive.done);
  check_true(send.done);
  check_equal(receive.await_status, SALTS_OK);
  check_equal(send.await_status, SALTS_OK);
  check_equal(receive.completed, NATIVE_IO_TEST_COROUTINE_LOOP_COUNT);
  check_equal(send.completed, NATIVE_IO_TEST_COROUTINE_LOOP_COUNT);
  check_equal(direct_completions, 0u);
  check_equal(memcmp(receive.bytes, send.bytes, sizeof(send.bytes)), 0);
  check_equal(native_io_backend_cancel_coroutine(&backend, receive_task), SALTS_ENOENT);
  check_equal(native_io_backend_cancel_coroutine(&backend, send_task), SALTS_ENOENT);

  native_io_test_close_endpoint(&backend, endpoints[0], sockets[0]);
  native_io_test_close_endpoint(&backend, endpoints[1], sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_coroutine_cancel_drain(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_coroutine_task task = {0};
  native_io_coroutine_task rejected = {9u, 9u};
  native_io_test_coroutine_receive_state state = {0};
  native_io_completion event = {0};
  native_io_coroutine_stats coroutine_stats = NATIVE_IO_COROUTINE_STATS_V1_INITIALIZER;
  size_t count = 99u;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint),
              SALTS_OK);
  state.endpoint = endpoint;
  check_equal(
      native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_receive, &state, &task),
      SALTS_OK);
  check_equal(native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_receive, &state,
                                                &rejected),
              SALTS_ENOBUFS);
  check_false(native_io_coroutine_task_valid(rejected));
  check_true(native_io_backend_get_coroutine_stats(&backend, &coroutine_stats));
  check_equal(coroutine_stats.capacity, 1u);
  check_equal(coroutine_stats.active, 1u);
  check_equal(coroutine_stats.retained_frames, 1u);

  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_EBUSY);
  check_equal(native_io_backend_cancel_coroutine(&backend, task), SALTS_OK);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS, &count),
              SALTS_OK);
  check_equal(count, 0u);
  check_equal(state.resumed, 1u);
  check_equal(state.completion.kind, NATIVE_IO_COMPLETION_CANCELLED);
  check_equal(state.completion.status, SALTS_ECANCELED);
  check_equal(native_io_backend_cancel_coroutine(&backend, task), SALTS_ENOENT);
  check_true(native_io_backend_get_coroutine_stats(&backend, &coroutine_stats));
  check_equal(coroutine_stats.active, 0u);
  check_equal(coroutine_stats.retained_frames, 1u);

  native_io_test_close_socket(sockets[0]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[1]);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_coroutine_illegal_yield(native_io_coroutine *coroutine,
                                                   void *user_data) {
  (void)coroutine;
  (void)user_data;
  (void)coro_yield();
}

static void native_io_test_coroutine_protocol_error_is_reclaimed(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_coroutine_task task = {9u, 9u};
  native_io_coroutine_stats stats = NATIVE_IO_COROUTINE_STATS_V1_INITIALIZER;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  stats.abi_version = 0u;
  check_false(native_io_backend_get_coroutine_stats(&backend, &stats));
  stats = (native_io_coroutine_stats)NATIVE_IO_COROUTINE_STATS_V1_INITIALIZER;
  stats.struct_size = 0u;
  check_false(native_io_backend_get_coroutine_stats(&backend, &stats));
  stats = (native_io_coroutine_stats)NATIVE_IO_COROUTINE_STATS_V1_INITIALIZER;
  check_equal(native_io_backend_spawn_coroutine(&backend, native_io_test_coroutine_illegal_yield,
                                                NULL, &task),
              SALTS_EPROTO);
  check_false(native_io_coroutine_task_valid(task));
  check_true(native_io_backend_get_coroutine_stats(&backend, &stats));
  check_equal(stats.active, 0u);
  check_equal(stats.retained_frames, 0u);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_reject_pipe_operation_on_socket(native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 1u, 1u, 1u};
  native_io_test_socket sockets[2];
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  unsigned char byte = 0u;
  const native_io_operation operation = {.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                        .endpoint = endpoint,
                                        .buffer = &byte,
                                        .length = sizeof(byte)};
  native_io_operation submitted = operation;

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(sockets), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint),
              SALTS_OK);
  submitted.endpoint = endpoint;
  check_equal(native_io_backend_submit(&backend, &submitted, &request), SALTS_EINVAL);
  check_false(native_io_request_valid(request));

  native_io_test_close_socket(sockets[0]);
  native_io_test_close_endpoint(&backend, endpoint, sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_reject_cross_protocol_socket_operation(
    native_io_backend_kind kind) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {kind, 2u, 1u, 1u};
  native_io_test_socket tcp_sockets[2];
  native_io_test_socket udp_sockets[2];
  struct sockaddr_in udp_addresses[2];
  native_io_endpoint tcp_endpoint = {0};
  native_io_endpoint udp_endpoint = {0};
  native_io_request request = {0};
  unsigned char byte = 0u;
  native_io_operation operation = {
      .kind = NATIVE_IO_OPERATION_UDP_SEND_TO,
      .buffer = &byte,
      .length = sizeof(byte),
      .address = &udp_addresses[1],
      .address_capacity = sizeof(udp_addresses[1]),
      .address_length = sizeof(udp_addresses[1]),
  };

  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_test_make_tcp_pair(tcp_sockets), SALTS_OK);
  check_equal(native_io_test_make_udp_pair(udp_sockets, udp_addresses), SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)tcp_sockets[1],
                                              &tcp_endpoint),
              SALTS_OK);
  check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)udp_sockets[1],
                                              &udp_endpoint),
              SALTS_OK);

  operation.endpoint = tcp_endpoint;
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_EINVAL);
  check_false(native_io_request_valid(request));

  operation.kind = NATIVE_IO_OPERATION_TCP_SEND;
  operation.endpoint = udp_endpoint;
  operation.address = NULL;
  operation.address_capacity = 0u;
  operation.address_length = 0u;
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_EINVAL);
  check_false(native_io_request_valid(request));

  native_io_test_close_socket(tcp_sockets[0]);
  native_io_test_close_endpoint(&backend, tcp_endpoint, tcp_sockets[1]);
  native_io_test_close_socket(udp_sockets[0]);
  native_io_test_close_endpoint(&backend, udp_endpoint, udp_sockets[1]);
  check_equal(native_io_backend_close(&backend), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

#if defined(_WIN32)
static void native_io_test_close_pipe(HANDLE pipe_handle) {
  if (pipe_handle != NULL && pipe_handle != INVALID_HANDLE_VALUE)
    (void)CloseHandle(pipe_handle);
}

static int native_io_test_make_named_pipe_pair(HANDLE pipes[2], bool outbound_only) {
  static LONG sequence = 0;
  char name[128];
  OVERLAPPED connected = {0};
  HANDLE event = NULL;
  DWORD error = ERROR_SUCCESS;
  BOOL pending = FALSE;
  int name_length;

  pipes[0] = INVALID_HANDLE_VALUE;
  pipes[1] = INVALID_HANDLE_VALUE;
  name_length = snprintf(name, sizeof(name), "\\\\.\\pipe\\native-io-test-%lu-%ld",
                         GetCurrentProcessId(), InterlockedIncrement(&sequence));
  if (name_length < 0 || (size_t)name_length >= sizeof(name)) return SALTS_ERANGE;

  pipes[0] = CreateNamedPipeA(name,
                              (outbound_only ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_DUPLEX) |
                                  FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1u,
                              NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY,
                              NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY, 0u, NULL);
  if (pipes[0] == INVALID_HANDLE_VALUE) return -(int)GetLastError();
  event = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (event == NULL) {
    error = GetLastError();
    goto failed;
  }
  connected.hEvent = event;
  if (!ConnectNamedPipe(pipes[0], &connected)) {
    error = GetLastError();
    if (error == ERROR_IO_PENDING)
      pending = TRUE;
    else if (error != ERROR_PIPE_CONNECTED)
      goto failed;
  }
  pipes[1] = CreateFileA(name, outbound_only ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                         0u, NULL, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, NULL);
  if (pipes[1] == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    goto failed;
  }
  if (pending) {
    DWORD transferred = 0u;
    if (!GetOverlappedResult(pipes[0], &connected, &transferred, TRUE)) {
      error = GetLastError();
      goto failed;
    }
  }
  (void)CloseHandle(event);
  return SALTS_OK;

failed:
  native_io_test_close_pipe(pipes[1]);
  native_io_test_close_pipe(pipes[0]);
  if (event != NULL) (void)CloseHandle(event);
  pipes[0] = INVALID_HANDLE_VALUE;
  pipes[1] = INVALID_HANDLE_VALUE;
  return -(int)error;
}

static void native_io_test_iocp_pipe_round_trip(void) {
  static const unsigned char payload[] = {0x70u, 0x69u, 0x70u, 0x65u};
  native_io_backend backend = {0};
  const native_io_backend_config config = {NATIVE_IO_BACKEND_IOCP, 2u, 2u, 2u};
  HANDLE pipes[2];
  native_io_endpoint endpoints[2] = {0};
  native_io_endpoint duplicate = {9u, 9u};
  native_io_request requests[2] = {0};
  native_io_completion events[2] = {0};
  unsigned char received[sizeof(payload)] = {0};
  native_io_operation operations[2];
  size_t count = 0u;

  check_equal(native_io_test_make_named_pipe_pair(pipes, false), SALTS_OK);
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipes[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE,
                                           &endpoints[0]),
              SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipes[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &duplicate),
              SALTS_EALREADY);
  check_false(native_io_endpoint_valid(duplicate));
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipes[1],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE,
                                           &endpoints[1]),
              SALTS_OK);
  operations[0] = (native_io_operation){NATIVE_IO_OPERATION_PIPE_READ, endpoints[0], received,
                                       sizeof(received), 71u, NULL, 0u, 0u};
  operations[1] = (native_io_operation){NATIVE_IO_OPERATION_PIPE_WRITE, endpoints[1], (void *)payload,
                                       sizeof(payload), 72u, NULL, 0u, 0u};
  check_equal(native_io_backend_submit(&backend, &operations[0], &requests[0]), SALTS_OK);
  check_equal(native_io_backend_submit(&backend, &operations[1], &requests[1]), SALTS_OK);
  check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
  for (count = 0u; count < 2u; ++count) {
    check_equal(events[count].kind, NATIVE_IO_COMPLETION_OK);
    check_equal(events[count].bytes, sizeof(payload));
  }
  check_equal(received, payload, sizeof(payload));

  check_equal(native_io_backend_close(&backend), SALTS_OK);
  native_io_test_close_pipe(pipes[0]);
  native_io_test_close_pipe(pipes[1]);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[0]), SALTS_OK);
  check_equal(native_io_backend_release_pipe(&backend, endpoints[1]), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}

static void native_io_test_iocp_pipe_cancel_and_eof(void) {
  native_io_backend backend = {0};
  const native_io_backend_config config = {NATIVE_IO_BACKEND_IOCP, 1u, 1u, 1u};
  HANDLE pipes[2];
  native_io_endpoint endpoint = {0};
  native_io_request request = {0};
  native_io_completion event = {0};
  unsigned char byte = 0u;
  native_io_operation operation;
  size_t count = 0u;

  check_equal(native_io_test_make_named_pipe_pair(pipes, false), SALTS_OK);
  check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
  check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipes[0],
                                           NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
              SALTS_OK);
  operation = (native_io_operation){NATIVE_IO_OPERATION_PIPE_READ, endpoint, &byte, sizeof(byte),
                                   73u, NULL, 0u, 0u};
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_cancel(&backend, request), SALTS_OK);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS,
                                       &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_CANCELLED);
  check_equal(event.status, SALTS_ECANCELED);

  request = (native_io_request){0};
  event = (native_io_completion){0};
  count = 0u;
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  native_io_test_close_pipe(pipes[1]);
  pipes[1] = INVALID_HANDLE_VALUE;
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS,
                                       &count),
              SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_EOF);
  check_equal(event.status, SALTS_EOF);

  /* A new read after peer closure must still be admitted and completed once. */
  request = (native_io_request){0};
  event = (native_io_completion){0};
  count = 0u;
  check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
  check_equal(native_io_backend_observe(&backend, &event, 1u, NATIVE_IO_TEST_TIMEOUT_MS,
                                       &count), SALTS_OK);
  check_equal(count, 1u);
  check_equal(event.kind, NATIVE_IO_COMPLETION_EOF);
  check_equal(event.status, SALTS_EOF);
  check_equal(event.bytes, (size_t)0u);
  check_equal(native_io_backend_observe(&backend, &event, 1u, 0u, &count), SALTS_ETIMEDOUT);
  check_equal(count, (size_t)0u);

  check_equal(native_io_backend_close(&backend), SALTS_OK);
  native_io_test_close_pipe(pipes[0]);
  check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
  check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}
#endif

spec("NativeIO direct backend") {
  it("describes every explicit backend model without fallback") {
    check_equal(native_io_backend_kind_model(NATIVE_IO_BACKEND_IOCP), NATIVE_IO_MODEL_COMPLETION);
    check_equal(native_io_backend_kind_model(NATIVE_IO_BACKEND_EPOLL), NATIVE_IO_MODEL_READINESS);
    check_equal(native_io_backend_kind_model(NATIVE_IO_BACKEND_IO_URING), NATIVE_IO_MODEL_COMPLETION);
    check_equal(native_io_backend_kind_model(NATIVE_IO_BACKEND_KQUEUE), NATIVE_IO_MODEL_READINESS);
#if defined(_WIN32)
    check_true(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IOCP));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_EPOLL));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IO_URING));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_KQUEUE));
    check_true(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IOCP));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_EPOLL));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IO_URING));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_KQUEUE));
#elif defined(__linux__)
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IOCP));
    check_true(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_EPOLL));
    check_true(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IO_URING));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_KQUEUE));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IOCP));
    check_true(native_io_backend_kind_supported(NATIVE_IO_BACKEND_EPOLL));
    check_true(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IO_URING));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_KQUEUE));
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
       defined(__DragonFly__)) && \
    UINTPTR_MAX > UINT32_MAX
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IOCP));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_EPOLL));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IO_URING));
    check_true(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_KQUEUE));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IOCP));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_EPOLL));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IO_URING));
    check_true(native_io_backend_kind_supported(NATIVE_IO_BACKEND_KQUEUE));
#else
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IOCP));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_EPOLL));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_IO_URING));
    check_false(native_io_backend_kind_supports_pipe(NATIVE_IO_BACKEND_KQUEUE));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IOCP));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_EPOLL));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_IO_URING));
    check_false(native_io_backend_kind_supported(NATIVE_IO_BACKEND_KQUEUE));
#endif
  }

  it("rejects malformed bounded configuration and clears output") {
    native_io_backend backend = {(void *)(uintptr_t)1u};
    const native_io_backend_config config = {NATIVE_IO_BACKEND_IOCP, 0u, 1u, 1u};
    check_equal(native_io_backend_init(&backend, &config), SALTS_EINVAL);
    check_null(backend.impl);
  }

  it("coalesces cross-thread wake requests without publishing fake completions") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    check_equal(native_io_backend_wake(NULL), SALTS_EINVAL);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_wake_coalesces(backends[index]);
  }

  it("resumes coroutine awaits from native terminal completions") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index) {
      native_io_test_coroutine_completion_resume(backends[index], false);
      native_io_test_coroutine_completion_resume(backends[index], true);
    }
  }

  it("resumes bounded vector writes through coroutine terminal completion routing") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_coroutine_vector_completion_resume(backends[index]);
  }

  it("routes a completion batch before looped coroutines reawait reused request slots") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_coroutine_reawait_batch(backends[index]);
  }

  it("retains cancelled coroutine frames until the terminal is drained") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_coroutine_cancel_drain(backends[index]);
  }

  it("reclaims a coroutine that suspends outside NativeIO await") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_coroutine_protocol_error_is_reclaimed(backends[index]);
  }

  it("validates addressed and connected UDP ownership shapes") {
    unsigned char payload = 0u;
    unsigned char address[32] = {0};
    native_io_operation operation = {.kind = NATIVE_IO_OPERATION_UDP_RECV_FROM,
                                    .endpoint = {1u, 1u},
                                    .buffer = &payload,
                                    .length = sizeof(payload)};

    check_true(native_io_operation_valid(&operation));
    operation.address = address;
    operation.address_capacity = sizeof(address);
    check_true(native_io_operation_valid(&operation));
    operation.kind = NATIVE_IO_OPERATION_UDP_SEND_TO;
    check_false(native_io_operation_valid(&operation));
    operation.address_length = sizeof(address);
    check_true(native_io_operation_valid(&operation));
    operation.address = NULL;
    operation.address_capacity = 0u;
    operation.address_length = 0u;
    check_true(native_io_operation_valid(&operation));
    operation.kind = NATIVE_IO_OPERATION_TCP_SEND;
    operation.address_capacity = sizeof(address);
    check_false(native_io_operation_valid(&operation));
    operation.address_capacity = 0u;
    check_true(native_io_operation_valid(&operation));
  }

  it("validates bounded vector-write descriptors without flattening") {
    unsigned char first = 1u;
    unsigned char second = 2u;
    native_io_buffer_span spans[2] = {{&first, 1u}, {&second, 1u}};
    native_io_vector_operation operation = {
        NATIVE_IO_OPERATION_STREAM_SEND, {1u, 1u}, spans, 2u, 91u};

    check_equal(NATIVE_IO_VECTOR_MAX, 16);
    check_true(native_io_vector_operation_valid(&operation));
    operation.kind = NATIVE_IO_OPERATION_PIPE_WRITE;
    check_true(native_io_vector_operation_valid(&operation));
    operation.kind = NATIVE_IO_OPERATION_STREAM_RECV;
    check_false(native_io_vector_operation_valid(&operation));
    operation.kind = NATIVE_IO_OPERATION_STREAM_SEND;
    operation.span_count = 0u;
    check_false(native_io_vector_operation_valid(&operation));
    operation.span_count = NATIVE_IO_VECTOR_MAX + 1u;
    check_false(native_io_vector_operation_valid(&operation));
    operation.span_count = 2u;
    spans[1].length = 0u;
    check_false(native_io_vector_operation_valid(&operation));
    spans[1].length = 1u;
    spans[1].data = NULL;
    check_false(native_io_vector_operation_valid(&operation));
    spans[1].data = &second;
    spans[0].length = (size_t)UINT32_MAX;
    spans[1].length = 1u;
    check_false(native_io_vector_operation_valid(&operation));
  }

  it("preserves TCP aliases for transport-neutral stream operations") {
    unsigned char payload = 0u;
    native_io_operation operation = {.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                    .endpoint = {1u, 1u},
                                    .buffer = &payload,
                                    .length = sizeof(payload)};

    check_equal(NATIVE_IO_OPERATION_STREAM_RECV, 1);
    check_equal(NATIVE_IO_OPERATION_STREAM_SEND, 2);
    check_equal(NATIVE_IO_OPERATION_STREAM_CONNECT, 7);
    check_equal(NATIVE_IO_OPERATION_TCP_RECV, NATIVE_IO_OPERATION_STREAM_RECV);
    check_equal(NATIVE_IO_OPERATION_TCP_SEND, NATIVE_IO_OPERATION_STREAM_SEND);
    check_equal(NATIVE_IO_OPERATION_TCP_CONNECT, NATIVE_IO_OPERATION_STREAM_CONNECT);
    check_equal(NATIVE_IO_OPERATION_UDP_RECV_FROM, 3);
    check_equal(NATIVE_IO_OPERATION_UDP_SEND_TO, 4);
    check_equal(NATIVE_IO_OPERATION_PIPE_READ, 5);
    check_equal(NATIVE_IO_OPERATION_PIPE_WRITE, 6);
    check_true(native_io_operation_valid(&operation));

    operation.address = &payload;
    check_false(native_io_operation_valid(&operation));
    operation.address = NULL;
    operation.address_capacity = sizeof(payload);
    check_false(native_io_operation_valid(&operation));
    operation.address_capacity = 0u;
    operation.address_length = sizeof(payload);
    check_false(native_io_operation_valid(&operation));

    operation.address_length = 0u;
    operation.kind = NATIVE_IO_OPERATION_PIPE_WRITE;
    check_true(native_io_operation_valid(&operation));
  }

  it("validates TCP connect address ownership without a payload borrow") {
    unsigned char address[32] = {0};
    native_io_operation operation = {.kind = NATIVE_IO_OPERATION_TCP_CONNECT,
                                    .endpoint = {1u, 1u},
                                    .address = address,
                                    .address_capacity = sizeof(address),
                                    .address_length = sizeof(address)};

    check_true(native_io_operation_valid(&operation));
    operation.address_length = 0u;
    check_false(native_io_operation_valid(&operation));
    operation.address_length = sizeof(address) + 1u;
    check_false(native_io_operation_valid(&operation));
    operation.address_length = sizeof(address);
    operation.buffer = address;
    check_false(native_io_operation_valid(&operation));
    operation.buffer = NULL;
    operation.length = 1u;
    check_false(native_io_operation_valid(&operation));
  }

  it("clears a pipe endpoint when attach arguments are invalid") {
    native_io_endpoint endpoint = {1u, 1u};

    check_equal(native_io_backend_attach_pipe(NULL, 0u,
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE,
                                             &endpoint),
                SALTS_EINVAL);
    check_false(native_io_endpoint_valid(endpoint));
  }

  it("rejects a byte-pipe operation on a socket endpoint before native admission") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_reject_pipe_operation_on_socket(backends[index]);
  }

  it("rejects TCP and UDP operations submitted to the other socket transport") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_reject_cross_protocol_socket_operation(backends[index]);
  }

#if defined(_WIN32)
  it("rejects synchronous anonymous pipes instead of selecting a worker fallback") {
    native_io_backend backend = {0};
    const native_io_backend_config config = {NATIVE_IO_BACKEND_IOCP, 1u, 1u, 1u};
    HANDLE descriptors[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    native_io_endpoint endpoint = {0};

    check_true(CreatePipe(&descriptors[0], &descriptors[1], NULL, 0u));
    check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
    check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0],
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
                SALTS_ENOTSUP);
    check_false(native_io_endpoint_valid(endpoint));
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
    native_io_test_close_pipe(descriptors[0]);
    native_io_test_close_pipe(descriptors[1]);
  }

  it("rejects message-mode named pipes before IOCP association") {
    static LONG sequence = 0;
    char name[128];
    native_io_backend backend = {0};
    const native_io_backend_config config = {NATIVE_IO_BACKEND_IOCP, 1u, 1u, 1u};
    native_io_endpoint endpoint = {0};
    HANDLE pipe_handle;
    int name_length = snprintf(name, sizeof(name),
                               "\\\\.\\pipe\\native-io-message-test-%lu-%ld",
                               GetCurrentProcessId(), InterlockedIncrement(&sequence));

    check_true(name_length >= 0 && (size_t)name_length < sizeof(name));
    pipe_handle = CreateNamedPipeA(
        name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1u,
        NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY, NATIVE_IO_TEST_PIPE_BUFFER_CAPACITY,
        0u, NULL);
    check_true(pipe_handle != INVALID_HANDLE_VALUE);
    check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
    check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipe_handle,
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
                SALTS_EINVAL);
    check_false(native_io_endpoint_valid(endpoint));
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
    native_io_test_close_pipe(pipe_handle);
  }

  it("accepts an overlapped outbound server with least-privilege access") {
    static const unsigned char payload[] = {0x6cu, 0x65u, 0x61u, 0x73u, 0x74u};
    native_io_backend backend = {0};
    const native_io_backend_config config = {NATIVE_IO_BACKEND_IOCP, 2u, 2u, 2u};
    HANDLE pipes[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    native_io_endpoint endpoints[2] = {0};
    native_io_request requests[2] = {0};
    native_io_completion events[2] = {0};
    unsigned char received[sizeof(payload)] = {0};
    native_io_operation operations[2];

    check_equal(native_io_test_make_named_pipe_pair(pipes, true), SALTS_OK);
    check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
    check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipes[0],
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[0]),
                SALTS_OK);
    check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)pipes[1],
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[1]),
                SALTS_OK);
    {
      native_io_buffer_span span = {(void *)payload, sizeof(payload)};
      native_io_vector_operation vector_write = {
          NATIVE_IO_OPERATION_PIPE_WRITE, endpoints[0], &span, 1u, 80u};
      native_io_request rejected = {1u, 1u};
      check_false(native_io_backend_endpoint_supports_vector_write(&backend, endpoints[0]));
      check_equal(native_io_backend_submit_vector(&backend, &vector_write, &rejected),
                  SALTS_ENOTSUP);
      check_false(native_io_request_valid(rejected));
    }
    operations[0] = (native_io_operation){NATIVE_IO_OPERATION_PIPE_WRITE, endpoints[0], (void *)payload,
                                         sizeof(payload), 81u, NULL, 0u, 0u};
    operations[1] = (native_io_operation){NATIVE_IO_OPERATION_PIPE_READ, endpoints[1], received,
                                         sizeof(received), 82u, NULL, 0u, 0u};
    check_equal(native_io_backend_submit(&backend, &operations[0], &requests[0]), SALTS_OK);
    check_equal(native_io_backend_submit(&backend, &operations[1], &requests[1]), SALTS_OK);
    check_equal(native_io_test_observe_all(&backend, events, 2u), SALTS_OK);
    check_equal(received, payload, sizeof(payload));
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    native_io_test_close_pipe(pipes[0]);
    native_io_test_close_pipe(pipes[1]);
    check_equal(native_io_backend_release_pipe(&backend, endpoints[0]), SALTS_OK);
    check_equal(native_io_backend_release_pipe(&backend, endpoints[1]), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  }

  it("round trips byte-pipe payloads through IOCP completion") {
    native_io_test_iocp_pipe_round_trip();
  }

  it("publishes IOCP pipe cancellation and peer EOF as terminal completions") {
    native_io_test_iocp_pipe_cancel_and_eof();
  }
#endif

#if !defined(_WIN32)
  it("rejects blocking byte-pipe descriptors without changing their flags") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index) {
      native_io_backend backend = {0};
      const native_io_backend_config config = {backends[index], 1u, 1u, 1u};
      int descriptors[2] = {-1, -1};
      native_io_endpoint endpoint = {1u, 1u};

      check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
      check_equal(native_io_test_make_pipe(descriptors, false), SALTS_OK);
      check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)descriptors[0],
                                               NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
                  SALTS_EINVAL);
      check_false(native_io_endpoint_valid(endpoint));
      check_equal(fcntl(descriptors[0], F_GETFL, 0) & O_NONBLOCK, 0);
      (void)close(descriptors[0]);
      (void)close(descriptors[1]);
      check_equal(native_io_backend_close(&backend), SALTS_OK);
      check_equal(native_io_backend_destroy(&backend), SALTS_OK);
    }
  }

  it("round trips bytes through readiness pipe endpoints") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_pipe_round_trip(backends[index], true);
  }

  it("round trips bounded vector writes through readiness pipe endpoints") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_pipe_vector_round_trip(backends[index], true);
  }

  it("preserves FIFO order across bounded vector pipe writes") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_vector_pipe_fifo(backends[index], true);
  }

  it("keeps readiness vector payloads borrowed through cancelled terminal observation") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_readiness_vector_cancel(backends[index]);
  }

  it("publishes a partial vector write as one concatenated-prefix byte count") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_readiness_vector_partial_prefix(backends[index]);
  }

  it("publishes pipe EOF and rejects stale endpoints after descriptor reuse") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_pipe_eof_and_reuse(backends[index]);
  }

  it("preserves pipe read FIFO order while cancelling a queued entry") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_pipe_fifo_and_cancel(backends[index]);
  }

  it("enforces pipe capacity and contains broken-peer SIGPIPE") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_readiness_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_readiness_pipe_capacity_and_broken_peer(backends[index]);
  }

#if defined(__linux__)
  it("round trips a blocking byte pipe through io_uring completion") {
    native_io_test_pipe_round_trip(NATIVE_IO_BACKEND_IO_URING, false);
  }

  it("round trips a bounded vector write through an io_uring pipe") {
    native_io_test_pipe_vector_round_trip(NATIVE_IO_BACKEND_IO_URING, false);
  }

  it("preserves FIFO order across io_uring vector pipe writes") {
    native_io_test_vector_pipe_fifo(NATIVE_IO_BACKEND_IO_URING, false);
  }

  it("cancels a queued io_uring vector write without releasing its request early") {
    native_io_test_uring_vector_cancel_queued();
  }

  it("publishes io_uring pipe EOF and rejects a reused stale endpoint") {
    native_io_test_pipe_eof_and_reuse(NATIVE_IO_BACKEND_IO_URING);
  }

  it("preserves io_uring pipe FIFO while cancelling a queued read") {
    native_io_test_pipe_fifo_and_cancel(NATIVE_IO_BACKEND_IO_URING);
  }

  it("publishes io_uring broken-pipe failure without leaking SIGPIPE") {
    native_io_test_io_uring_pipe_broken_peer();
  }
#endif
#endif

  it("round trips TCP through every platform backend") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_round_trip_tcp(backends[index]);
  }

  it("round trips bounded vector writes through every stream backend") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_round_trip_tcp_vector(backends[index]);
  }

  it("connects TCP through every platform backend") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_tcp_connect(backends[index]);
  }

  it("publishes one failed completion for a refused TCP connect") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_tcp_connect_refused(backends[index]);
  }

  it("preserves FIFO receive lanes on one endpoint") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_fifo_tcp_receives(backends[index]);
  }

  it("keeps cancellation borrowed until terminal observation") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_cancel_pending(backends[index]);
  }

  it("cancels both active and queued operations in one lane") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_cancel_same_lane(backends[index]);
  }

  it("round trips UDP and publishes its peer address") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_round_trip_udp(backends[index]);
  }

  it("rejects truncated UDP receives for addressed and connected sockets") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index) {
      native_io_test_udp_truncation(backends[index], false);
      native_io_test_udp_truncation(backends[index], true);
    }
  }

#if !defined(_WIN32)
  it("preserves the observe deadline across repeated signal interruptions") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_interrupted_timeout(backends[index]);
  }
#endif

  it("enforces capacity stale-handle and close boundaries") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_capacity_and_close(backends[index]);
  }

  it("batches prepared requests with FIFO cancellation close and bounded slot reuse") {
    native_io_backend_kind backends[NATIVE_IO_TEST_MAX_BACKENDS];
    const size_t count = native_io_test_backends(backends);
    for (size_t index = 0u; index < count; ++index)
      native_io_test_prepared_fifo(backends[index]);
  }
#if defined(__linux__)
  it("keeps epoll FIFO and progress after an idle edge has been consumed") {
    native_io_test_epoll_pending_fifo();
  }
  it("cancels an io_uring staged head without sending its bytes") {
    native_io_test_uring_cancel_prepared_head();
  }
#endif
}
