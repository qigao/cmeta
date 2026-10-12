/* #1168: actual epoll_wait/kevent return -> same Owner cancel, before
 * NativeIO resolves and publishes the terminal for the original request.
 *
 * This is linked to a PRIVATE test-only compilation of NativeIO. There is
 * no public hook, synthetic kernel event, extra backend, or second Owner.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <salts/native_io.h>
#include <salts/error_codes.h>
#include "native_io_readiness.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { GATE_ROUNDS = 12, GATE_TIMEOUT_MS = 5000 };
enum { GATE_COMPLETE_THEN_CANCEL = 0, GATE_CANCEL_AT_READY = 1,
       GATE_CANCEL_BEFORE_PEER_SEND = 2 };

typedef struct kernel_ready_gate {
  native_io_backend *backend;
  native_io_endpoint endpoint;
  native_io_request request;
  unsigned armed;
  unsigned seen;
  unsigned wrong_token;
  int cancel_status;
  int premature_release_status;
} kernel_ready_gate;

static kernel_ready_gate gate;

/* The source backend calls this only after a *real* epoll_wait/kevent
 * returned, and on the original observe Owner stack before dispatch.
 * Calling cancel here is owner-serialized (not a competing worker API).
 */
void salts_native_io_test_before_ready_dispatch(
    cmeta_io_impl *base, const cmeta_io_ready_event *events, size_t count) {
  const uint64_t expected = ((uint64_t)gate.endpoint.generation << 32u) |
                            (uint64_t)gate.endpoint.slot;
  size_t i;
  if (!gate.armed || !gate.backend || gate.backend->impl != base) return;
  for (i = 0u; i < count; ++i) {
    if ((events[i].interests & SALTS_IO_READY_READ) == 0u) continue;
    if (events[i].token != expected) {
      ++gate.wrong_token;
      continue;
    }
    ++gate.seen;
    if (gate.seen != 1u) continue;
    gate.cancel_status = native_io_backend_cancel(gate.backend, gate.request);
    /* The original borrow survives even a successful cancel request until
     * observe returns the exact terminal. */
    gate.premature_release_status =
        native_io_backend_release_socket(gate.backend, gate.endpoint);
  }
}

static int fail(int line, const char *what, int got) {
  fprintf(stderr, "NativeIO real ready/cancel gate failed at line %d: %s (got %d)\n",
          line, what, got);
  return 1;
}
#define REQUIRE(expr) do { if (!(expr)) return fail(__LINE__, #expr, 0); } while (0)
#define REQUIRE_STATUS(expr, expected) do { \
  const int gate_status = (expr); \
  if (gate_status != (expected)) return fail(__LINE__, #expr, gate_status); \
} while (0)

