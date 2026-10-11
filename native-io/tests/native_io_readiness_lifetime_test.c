#define _POSIX_C_SOURCE 200809L

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/native_io.h>
#include <salts/thread.h>
#include <tinytest.h>

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

enum { REUSE_WAIT_MS = 5000 };

typedef struct fd_reuse_owner {
  int retired_fd;
  int status;
  atomic_bool done;
  atomic_bool released;
} fd_reuse_owner;

static void reuse_retired_fd(void *user) {
  fd_reuse_owner *owner = (fd_reuse_owner *)user;
  const int replacement = epoll_create1(EPOLL_CLOEXEC);
  owner->status = replacement < 0 ? -errno : SALTS_OK;
  if (replacement >= 0) {
    if (replacement != owner->retired_fd) {
      if (dup2(replacement, owner->retired_fd) < 0) owner->status = -errno;
      (void)close(replacement);
    }
  }
  /* This schedules descriptor reuse without publishing the replacement to the
   * backend owner. release_socket must not access that other owner's fd. Only
   * join publishes status, after metadata release has completed. */
  atomic_store_explicit(&owner->done, true, memory_order_relaxed);
  const uint64_t started = cmeta_monotonic_ms();
  while (!atomic_load_explicit(&owner->released, memory_order_acquire) &&
         cmeta_monotonic_ms() - started < REUSE_WAIT_MS)
    cmeta_thread_yield();
  if (owner->status == SALTS_OK && close(owner->retired_fd) != 0)
    owner->status = -errno;
}

spec("readiness endpoint lifetime") {
  static native_io_backend backend;
  static cmeta_thread_t thread;
  static int sockets[2];
  static fd_reuse_owner reuse;

  before_each() {
    backend = (native_io_backend){0};
    thread = NULL;
    sockets[0] = sockets[1] = -1;
    reuse.retired_fd = -1;
    reuse.status = SALTS_EIO;
    atomic_init(&reuse.done, false);
    atomic_init(&reuse.released, false);
  }

  after_each() {
    atomic_store_explicit(&reuse.released, true, memory_order_release);
    if (thread != NULL) (void)cmeta_thread_join(&thread);
    for (size_t i = 0u; i < 2u; ++i)
      if (sockets[i] >= 0) (void)close(sockets[i]);
    if (backend.impl != NULL) {
      (void)native_io_backend_close(&backend);
      (void)native_io_backend_destroy(&backend);
    }
  }

  it("releases only metadata after another owner reuses a closed socket fd") {
    const native_io_backend_config config = {NATIVE_IO_BACKEND_EPOLL, 1u, 1u, 1u};
    native_io_endpoint endpoint = {0};
    native_io_request request = {0};
    native_io_completion completion = {0};
    unsigned char byte = 0u;
    size_t count = 0u;
    check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
    check_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[1], &endpoint), SALTS_OK);
    const native_io_operation operation = {
        .kind = NATIVE_IO_OPERATION_STREAM_RECV, .endpoint = endpoint,
        .buffer = &byte, .length = sizeof(byte)};
    check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
    check_equal(native_io_backend_release_socket(&backend, endpoint), SALTS_EBUSY);
    check_equal(native_io_backend_cancel(&backend, request), SALTS_OK);
    check_equal(native_io_backend_observe(&backend, &completion, 1u, 0u, &count), SALTS_OK);
    check_equal(count, 1u);
    check_equal(completion.kind, NATIVE_IO_COMPLETION_CANCELLED);
    reuse.retired_fd = sockets[1];
    check_equal(close(sockets[1]), 0);
    sockets[1] = -1;
    check_equal(cmeta_thread_create(&thread, reuse_retired_fd, &reuse), SALTS_OK);
    const uint64_t started = cmeta_monotonic_ms();
    while (!atomic_load_explicit(&reuse.done, memory_order_relaxed) &&
           cmeta_monotonic_ms() - started < REUSE_WAIT_MS)
      cmeta_thread_yield();
    check(atomic_load_explicit(&reuse.done, memory_order_relaxed));
    check_equal(native_io_backend_release_socket(&backend, endpoint), SALTS_OK);
    atomic_store_explicit(&reuse.released, true, memory_order_release);
    check_equal(cmeta_thread_join(&thread), SALTS_OK);
    check_equal(reuse.status, SALTS_OK);
    check_equal(native_io_backend_release_socket(&backend, endpoint), SALTS_ENOENT);
    native_io_endpoint replacement = {0};
    check_equal(native_io_backend_attach_socket(&backend, (uintptr_t)sockets[0], &replacement), SALTS_OK);
    check_not_equal(replacement.generation, endpoint.generation);
    check_equal(close(sockets[0]), 0);
    sockets[0] = -1;
    check_equal(native_io_backend_release_socket(&backend, replacement), SALTS_OK);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  }

  it("detaches a drained live pipe before reattaching its descriptor") {
    const native_io_backend_config config = {NATIVE_IO_BACKEND_EPOLL, 1u, 1u, 1u};
    native_io_endpoint endpoint = {0};
    native_io_endpoint replacement = {0};
    native_io_request request = {0};
    native_io_completion completion = {0};
    unsigned char byte = 0u;
    const unsigned char payload = 0x41u;
    size_t count = 0u;
    check_equal(native_io_backend_init(&backend, &config), SALTS_OK);
    check_equal(pipe(sockets), 0);
    for (size_t i = 0u; i < 2u; ++i) {
      const int flags = fcntl(sockets[i], F_GETFL, 0);
      check_greater_equal(flags, 0);
      check_equal(fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK), 0);
    }
    check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)sockets[0],
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint), SALTS_OK);
    native_io_operation operation = {
        .kind = NATIVE_IO_OPERATION_PIPE_READ, .endpoint = endpoint,
        .buffer = &byte, .length = sizeof(byte)};
    check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
    check_equal(write(sockets[1], &payload, sizeof(payload)), (ssize_t)sizeof(payload));
    check_equal(native_io_backend_observe(&backend, &completion, 1u, REUSE_WAIT_MS, &count), SALTS_OK);
    check_equal(count, 1u);
    check_equal(completion.kind, NATIVE_IO_COMPLETION_OK);
    check_equal(byte, payload);
    check_equal(native_io_backend_release_pipe(&backend, endpoint), SALTS_OK);
    check_equal(native_io_backend_attach_pipe(&backend, (uintptr_t)sockets[0],
                                             NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &replacement), SALTS_OK);
    check_not_equal(replacement.generation, endpoint.generation);
    operation.endpoint = replacement;
    check_equal(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
    check_equal(native_io_backend_cancel(&backend, request), SALTS_OK);
    check_equal(native_io_backend_observe(&backend, &completion, 1u, 0u, &count), SALTS_OK);
    check_equal(count, 1u);
    check_equal(completion.kind, NATIVE_IO_COMPLETION_CANCELLED);
    check_equal(native_io_backend_release_pipe(&backend, replacement), SALTS_OK);
    check_equal(native_io_backend_close(&backend), SALTS_OK);
    check_equal(native_io_backend_destroy(&backend), SALTS_OK);
  }
}
