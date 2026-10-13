/* #1168: a REAL IOCP completion packet has left
 * GetQueuedCompletionStatus but has not been published to the Owner.
 * This builds ONLY against a private source-copy NativeIO target.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <salts/native_io.h>
#include <salts/error_codes.h>
#include "native_io_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { IOCP_GATE_ROUNDS = 12, IOCP_GATE_TIMEOUT_MS = 5000 };
enum { IOCP_COMPLETE_FIRST = 0, IOCP_DEQUEUED_CANCEL = 1,
       IOCP_CANCEL_FIRST = 2 };

typedef struct iocp_gate {
  native_io_backend *backend;
  native_io_endpoint endpoint;
  native_io_request request;
  unsigned armed;
  unsigned seen;
  unsigned wrong_request;
  int cancel_status;
  int premature_release_status;
} iocp_gate;

static iocp_gate gate;

/* GQCS returned a real OVERLAPPED identity. Reentering only cancel on
 * this SAME NativeIO Owner is a test-only dispatch seam, not a second
 * progress thread or an invented completion. */
void salts_native_io_test_before_iocp_dispatch(
    cmeta_io_impl *base, native_io_request request) {
  if (!gate.armed || !gate.backend || gate.backend->impl != base) return;
  if (request.slot != gate.request.slot ||
      request.generation != gate.request.generation) {
    ++gate.wrong_request;
    return;
  }
  ++gate.seen;
  if (gate.seen != 1u) return;
  gate.cancel_status = native_io_backend_cancel(gate.backend, request);
  gate.premature_release_status =
      native_io_backend_release_socket(gate.backend, gate.endpoint);
}

static int fail(int line, const char *what, int got) {
  fprintf(stderr, "NativeIO IOCP dequeue/cancel failed at line %d: %s (got %d)\n",
          line, what, got);
  return 1;
}
#define REQUIRE(expr) do { if (!(expr)) return fail(__LINE__, #expr, 0); } while (0)
#define REQUIRE_STATUS(expr, expected) do { \
  const int iocp_status = (expr); \
  if (iocp_status != (expected)) return fail(__LINE__, #expr, iocp_status); \
} while (0)

/* Real localhost Winsock stream; the receiver is explicitly created with
 * WSA_FLAG_OVERLAPPED before it is associated with the original IOCP. */
static int make_tcp_pair(SOCKET *receiver_out, SOCKET *sender_out) {
  SOCKET listener = INVALID_SOCKET;
  SOCKET receiver = INVALID_SOCKET;
  SOCKET sender = INVALID_SOCKET;
  struct sockaddr_in address;
  int address_length = (int)sizeof(address);

  *receiver_out = INVALID_SOCKET;
  *sender_out = INVALID_SOCKET;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = 0u;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_OVERLAPPED);
  receiver = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                        WSA_FLAG_OVERLAPPED);
  if (listener == INVALID_SOCKET || receiver == INVALID_SOCKET) goto failed;
  if (bind(listener, (const struct sockaddr *)&address,
           (int)sizeof(address)) == SOCKET_ERROR) goto failed;
  if (listen(listener, 1) == SOCKET_ERROR) goto failed;
  if (getsockname(listener, (struct sockaddr *)&address,
                  &address_length) == SOCKET_ERROR) goto failed;
  if (connect(receiver, (const struct sockaddr *)&address,
              address_length) == SOCKET_ERROR) goto failed;
  sender = accept(listener, NULL, NULL);
  if (sender == INVALID_SOCKET) goto failed;
  (void)closesocket(listener);
  *receiver_out = receiver;
  *sender_out = sender;
  return 0;

failed:
  if (listener != INVALID_SOCKET) (void)closesocket(listener);
  if (receiver != INVALID_SOCKET) (void)closesocket(receiver);
  if (sender != INVALID_SOCKET) (void)closesocket(sender);
  return -1;
}

