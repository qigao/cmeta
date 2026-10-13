/* #1168: real Linux io_uring CQE is visible before its original
 * request terminal is delivered. No fake CQE and no second NativeIO Owner.
 * This executable is a separate source-only test build, not public ABI.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <salts/native_io.h>
#include <salts/error_codes.h>
#include "native_io_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { URING_GATE_ROUNDS = 12, URING_GATE_TIMEOUT_MS = 5000 };
enum {
  URING_COMPLETE_FIRST = 0,
  URING_CQE_THEN_CANCEL = 1,
  URING_CANCEL_FIRST = 2
};

typedef struct uring_gate_state {
  native_io_backend *backend;
  native_io_endpoint endpoint;
  native_io_request request;
  unsigned armed;
  unsigned entered;
  unsigned wrong_identity;
  int observed_result;
  int cancel_status;
  int retained_status;
} uring_gate_state;

static uring_gate_state gate;

/* Called by the private source-copy NativeIO driver on the original Owner
 * stack AFTER a real kernel CQE (token+res) has been acquired, but BEFORE
 * it is published as the unique terminal. Cancel may be accepted even
 * though the kernel completion has already won: the original OK survives.
 */
void salts_native_io_test_before_uring_dispatch(
    cmeta_io_impl *owner, native_io_request request, int kernel_result) {
  if (!gate.armed || !gate.backend || gate.backend->impl != owner) return;
  if (request.slot != gate.request.slot ||
      request.generation != gate.request.generation) {
    ++gate.wrong_identity;
    return;
  }
  ++gate.entered;
  if (gate.entered != 1u) return;
  gate.observed_result = kernel_result;
  gate.cancel_status = native_io_backend_cancel(gate.backend, request);
  /* The original payload and endpoint remain borrowed until observe
   * returns that exact request's original terminal completion. */
  gate.retained_status =
      native_io_backend_release_socket(gate.backend, gate.endpoint);
}

static int fail(unsigned line, const char *expression, int status) {
  fprintf(stderr, "real io_uring CQE cancel gate failed line %u: %s (%d)\n",
          line, expression, status);
  return 1;
}
#define REQUIRE(expr) do { if (!(expr)) return fail(__LINE__, #expr, 0); } while (0)
#define REQUIRE_STATUS(expr, value) do { \
  const int status_ = (expr); \
  if (status_ != (value)) return fail(__LINE__, #expr, status_); \
} while (0)

static int make_pair(int fds[2]) {
  int flags;
  fds[0] = -1;
  fds[1] = -1;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -errno;
  flags = fcntl(fds[0], F_GETFL, 0);
  if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    const int saved = errno;
    (void)close(fds[0]);
    (void)close(fds[1]);
    fds[0] = fds[1] = -1;
    return -saved;
  }
  return SALTS_OK;
}