static native_io_backend_kind test_backend(void) {
#if defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

int main(void) {
  native_io_backend backend = {0};
  native_io_backend_config cfg = {0};
  native_io_endpoint prev_endpoint = {0};
  native_io_request prev_request = {0};
  unsigned round;

  cfg.kind = test_backend();
  cfg.endpoint_capacity = 1u;
  cfg.request_capacity = 1u;
  cfg.completion_batch_capacity = 1u;
  REQUIRE_STATUS(native_io_backend_init(&backend, &cfg), SALTS_OK);
  for (round = 0u; round < GATE_ROUNDS; ++round) {
    const unsigned mode = round % 3u;
    const unsigned char sent = (unsigned char)(0x40u + round);
    unsigned char received = 0xe5u;
    int sockets[2] = {-1, -1};
    native_io_endpoint endpoint = {0};
    native_io_request request = {0};
    native_io_operation op = {0};
    native_io_completion completion = {0};
    size_t count = 0u;
    int flags;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    flags = fcntl(sockets[0], F_GETFL, 0);
    REQUIRE(flags >= 0);
    REQUIRE(fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
    REQUIRE_STATUS(native_io_backend_attach_socket(
                       &backend, (uintptr_t)sockets[0], &endpoint), SALTS_OK);
    if (round != 0u) {
      REQUIRE(endpoint.slot == prev_endpoint.slot);
      REQUIRE(endpoint.generation != prev_endpoint.generation);
    }

    op.kind = NATIVE_IO_OPERATION_STREAM_RECV;
    op.endpoint = endpoint;
    op.buffer = &received;
    op.length = 1u;
    op.user_data = (uintptr_t)(1000u + round);
    REQUIRE_STATUS(native_io_backend_submit(&backend, &op, &request), SALTS_OK);
    if (round != 0u) {
      REQUIRE(request.slot == prev_request.slot);
      REQUIRE(request.generation != prev_request.generation);
    }
    memset(&gate, 0, sizeof(gate));
    gate.backend = &backend;
    gate.endpoint = endpoint;
    gate.request = request;
    gate.armed = mode == GATE_CANCEL_AT_READY;

    if (mode == GATE_CANCEL_BEFORE_PEER_SEND) {
      REQUIRE_STATUS(native_io_backend_cancel(&backend, request), SALTS_OK);
      /* Peer delivers a genuine LATE kernel byte after cancellation.
       * It may never become the old request's second completion. */
      REQUIRE(send(sockets[1], &sent, 1u, 0) == 1);
    } else {
      /* Accepted request is pending before peer writes real kernel bytes.
       * The test hook cancels only AFTER OS readiness is observed. */
      REQUIRE(send(sockets[1], &sent, 1u, 0) == 1);
    }
    REQUIRE_STATUS(native_io_backend_observe(
                       &backend, &completion, 1u, GATE_TIMEOUT_MS, &count), SALTS_OK);
    REQUIRE(count == 1u);
    REQUIRE(completion.request.slot == request.slot);
    REQUIRE(completion.request.generation == request.generation);
    REQUIRE(completion.endpoint.slot == endpoint.slot);
    REQUIRE(completion.endpoint.generation == endpoint.generation);
    REQUIRE(completion.user_data == op.user_data);

    if (mode == GATE_COMPLETE_THEN_CANCEL) {
      REQUIRE(gate.seen == 0u);
      REQUIRE(completion.kind == NATIVE_IO_COMPLETION_OK);
      REQUIRE(completion.status == SALTS_OK);
      REQUIRE(completion.bytes == 1u && received == sent);
    } else {
      REQUIRE(completion.kind == NATIVE_IO_COMPLETION_CANCELLED);
      REQUIRE(completion.status == SALTS_ECANCELED);
      REQUIRE(received == 0xe5u);
      if (mode == GATE_CANCEL_AT_READY) {
        REQUIRE(gate.seen == 1u && gate.wrong_token == 0u);
        REQUIRE(gate.cancel_status == SALTS_OK);
        REQUIRE(gate.premature_release_status == SALTS_EBUSY);
      } else {
        REQUIRE(gate.seen == 0u);
      }
    }

    gate.armed = 0u;
    REQUIRE_STATUS(native_io_backend_cancel(&backend, request), SALTS_ENOENT);
    count = 99u;
    REQUIRE_STATUS(native_io_backend_observe(
                       &backend, &completion, 1u, 0u, &count), SALTS_ETIMEDOUT);
    REQUIRE(count == 0u);
    REQUIRE(close(sockets[0]) == 0);
    REQUIRE_STATUS(native_io_backend_release_socket(&backend, endpoint), SALTS_OK);
    REQUIRE(close(sockets[1]) == 0);
    prev_endpoint = endpoint;
    prev_request = request;
  }
  REQUIRE_STATUS(native_io_backend_close(&backend), SALTS_OK);
  REQUIRE_STATUS(native_io_backend_destroy(&backend), SALTS_OK);
  printf("real %s readiness vs cancel: %u owner-only kernel crosses PASS\n",
         cfg.kind == NATIVE_IO_BACKEND_EPOLL ? "epoll" : "kqueue", GATE_ROUNDS);
  return 0;
}