int main(void) {
  native_io_backend backend = {0};
  native_io_backend_config cfg = {0};
  native_io_endpoint prev_endpoint = {0};
  native_io_request prev_request = {0};
  unsigned round;

  cfg.kind = NATIVE_IO_BACKEND_IOCP;
  cfg.endpoint_capacity = 1u;
  cfg.request_capacity = 1u;
  cfg.completion_batch_capacity = 1u;
  REQUIRE_STATUS(native_io_backend_init(&backend, &cfg), SALTS_OK);
  for (round = 0u; round < IOCP_GATE_ROUNDS; ++round) {
    const unsigned mode = round % 3u;
    const unsigned char sent = (unsigned char)(0x40u + round);
    unsigned char received = 0xe5u;
    SOCKET receiver, sender;
    native_io_endpoint endpoint = {0};
    native_io_request request = {0};
    native_io_operation operation = {0};
    native_io_completion completion = {0};
    size_t count = 0u;

    REQUIRE(make_tcp_pair(&receiver, &sender) == 0);
    REQUIRE_STATUS(native_io_backend_attach_socket(
                       &backend, (uintptr_t)receiver, &endpoint), SALTS_OK);
    if (round != 0u) {
      REQUIRE(endpoint.slot == prev_endpoint.slot);
      REQUIRE(endpoint.generation != prev_endpoint.generation);
    }
    operation.kind = NATIVE_IO_OPERATION_STREAM_RECV;
    operation.endpoint = endpoint;
    operation.buffer = &received;
    operation.length = 1u;
    operation.user_data = (uintptr_t)(2000u + round);
    REQUIRE_STATUS(native_io_backend_submit(&backend, &operation, &request), SALTS_OK);
    if (round != 0u) {
      REQUIRE(request.slot == prev_request.slot);
      REQUIRE(request.generation != prev_request.generation);
    }
    memset(&gate, 0, sizeof(gate));
    gate.backend = &backend;
    gate.endpoint = endpoint;
    gate.request = request;
    gate.armed = mode == IOCP_DEQUEUED_CANCEL;

    if (mode == IOCP_CANCEL_FIRST) {
      REQUIRE_STATUS(native_io_backend_cancel(&backend, request), SALTS_OK);
      /* Cancellation is asynchronous on IOCP. Observe its REAL terminal
       * before releasing the endpoint. This old socket is never reused. */
    } else {
      REQUIRE(send(sender, (const char *)&sent, 1, 0) == 1);
    }
    REQUIRE_STATUS(native_io_backend_observe(
                       &backend, &completion, 1u, IOCP_GATE_TIMEOUT_MS, &count),
                   SALTS_OK);
    REQUIRE(count == 1u);
    REQUIRE(completion.request.slot == request.slot);
    REQUIRE(completion.request.generation == request.generation);
    REQUIRE(completion.endpoint.slot == endpoint.slot);
    REQUIRE(completion.endpoint.generation == endpoint.generation);
    REQUIRE(completion.user_data == operation.user_data);
    if (mode == IOCP_CANCEL_FIRST) {
      REQUIRE(completion.kind == NATIVE_IO_COMPLETION_CANCELLED);
      REQUIRE(completion.status == SALTS_ECANCELED);
      REQUIRE(received == 0xe5u);
      REQUIRE(gate.seen == 0u);
      /* Winsock forbids continued I/O on a socket after CancelIoEx of an
       * outstanding OVERLAPPED operation. Do not send late bytes on the
       * cancelled endpoint: retire it and qualify only a fresh Generation. */
    } else {
      REQUIRE(completion.kind == NATIVE_IO_COMPLETION_OK);
      REQUIRE(completion.status == SALTS_OK);
      REQUIRE(completion.bytes == 1u && received == sent);
      if (mode == IOCP_DEQUEUED_CANCEL) {
        REQUIRE(gate.seen == 1u && gate.wrong_request == 0u);
        REQUIRE(gate.cancel_status == SALTS_EALREADY);
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
    REQUIRE(closesocket(receiver) == 0);
    REQUIRE_STATUS(native_io_backend_release_socket(&backend, endpoint), SALTS_OK);
    REQUIRE(closesocket(sender) == 0);
    prev_endpoint = endpoint;
    prev_request = request;
  }
  REQUIRE_STATUS(native_io_backend_close(&backend), SALTS_OK);
  REQUIRE_STATUS(native_io_backend_destroy(&backend), SALTS_OK);
  puts("real IOCP completion/cancel: 12 owner-only queued packet crossings PASS");
  return 0;
}