int main(void) {
  native_io_backend backend = {0};
  native_io_backend_config config = {
      NATIVE_IO_BACKEND_IO_URING, 1u, 1u, 1u};
  native_io_endpoint prior_endpoint = {0};
  native_io_request prior_request = {0};
  unsigned round;
  int status = native_io_backend_init(&backend, &config);
  if (status == -EPERM || status == -EACCES ||
      status == -ENOSYS || status == SALTS_ENOTSUP) {
    /* This runner cannot create a real ring. NO fallback to epoll and
     * NO green CQE qualification is claimed by this explicit skip. */
    fprintf(stderr, "SKIP real io_uring CQE gate: kernel setup rejected %d\n", status);
    return 77;
  }
  REQUIRE_STATUS(status, SALTS_OK);

  for (round = 0u; round < URING_GATE_ROUNDS; ++round) {
    const unsigned mode = round % 3u;
    const unsigned char sent = (unsigned char)(0x41u + round);
    unsigned char received = 0xe5u;
    int fds[2] = {-1, -1};
    native_io_endpoint endpoint = {0};
    native_io_request request = {0};
    native_io_operation operation = {0};
    native_io_completion event = {0};
    size_t count = 0u;
    unsigned extra;

    REQUIRE_STATUS(make_pair(fds), SALTS_OK);
    REQUIRE_STATUS(native_io_backend_attach_socket(
                       &backend, (uintptr_t)fds[0], &endpoint), SALTS_OK);
    if (round != 0u) {
      REQUIRE(endpoint.slot == prior_endpoint.slot);
      REQUIRE(endpoint.generation != prior_endpoint.generation);
    }
    operation.kind = NATIVE_IO_OPERATION_STREAM_RECV;
    operation.endpoint = endpoint;
    operation.buffer = &received;
    operation.length = 1u;
    operation.user_data = (uintptr_t)(4000u + round);
    REQUIRE_STATUS(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
    if (round != 0u) {
      REQUIRE(request.slot == prior_request.slot);
      REQUIRE(request.generation != prior_request.generation);
    }
    memset(&gate, 0, sizeof(gate));
    gate.backend = &backend;
    gate.endpoint = endpoint;
    gate.request = request;
    gate.armed = mode == URING_CQE_THEN_CANCEL;

    if (mode == URING_CANCEL_FIRST) {
      /* No peer data exists when this original in-flight request is
       * cancelled; its one terminal must be the real -ECANCELED CQE. */
      REQUIRE_STATUS(native_io_backend_cancel(&backend, request), SALTS_OK);
    } else {
      REQUIRE(send(fds[1], &sent, 1u, 0) == 1);
    }

    REQUIRE_STATUS(native_io_backend_observe(
                       &backend, &event, 1u, URING_GATE_TIMEOUT_MS, &count), SALTS_OK);
    REQUIRE(count == 1u);
    REQUIRE(event.request.slot == request.slot);
    REQUIRE(event.request.generation == request.generation);
    REQUIRE(event.endpoint.slot == endpoint.slot);
    REQUIRE(event.endpoint.generation == endpoint.generation);
    REQUIRE(event.user_data == operation.user_data);

    if (mode == URING_CANCEL_FIRST) {
      REQUIRE(event.kind == NATIVE_IO_COMPLETION_CANCELLED);
      REQUIRE(event.status == SALTS_ECANCELED);
      REQUIRE(received == 0xe5u);
      REQUIRE(gate.entered == 0u);
    } else {
      REQUIRE(event.kind == NATIVE_IO_COMPLETION_OK);
      REQUIRE(event.status == SALTS_OK);
      REQUIRE(event.bytes == 1u && received == sent);
      if (mode == URING_CQE_THEN_CANCEL) {
        REQUIRE(gate.entered == 1u && gate.wrong_identity == 0u);
        REQUIRE(gate.observed_result == 1);
        REQUIRE(gate.cancel_status == SALTS_OK);
        REQUIRE(gate.retained_status == SALTS_EBUSY);
      } else {
        REQUIRE(gate.entered == 0u);
      }
    }

    gate.armed = 0u;
    REQUIRE_STATUS(native_io_backend_cancel(&backend, request), SALTS_ENOENT);
    /* Drain any late ASYNC_CANCEL acknowledgement (reserved CQE token
     * zero), never a second completion for this original request. */
    for (extra = 0u; extra < 8u; ++extra) {
      count = 0u;
      status = native_io_backend_observe(&backend, &event, 1u, 0u, &count);
      REQUIRE(count == 0u);
      REQUIRE(status == SALTS_OK || status == SALTS_ETIMEDOUT);
      if (status == SALTS_ETIMEDOUT) break;
    }
    REQUIRE(close(fds[0]) == 0);
    REQUIRE_STATUS(native_io_backend_release_socket(&backend, endpoint), SALTS_OK);
    REQUIRE(close(fds[1]) == 0);
    prior_endpoint = endpoint;
    prior_request = request;
  }
  REQUIRE_STATUS(native_io_backend_close(&backend), SALTS_OK);
  REQUIRE_STATUS(native_io_backend_destroy(&backend), SALTS_OK);
  puts("real io_uring CQE/Owner cancel: 12 kernel request-generation crosses PASS");
  return 0;
}
