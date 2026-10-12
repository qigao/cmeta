#define _POSIX_C_SOURCE 200809L

#include <salts/error_codes.h>
#include <salts/native_io.h>
#include <tinytest.h>

/* Never installed: private callback is only compiled into this test-owned
 * duplicate NativeIO backend. The production Salts::NativeIO is unchanged. */
#include "native_io_readiness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#define TEST_READINESS_BACKEND NATIVE_IO_BACKEND_EPOLL
#else
#define TEST_READINESS_BACKEND NATIVE_IO_BACKEND_KQUEUE
#endif

enum { TEST_KERNEL_WAIT_MS = 3000 };

typedef struct readiness_race_fixture {
  native_io_backend *backend;
  native_io_endpoint endpoint;
  native_io_request request;
  unsigned actual_kernel_events;
  unsigned unexpected_kernel_events;
  int cancel_result;
} readiness_race_fixture;

static void readiness_cancel_after_kernel_event(
    void *user, native_io_backend_kind kind, native_io_endpoint endpoint,
    uint32_t ready_interests) {
  readiness_race_fixture *fixture = (readiness_race_fixture *)user;
  ++fixture->actual_kernel_events;
  if (kind != TEST_READINESS_BACKEND ||
      endpoint.slot != fixture->endpoint.slot ||
      endpoint.generation != fixture->endpoint.generation ||
      (ready_interests & SALTS_IO_READY_READ) == 0u) {
    ++fixture->unexpected_kernel_events;
    return;
  }

  /* Executed INSIDE the same Owner's actual observe() after a real
   * epoll_wait/kevent readiness event, before NativeIO calls recv().
   * Never race two methods on the backend from different threads. */
  fixture->cancel_result =
      native_io_backend_cancel(fixture->backend, fixture->request);
}

static void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  check_greater_equal(flags, 0);
  check_equal(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
}

spec("NativeIO real readiness completion/cancel owner crossover") {
  static native_io_backend backend;
  static int old_pair[2], next_pair[2];
  static readiness_race_fixture fixture;

  before_each() {
    backend = (native_io_backend){0};
    old_pair[0] = old_pair[1] = -1;
    next_pair[0] = next_pair[1] = -1;
    fixture = (readiness_race_fixture){0};
    fixture.backend = &backend;
    fixture.cancel_result = SALTS_EINVAL;
    cmeta_io_readiness_test_set_dispatch_hook(NULL, NULL);
  }

  after_each() {
    cmeta_io_readiness_test_set_dispatch_hook(NULL, NULL);
    /* Tests own the FDs; NativeIO never closes attached sockets. The
     * normal paths below retire every request and release metadata. */
    if (backend.impl != NULL) {
      (void)native_io_backend_close(&backend);
      (void)native_io_backend_destroy(&backend);
    }
    if (old_pair[0] >= 0) (void)close(old_pair[0]);
    if (old_pair[1] >= 0) (void)close(old_pair[1]);
    if (next_pair[0] >= 0) (void)close(next_pair[0]);
    if (next_pair[1] >= 0) (void)close(next_pair[1]);
  }

  it("cancels after real epoll or kevent dequeue, before request dispatch") {
    const native_io_backend_config cfg = {
        TEST_READINESS_BACKEND, 1u, 1u, 1u};
    native_io_endpoint replacement = {0};
    native_io_request second_request = {0};
    native_io_completion completion = {0};
    native_io_backend_stats stats = {0};
    native_io_operation op = {0};
    unsigned char first_read = 0u, second_read = 0u;
    const unsigned char old_payload = 0x31u, late_payload = 0x33u;
    const unsigned char new_payload = 0x52u;
    size_t count = 0u;

    check_equal(native_io_backend_init(&backend, &cfg), SALTS_OK);
    check_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, old_pair), 0);
    set_nonblocking(old_pair[0]);
    check_equal(native_io_backend_attach_socket(
                    &backend, (uintptr_t)old_pair[0], &fixture.endpoint), SALTS_OK);

    op.kind = NATIVE_IO_OPERATION_STREAM_RECV;
    op.endpoint = fixture.endpoint;
    op.buffer = &first_read;
    op.length = sizeof(first_read);
    op.user_data = (uintptr_t)0xabc101u;
    check_equal(native_io_backend_submit(&backend, &op, &fixture.request), SALTS_OK);
    check_equal(native_io_backend_release_socket(
                    &backend, fixture.endpoint), SALTS_EBUSY);

    /* The peer makes the actual kernel socket readable; the first OS
     * readiness event is intercepted only AFTER the native wait returns. */
    cmeta_io_readiness_test_set_dispatch_hook(
        readiness_cancel_after_kernel_event, &fixture);
    check_equal(send(old_pair[1], &old_payload, sizeof(old_payload), 0),
                (ssize_t)sizeof(old_payload));
    check_equal(native_io_backend_observe(
                    &backend, &completion, 1u, TEST_KERNEL_WAIT_MS, &count),
                SALTS_OK);
    cmeta_io_readiness_test_set_dispatch_hook(NULL, NULL);
    check_equal(fixture.actual_kernel_events, 1u);
    check_equal(fixture.unexpected_kernel_events, 0u);
    check_equal(fixture.cancel_result, SALTS_OK);
    check_equal(count, 1u);
    check_equal(completion.kind, NATIVE_IO_COMPLETION_CANCELLED);
    check_equal(completion.request.slot, fixture.request.slot);
    check_equal(completion.request.generation, fixture.request.generation);
    check_equal(completion.endpoint.generation, fixture.endpoint.generation);
    check_equal(completion.user_data, (uintptr_t)0xabc101u);
    check_equal(first_read, 0u);

    /* Late old bytes cannot publish a second terminal or enter a future
     * connection generation; retaining the old FD here is intentional. */
    check_equal(send(old_pair[1], &late_payload, sizeof(late_payload), 0),
                (ssize_t)sizeof(late_payload));
    count = 0u;
    check_equal(native_io_backend_observe(
                    &backend, &completion, 1u, 0u, &count), SALTS_ETIMEDOUT);
    check_equal(count, 0u);
    check_equal(native_io_backend_cancel(&backend, fixture.request), SALTS_ENOENT);

    check_equal(close(old_pair[0]), 0);
    old_pair[0] = -1;
    check_equal(native_io_backend_release_socket(
                    &backend, fixture.endpoint), SALTS_OK);

    /* One bounded endpoint/request slot, but a NEW generation and socket. */
    check_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, next_pair), 0);
    set_nonblocking(next_pair[0]);
    check_equal(native_io_backend_attach_socket(
                    &backend, (uintptr_t)next_pair[0], &replacement), SALTS_OK);
    check_equal(replacement.slot, fixture.endpoint.slot);
    check_not_equal(replacement.generation, fixture.endpoint.generation);
    op.endpoint = replacement;
    op.buffer = &second_read;
    op.user_data = (uintptr_t)0xabc202u;
    check_equal(native_io_backend_submit(&backend, &op, &second_request), SALTS_OK);
    check_equal(send(next_pair[1], &new_payload, sizeof(new_payload), 0),
                (ssize_t)sizeof(new_payload));
    count = 0u;
    check_equal(native_io_backend_observe(
                    &backend, &completion, 1u, TEST_KERNEL_WAIT_MS, &count),
                SALTS_OK);
    check_equal(count, 1u);
    check_equal(completion.kind, NATIVE_IO_COMPLETION_OK);
    check_equal(completion.user_data, (uintptr_t)0xabc202u);
    check_equal(completion.request.generation, second_request.generation);
    check_equal(completion.endpoint.generation, replacement.generation);
    check_equal(second_read, new_payload);

    check_equal(close(next_pair[0]), 0);
    next_pair[0] = -1;
    check_equal(native_io_backend_release_socket(
                    &backend, replacement), SALTS_OK);
    check(native_io_backend_get_stats(&backend, &stats));
    check_equal(stats.active_requests, 0u);
    check_equal(stats.cancelled, 1u);
    check_equal(stats.completed, 1u);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  }

  it("cancels before the peer sends; no late terminal after close") {
    const native_io_backend_config cfg = {
        TEST_READINESS_BACKEND, 1u, 1u, 1u};
    native_io_operation op = {0};
    native_io_completion terminal = {0};
    unsigned char received = 0u;
    const unsigned char late = 0x5au;
    size_t count = 0u;
    check_equal(native_io_backend_init(&backend, &cfg), SALTS_OK);
    check_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, old_pair), 0);
    set_nonblocking(old_pair[0]);
    check_equal(native_io_backend_attach_socket(
                    &backend, (uintptr_t)old_pair[0], &fixture.endpoint), SALTS_OK);
    op.kind = NATIVE_IO_OPERATION_STREAM_RECV;
    op.endpoint = fixture.endpoint;
    op.buffer = &received;
    op.length = sizeof(received);
    op.user_data = (uintptr_t)0xabc303u;
    check_equal(native_io_backend_submit(&backend, &op, &fixture.request), SALTS_OK);
    check_equal(native_io_backend_cancel(&backend, fixture.request), SALTS_OK);
    check_equal(native_io_backend_observe(
                    &backend, &terminal, 1u, 0u, &count), SALTS_OK);
    check_equal(count, 1u);
    check_equal(terminal.kind, NATIVE_IO_COMPLETION_CANCELLED);
    check_equal(terminal.user_data, (uintptr_t)0xabc303u);
    check_equal(terminal.request.generation, fixture.request.generation);
    check_equal(send(old_pair[1], &late, sizeof(late), 0), (ssize_t)sizeof(late));
    count = 0u;
    check_equal(native_io_backend_observe(
                    &backend, &terminal, 1u, 0u, &count), SALTS_ETIMEDOUT);
    check_equal(count, 0u);
    check_equal(received, 0u);
    check_equal(close(old_pair[0]), 0);
    old_pair[0] = -1;
    check_equal(native_io_backend_release_socket(
                    &backend, fixture.endpoint), SALTS_OK);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  }
}
