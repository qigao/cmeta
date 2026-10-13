#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#include "native_io_internal.h"
#include "native_io_accept_escrow.h"

/* #1168: private first-party source-copy CQE dispatch test only.
 * This symbol and callsite are excluded from the installed NativeIO target. */
#if defined(SALTS_NATIVE_IO_URING_CQE_TEST_HOOK)
extern void salts_native_io_test_before_uring_dispatch(
    cmeta_io_impl *owner, native_io_request request, int kernel_result);
#endif

#include <salts/clock.h>
#include <salts/error_codes.h>

#include <errno.h>
#include <limits.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(IORING_FEAT_EXT_ARG) && defined(IORING_ENTER_EXT_ARG)
  #define SALTS_IO_URING_HAS_EXT_ARG_WAIT 1
#else
  #define SALTS_IO_URING_HAS_EXT_ARG_WAIT 0
#endif

#if defined(IORING_SETUP_SINGLE_ISSUER)
  #define SALTS_IO_URING_HAS_SINGLE_ISSUER 1
#else
  #define SALTS_IO_URING_HAS_SINGLE_ISSUER 0
#endif

#if SALTS_IO_URING_HAS_SINGLE_ISSUER && SALTS_IO_URING_HAS_EXT_ARG_WAIT && \
    defined(IORING_SETUP_DEFER_TASKRUN)
  #define SALTS_IO_URING_HAS_DEFER_TASKRUN 1
#else
  #define SALTS_IO_URING_HAS_DEFER_TASKRUN 0
#endif

#if SALTS_IO_URING_HAS_DEFER_TASKRUN && defined(IORING_SETUP_TASKRUN_FLAG) && \
    defined(IORING_SQ_TASKRUN) && defined(IORING_SQ_CQ_OVERFLOW)
  #define SALTS_IO_URING_HAS_TASKRUN_FLAG 1
#else
  #define SALTS_IO_URING_HAS_TASKRUN_FLAG 0
#endif

typedef enum cmeta_io_uring_phase {
  SALTS_IO_URING_FREE = 0,
  SALTS_IO_URING_PENDING,
  SALTS_IO_URING_TERMINAL
} cmeta_io_uring_phase;

typedef struct cmeta_io_uring_lane {
  /* Only the head enters the kernel: concurrent recv SQEs need not consume stream bytes FIFO. */
  uint32_t head;
  uint32_t tail;
} cmeta_io_uring_lane;

typedef struct cmeta_io_uring_endpoint {
  int fd;
  uint32_t generation;
  size_t active_requests;
  size_t retained_accepts;
  cmeta_io_uring_lane read_lane;
  cmeta_io_uring_lane write_lane;
  cmeta_io_resource_kind resource_kind;
  bool connected;
  bool connect_active;
  bool active;
} cmeta_io_uring_endpoint;

typedef struct cmeta_io_uring_request_record {
  cmeta_io_uring_phase phase;
  native_io_request request;
  native_io_endpoint endpoint;
  native_io_operation operation;
  struct iovec vectors[NATIVE_IO_VECTOR_MAX];
  size_t vector_count;
  struct msghdr message;
  native_io_completion completion;
  uint64_t native_token;
  uint32_t previous;
  uint32_t next;
  uint32_t staged_previous;
  uint32_t staged_next;
  bool staged;
  bool vector_write;
  bool write_lane;
  bool in_flight;
  bool cancel_requested;
  bool stream_accept;
} cmeta_io_uring_request_record;

typedef struct cmeta_io_uring_accept_result {
  native_io_request request;
  native_io_endpoint listener;
  int child_fd;
  bool live;
} cmeta_io_uring_accept_result;

typedef struct cmeta_io_uring_impl {
  cmeta_io_impl base;
  cmeta_io_uring_endpoint *endpoints;
  cmeta_io_uring_request_record *requests;
  cmeta_io_uring_accept_result *accept_results;
  native_io_accept_escrow accept_escrow;
  uint32_t *free_endpoints;
  uint32_t *free_requests;
  /* Terminal records stay borrowed until observe copies their completion. */
  uint32_t *terminal_requests;
  size_t endpoint_capacity;
  size_t request_capacity;
  size_t completion_batch_capacity;
  size_t free_endpoint_count;
  size_t free_request_count;
  size_t terminal_head;
  size_t terminal_count;
  size_t endpoint_count;
  size_t active_requests;
  uint64_t submitted;
  uint32_t staged_head;
  uint32_t staged_tail;
  uint64_t completed;
  uint64_t cancelled;
  uint64_t failed;
  uint64_t rejected_full;
  uint64_t native_submit_errors;
  uint64_t native_cancel_errors;
  int ring_fd;
  int wake_fd;
  void *sq_ring;
  void *cq_ring;
  struct io_uring_sqe *sqes;
  size_t sq_ring_size;
  size_t cq_ring_size;
  size_t sqes_size;
  unsigned *sq_head;
  unsigned *sq_tail;
  unsigned *sq_mask;
  unsigned *sq_entries;
  unsigned *sq_flags;
  unsigned *sq_array;
  unsigned *cq_head;
  unsigned *cq_tail;
  unsigned *cq_mask;
  struct io_uring_cqe *cqes;
  bool single_mmap;
  bool ring_native_wait;
  bool defer_taskrun;
  bool taskrun_flag;
  bool wake_poll_in_flight;
  bool admission_open;
  atomic_bool wake_pending;
} cmeta_io_uring_impl;

enum { SALTS_IO_URING_CANCEL_TOKEN = 0u, SALTS_IO_URING_INDEX_NONE = UINT32_MAX };
static const uint64_t SALTS_IO_URING_WAKE_TOKEN = UINT64_MAX;

typedef struct cmeta_io_uring_sigpipe_guard {
  sigset_t blocked;
  sigset_t previous;
  bool active;
  bool had_pending;
} cmeta_io_uring_sigpipe_guard;

static void uring_counter_increment(uint64_t *counter) {
  if (*counter != UINT64_MAX) ++*counter;
}

static uint32_t uring_next_generation(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static cmeta_io_uring_endpoint *uring_endpoint(
    cmeta_io_uring_impl *impl, native_io_endpoint endpoint);

static cmeta_io_uring_accept_result *uring_accept_result_from_token(
    cmeta_io_uring_impl *impl, uintptr_t token) {
  const uintptr_t base = (uintptr_t)impl->accept_results;
  const uintptr_t value = token;
  const size_t bytes =
      impl->request_capacity * sizeof(*impl->accept_results);
  uintptr_t offset;
  if (impl->accept_results == NULL || value < base ||
      value - base >= bytes)
    return NULL;
  offset = value - base;
  if (offset % sizeof(*impl->accept_results) != 0u)
    return NULL;
  return &impl->accept_results[
      offset / sizeof(*impl->accept_results)];
}

static int uring_retire_accept_result(
    void *context, uintptr_t transport) {
  cmeta_io_uring_impl *impl =
      (cmeta_io_uring_impl *)context;
  cmeta_io_uring_accept_result *result =
      uring_accept_result_from_token(impl, transport);
  cmeta_io_uring_endpoint *listener;
  int status = SALTS_OK;

  if (result == NULL || !result->live)
    return SALTS_EINVAL;
  listener = uring_endpoint(impl, result->listener);
  if (result->child_fd >= 0 && close(result->child_fd) != 0)
    status = -errno;
  if (listener != NULL && listener->retained_accepts != 0u)
    --listener->retained_accepts;
  memset(result, 0, sizeof(*result));
  result->child_fd = -1;
  return status;
}

static cmeta_io_uring_accept_result *uring_accept_result_reserve(
    cmeta_io_uring_impl *impl,
    native_io_request request,
    native_io_endpoint listener,
    int child_fd) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    cmeta_io_uring_accept_result *result =
        &impl->accept_results[index];
    if (result->live)
      continue;
    result->request = request;
    result->listener = listener;
    result->child_fd = child_fd;
    result->live = true;
    return result;
  }
  return NULL;
}

static int uring_publish_accept_result(
    cmeta_io_uring_impl *impl,
    cmeta_io_uring_request_record *request,
    int child_fd) {
  cmeta_io_uring_accept_result *result;
  cmeta_io_uring_endpoint *listener;
  int status;

  if (child_fd < 0)
    return SALTS_EINVAL;
  result = uring_accept_result_reserve(
      impl, request->request, request->endpoint, child_fd);
  if (result == NULL) {
    (void)close(child_fd);
    return SALTS_ENOBUFS;
  }

  status = native_io_accept_escrow_publish(
      &impl->accept_escrow, request->request,
      (uintptr_t)result);
  if (status != SALTS_OK) {
    (void)close(child_fd);
    memset(result, 0, sizeof(*result));
    result->child_fd = -1;
    return status;
  }

  listener = uring_endpoint(impl, request->endpoint);
  if (listener == NULL) {
    (void)native_io_accept_escrow_discard(
        &impl->accept_escrow, request->request);
    return SALTS_ENOENT;
  }
  ++listener->retained_accepts;
  return SALTS_OK;
}

static int uring_discard_listener_accepts(
    cmeta_io_uring_impl *impl,
    native_io_endpoint listener_handle) {
  int first_status = SALTS_OK;
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    cmeta_io_uring_accept_result *result =
        &impl->accept_results[index];
    int status;
    if (!result->live ||
        result->listener.slot != listener_handle.slot ||
        result->listener.generation !=
            listener_handle.generation)
      continue;
    status = native_io_accept_escrow_discard(
        &impl->accept_escrow, result->request);
    if (first_status == SALTS_OK && status != SALTS_OK)
      first_status = status;
  }
  return first_status;
}

static uint64_t uring_request_token(uint32_t index, uint32_t generation) {
  return ((uint64_t)generation << 32u) | (uint64_t)(index + 1u);
}

static int uring_sigpipe_guard_begin(cmeta_io_uring_sigpipe_guard *guard) {
  sigset_t pending;
  int status;
  memset(guard, 0, sizeof(*guard));
  sigemptyset(&guard->blocked);
  sigaddset(&guard->blocked, SIGPIPE);
  status = pthread_sigmask(SIG_BLOCK, &guard->blocked, &guard->previous);
  if (status != 0) return -status;
  guard->active = true;
  if (sigpending(&pending) == 0) guard->had_pending = sigismember(&pending, SIGPIPE) == 1;
  return SALTS_OK;
}

static void uring_sigpipe_guard_end(cmeta_io_uring_sigpipe_guard *guard) {
  sigset_t pending;
  if (guard == NULL || !guard->active) return;
  if (!guard->had_pending && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
    int signal_number;
    int status;
    do {
      status = sigwait(&guard->blocked, &signal_number);
    } while (status == EINTR);
  }
  (void)pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
  guard->active = false;
}

static cmeta_io_uring_endpoint *uring_endpoint(cmeta_io_uring_impl *impl,
                                               native_io_endpoint endpoint) {
  cmeta_io_uring_endpoint *record;
  if (!native_io_endpoint_valid(endpoint) || endpoint.slot > impl->endpoint_capacity) return NULL;
  record = &impl->endpoints[endpoint.slot - 1u];
  return record->active && record->generation == endpoint.generation ? record : NULL;
}

static cmeta_io_uring_request_record *uring_request(cmeta_io_uring_impl *impl,
                                                    native_io_request request) {
  cmeta_io_uring_request_record *record;
  if (!native_io_request_valid(request) || request.slot > impl->request_capacity) return NULL;
  record = &impl->requests[request.slot - 1u];
  return record->phase != SALTS_IO_URING_FREE && record->request.generation == request.generation
             ? record
             : NULL;
}

static cmeta_io_uring_request_record *uring_record_for_token(cmeta_io_uring_impl *impl,
                                                             uint64_t token) {
  const uint32_t slot = (uint32_t)token;
  const uint32_t generation = (uint32_t)(token >> 32u);
  cmeta_io_uring_request_record *record;
  if (slot == 0u || generation == 0u || slot > impl->request_capacity) return NULL;
  record = &impl->requests[slot - 1u];
  return record->phase == SALTS_IO_URING_PENDING && record->in_flight &&
                 record->native_token == token
             ? record
             : NULL;
}

static bool uring_is_write(native_io_operation_kind kind) {
  return kind == NATIVE_IO_OPERATION_STREAM_SEND || kind == NATIVE_IO_OPERATION_UDP_SEND_TO ||
         kind == NATIVE_IO_OPERATION_STREAM_CONNECT || kind == NATIVE_IO_OPERATION_PIPE_WRITE;
}

static cmeta_io_uring_lane *uring_lane(cmeta_io_uring_endpoint *endpoint, bool write_lane) {
  return write_lane ? &endpoint->write_lane : &endpoint->read_lane;
}

static void uring_lane_push(cmeta_io_uring_impl *impl, cmeta_io_uring_endpoint *endpoint,
                            uint32_t index) {
  cmeta_io_uring_request_record *request = &impl->requests[index];
  cmeta_io_uring_lane *lane = uring_lane(endpoint, request->write_lane);
  request->previous = lane->tail;
  request->next = SALTS_IO_URING_INDEX_NONE;
  if (lane->tail == SALTS_IO_URING_INDEX_NONE) lane->head = index;
  else impl->requests[lane->tail].next = index;
  lane->tail = index;
}

static void uring_lane_remove(cmeta_io_uring_impl *impl, cmeta_io_uring_endpoint *endpoint,
                              uint32_t index) {
  cmeta_io_uring_request_record *request = &impl->requests[index];
  cmeta_io_uring_lane *lane = uring_lane(endpoint, request->write_lane);
  if (request->previous == SALTS_IO_URING_INDEX_NONE) lane->head = request->next;
  else impl->requests[request->previous].next = request->next;
  if (request->next == SALTS_IO_URING_INDEX_NONE) lane->tail = request->previous;
  else impl->requests[request->next].previous = request->previous;
  request->previous = SALTS_IO_URING_INDEX_NONE;
  request->next = SALTS_IO_URING_INDEX_NONE;
}

static int uring_enter_once(cmeta_io_uring_impl *impl, unsigned submit, unsigned minimum,
                            unsigned flags, const void *argument, size_t argument_size) {
  const int status =
      (int)syscall(__NR_io_uring_enter, impl->ring_fd, submit, minimum, flags, argument,
                   argument_size);
  return status < 0 ? -errno : status;
}

static int uring_enter(cmeta_io_uring_impl *impl, unsigned submit, unsigned minimum,
                       unsigned flags) {
  int status;
  do {
    status = uring_enter_once(impl, submit, minimum, flags, NULL, 0u);
  } while (status == -EINTR);
  return status;
}

/* Scheduling links live in the request record: no second payload/owner queue,
 * allocation, or scan over unused endpoint capacity. Admission/removal is O(1). */
static void uring_stage(cmeta_io_uring_impl *impl, uint32_t index) {
  cmeta_io_uring_request_record *request = &impl->requests[index];
  request->staged_previous = impl->staged_tail;
  request->staged_next = SALTS_IO_URING_INDEX_NONE;
  if (impl->staged_tail == SALTS_IO_URING_INDEX_NONE) impl->staged_head = index;
  else impl->requests[impl->staged_tail].staged_next = index;
  impl->staged_tail = index;
  request->staged = true;
}

static void uring_unstage(cmeta_io_uring_impl *impl, uint32_t index) {
  cmeta_io_uring_request_record *request = &impl->requests[index];
  if (!request->staged) return;
  if (request->staged_previous == SALTS_IO_URING_INDEX_NONE)
    impl->staged_head = request->staged_next;
  else impl->requests[request->staged_previous].staged_next = request->staged_next;
  if (request->staged_next == SALTS_IO_URING_INDEX_NONE)
    impl->staged_tail = request->staged_previous;
  else impl->requests[request->staged_next].staged_previous = request->staged_previous;
  request->staged = false;
}

static int uring_publish_sqe(cmeta_io_uring_impl *impl, const struct io_uring_sqe *prepared) {
  const unsigned head =
      atomic_load_explicit((_Atomic unsigned *)impl->sq_head, memory_order_acquire);
  const unsigned tail =
      atomic_load_explicit((_Atomic unsigned *)impl->sq_tail, memory_order_relaxed);
  unsigned index;
  int submitted;
  if (tail - head >= *impl->sq_entries) return SALTS_EBUSY;
  index = tail & *impl->sq_mask;
  impl->sqes[index] = *prepared;
  impl->sq_array[index] = index;
  atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, tail + 1u, memory_order_release);
  submitted = uring_enter(impl, 1u, 0u, 0u);
  if (submitted == 1) return SALTS_OK;
  atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, tail, memory_order_release);
  return submitted < 0 ? submitted : SALTS_EIO;
}

static int uring_arm_wake_poll(cmeta_io_uring_impl *impl) {
  struct io_uring_sqe sqe;
  int status;
  if (!impl->ring_native_wait || impl->wake_poll_in_flight) return SALTS_OK;
  memset(&sqe, 0, sizeof(sqe));
  sqe.opcode = IORING_OP_POLL_ADD;
  sqe.fd = impl->wake_fd;
  sqe.poll_events = POLLIN;
  sqe.user_data = SALTS_IO_URING_WAKE_TOKEN;
  status = uring_publish_sqe(impl, &sqe);
  if (status == SALTS_OK) impl->wake_poll_in_flight = true;
  return status;
}

static void uring_prepare_operation(cmeta_io_uring_request_record *record, struct io_uring_sqe *sqe,
                                    int fd) {
  memset(sqe, 0, sizeof(*sqe));
  sqe->fd = fd;
  sqe->user_data = record->native_token;
  if (record->stream_accept) {
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->addr = 0u;
    sqe->off = 0u;
    sqe->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    return;
  }
  if (record->vector_write) {
    if (record->operation.kind == NATIVE_IO_OPERATION_PIPE_WRITE) {
      sqe->opcode = IORING_OP_WRITEV;
      sqe->addr = (uint64_t)(uintptr_t)record->vectors;
      sqe->len = (uint32_t)record->vector_count;
      sqe->off = UINT64_MAX;
    } else {
      memset(&record->message, 0, sizeof(record->message));
      record->message.msg_iov = record->vectors;
      record->message.msg_iovlen = record->vector_count;
      sqe->opcode = IORING_OP_SENDMSG;
      sqe->addr = (uint64_t)(uintptr_t)&record->message;
      sqe->len = 1u;
      sqe->msg_flags = MSG_NOSIGNAL;
    }
    return;
  }
  if (record->operation.kind == NATIVE_IO_OPERATION_PIPE_READ ||
      record->operation.kind == NATIVE_IO_OPERATION_PIPE_WRITE) {
    sqe->opcode =
        record->operation.kind == NATIVE_IO_OPERATION_PIPE_READ ? IORING_OP_READ : IORING_OP_WRITE;
    sqe->addr = (uint64_t)(uintptr_t)record->operation.buffer;
    sqe->len = (uint32_t)record->operation.length;
    sqe->off = UINT64_MAX;
  } else if (record->operation.kind == NATIVE_IO_OPERATION_STREAM_RECV) {
    sqe->opcode = IORING_OP_RECV;
    sqe->addr = (uint64_t)(uintptr_t)record->operation.buffer;
    sqe->len = (uint32_t)record->operation.length;
  } else if (record->operation.kind == NATIVE_IO_OPERATION_STREAM_SEND) {
    sqe->opcode = IORING_OP_SEND;
    sqe->addr = (uint64_t)(uintptr_t)record->operation.buffer;
    sqe->len = (uint32_t)record->operation.length;
    sqe->msg_flags = MSG_NOSIGNAL;
  } else if (record->operation.kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    sqe->opcode = IORING_OP_CONNECT;
    sqe->addr = (uint64_t)(uintptr_t)record->operation.address;
    sqe->off = (uint64_t)record->operation.address_length;
  } else {
    record->vectors[0].iov_base = record->operation.buffer;
    record->vectors[0].iov_len = record->operation.length;
    memset(&record->message, 0, sizeof(record->message));
    record->message.msg_name = record->operation.address;
    record->message.msg_namelen = (socklen_t)(record->operation.kind == NATIVE_IO_OPERATION_UDP_RECV_FROM
                                                  ? record->operation.address_capacity
                                                  : record->operation.address_length);
    record->message.msg_iov = &record->vectors[0];
    record->message.msg_iovlen = 1u;
    sqe->opcode =
        record->operation.kind == NATIVE_IO_OPERATION_UDP_RECV_FROM ? IORING_OP_RECVMSG : IORING_OP_SENDMSG;
    sqe->addr = (uint64_t)(uintptr_t)&record->message;
    sqe->len = 1u;
    if (record->operation.kind == NATIVE_IO_OPERATION_UDP_SEND_TO) sqe->msg_flags = MSG_NOSIGNAL;
  }
}

static int uring_start_request(cmeta_io_uring_impl *impl, cmeta_io_uring_request_record *request,
                               int fd) {
  cmeta_io_uring_sigpipe_guard sigpipe_guard = {0};
  struct io_uring_sqe sqe;
  const bool guard_sigpipe = request->operation.kind == NATIVE_IO_OPERATION_PIPE_WRITE;
  int status;
  uring_prepare_operation(request, &sqe, fd);
  status = guard_sigpipe ? uring_sigpipe_guard_begin(&sigpipe_guard) : SALTS_OK;
  if (status == SALTS_OK) status = uring_publish_sqe(impl, &sqe);
  uring_sigpipe_guard_end(&sigpipe_guard);
  if (status == SALTS_OK) request->in_flight = true;
  return status;
}

static void uring_release_request(cmeta_io_uring_impl *impl, cmeta_io_uring_request_record *request,
                                  uint32_t index) {
  cmeta_io_uring_endpoint *endpoint = uring_endpoint(impl, request->endpoint);
  if (endpoint != NULL && endpoint->active_requests != 0u) --endpoint->active_requests;
  request->phase = SALTS_IO_URING_FREE;
  request->native_token = 0u;
  request->operation = (native_io_operation){0};
  request->vector_count = 0u;
  request->vector_write = false;
  request->completion = (native_io_completion){0};
  request->previous = SALTS_IO_URING_INDEX_NONE;
  request->next = SALTS_IO_URING_INDEX_NONE;
  request->write_lane = false;
  request->in_flight = false;
  request->cancel_requested = false;
  request->stream_accept = false;
  impl->free_requests[impl->free_request_count++] = index;
  --impl->active_requests;
}

static void uring_make_completion(cmeta_io_uring_impl *impl, cmeta_io_uring_request_record *request,
                                  int result, native_io_completion *completion) {
  const bool cancelled = request->cancel_requested && result == -ECANCELED;
  if (result >= 0 && request->operation.kind == NATIVE_IO_OPERATION_UDP_RECV_FROM &&
      (request->message.msg_flags & MSG_TRUNC) != 0)
    result = -EMSGSIZE;
  if (request->operation.kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    cmeta_io_uring_endpoint *endpoint = uring_endpoint(impl, request->endpoint);
    if (endpoint != NULL) {
      endpoint->connect_active = false;
      endpoint->connected = result == 0;
    }
  }
  *completion = (native_io_completion){request->request,
                                      request->endpoint,
                                      NATIVE_IO_COMPLETION_OK,
                                      0u,
                                      SALTS_OK,
                                      0u,
                                      request->operation.user_data,
                                      0u};
  if (cancelled) {
    completion->kind = NATIVE_IO_COMPLETION_CANCELLED;
    completion->status = SALTS_ECANCELED;
    completion->native_status = ECANCELED;
    uring_counter_increment(&impl->cancelled);
  } else if (result < 0) {
    completion->kind = NATIVE_IO_COMPLETION_FAILED;
    completion->status = result;
    completion->native_status = (uint32_t)(-result);
    uring_counter_increment(&impl->failed);
  } else if ((request->operation.kind == NATIVE_IO_OPERATION_STREAM_RECV ||
              request->operation.kind == NATIVE_IO_OPERATION_PIPE_READ) &&
             result == 0) {
    completion->kind = NATIVE_IO_COMPLETION_EOF;
    completion->status = SALTS_EOF;
  } else {
    completion->bytes = (size_t)result;
    if (request->operation.kind == NATIVE_IO_OPERATION_UDP_RECV_FROM)
      completion->address_length = (size_t)request->message.msg_namelen;
  }
  uring_counter_increment(&impl->completed);
}

static void uring_queue_terminal(cmeta_io_uring_impl *impl, cmeta_io_uring_request_record *request,
                                 uint32_t index, int result) {
  const size_t tail = (impl->terminal_head + impl->terminal_count) % impl->request_capacity;
  request->in_flight = false;
  request->native_token = 0u;
  if (request->stream_accept && result >= 0) {
    const int publish_status =
        uring_publish_accept_result(impl, request, result);
    result = publish_status == SALTS_OK ? 0 : publish_status;
  }
  uring_make_completion(impl, request, result, &request->completion);
  request->phase = SALTS_IO_URING_TERMINAL;
  impl->terminal_requests[tail] = index;
  ++impl->terminal_count;
}

static void uring_start_lane(cmeta_io_uring_impl *impl, cmeta_io_uring_lane *lane) {
  if (lane->head != SALTS_IO_URING_INDEX_NONE) {
    cmeta_io_uring_request_record *request = &impl->requests[lane->head];
    if (!request->in_flight && !request->staged) uring_stage(impl, lane->head);
  }
}

static void uring_fail_staged(cmeta_io_uring_impl *impl, int status) {
  while (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
    const uint32_t index = impl->staged_head;
    cmeta_io_uring_request_record *request = &impl->requests[index];
    cmeta_io_uring_endpoint *endpoint = uring_endpoint(impl, request->endpoint);
    cmeta_io_uring_lane *lane = uring_lane(endpoint, request->write_lane);
    uring_unstage(impl, index);
    uring_lane_remove(impl, endpoint, index);
    uring_queue_terminal(impl, request, index, status);
    uring_counter_increment(&impl->native_submit_errors);
    uring_start_lane(impl, lane);
  }
}

/* O(eligible requests), including short enters: keep the prepared suffix in SQ
 * instead of rebuilding it on every retry. With no SQPOLL, an error can withdraw
 * only that unconsumed suffix without releasing any kernel-owned storage. */
static int uring_flush(cmeta_io_impl *base) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  while (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
    const unsigned head = atomic_load_explicit((_Atomic unsigned *)impl->sq_head, memory_order_acquire);
    const unsigned tail = atomic_load_explicit((_Atomic unsigned *)impl->sq_tail, memory_order_relaxed);
    const unsigned available = *impl->sq_entries - (tail - head);
    unsigned count = 0u;
    uint32_t index = impl->staged_head;
    bool pipe_write = false;
    cmeta_io_uring_sigpipe_guard guard = {0};
    int status;
    while (index != SALTS_IO_URING_INDEX_NONE && count < available) {
      cmeta_io_uring_request_record *request = &impl->requests[index];
      const unsigned slot = (tail + count) & *impl->sq_mask;
      uring_prepare_operation(request, &impl->sqes[slot],
                              impl->endpoints[request->endpoint.slot - 1u].fd);
      impl->sq_array[slot] = slot;
      pipe_write |= request->operation.kind == NATIVE_IO_OPERATION_PIPE_WRITE;
      index = request->staged_next;
      ++count;
    }
    status = count == 0u ? SALTS_EBUSY
                        : pipe_write ? uring_sigpipe_guard_begin(&guard) : SALTS_OK;
    if (status == SALTS_OK) {
      unsigned remaining = count;
      unsigned consumed_head = head;
      atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, tail + count, memory_order_release);
      do {
        status = uring_enter(impl, remaining, 0u, 0u);
        const unsigned next_head = atomic_load_explicit((_Atomic unsigned *)impl->sq_head,
                                                         memory_order_acquire);
        const unsigned consumed = next_head - consumed_head;
        if (consumed > remaining) {
          uring_sigpipe_guard_end(&guard);
          return SALTS_EPROTO;
        }
        for (unsigned i = 0u; i < consumed; ++i) {
          const uint32_t accepted = impl->staged_head;
          uring_unstage(impl, accepted);
          impl->requests[accepted].in_flight = true;
        }
        remaining -= consumed;
        consumed_head = next_head;
        if (status >= 0) status = consumed == 0u ? SALTS_EIO : SALTS_OK;
        if (status != SALTS_OK) {
          atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, next_head, memory_order_release);
          break;
        }
      } while (remaining != 0u);
    }
    uring_sigpipe_guard_end(&guard);
    if (status != SALTS_OK) {
      uring_fail_staged(impl, status);
      return status;
    }
  }
  return SALTS_OK;
}

static int uring_attach_endpoint(cmeta_io_uring_impl *impl, int fd,
                                 cmeta_io_resource_kind resource_kind, bool connected,
                                 native_io_endpoint *out_endpoint) {
  cmeta_io_uring_endpoint *endpoint;
  uint32_t index;
  size_t cursor;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  for (cursor = 0u; cursor < impl->endpoint_capacity; ++cursor)
    if (impl->endpoints[cursor].active && impl->endpoints[cursor].fd == fd) return SALTS_EALREADY;
  if (impl->free_endpoint_count == 0u) return SALTS_ENOBUFS;
  index = impl->free_endpoints[--impl->free_endpoint_count];
  endpoint = &impl->endpoints[index];
  endpoint->fd = fd;
  endpoint->generation = uring_next_generation(endpoint->generation);
  endpoint->active_requests = 0u;
  endpoint->retained_accepts = 0u;
  endpoint->read_lane = (cmeta_io_uring_lane){SALTS_IO_URING_INDEX_NONE, SALTS_IO_URING_INDEX_NONE};
  endpoint->write_lane =
      (cmeta_io_uring_lane){SALTS_IO_URING_INDEX_NONE, SALTS_IO_URING_INDEX_NONE};
  endpoint->resource_kind = resource_kind;
  endpoint->connected = connected;
  endpoint->connect_active = false;
  endpoint->active = true;
  ++impl->endpoint_count;
  *out_endpoint = (native_io_endpoint){index + 1u, endpoint->generation};
  return SALTS_OK;
}

static int uring_attach_socket(cmeta_io_impl *base, uintptr_t native_socket,
                               native_io_endpoint *out_endpoint) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  cmeta_io_resource_kind resource_kind;
  struct sockaddr_storage peer_address;
  socklen_t peer_address_length = (socklen_t)sizeof(peer_address);
  bool connected = false;
  int socket_type = 0;
  socklen_t option_length = (socklen_t)sizeof(socket_type);
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (native_socket > (uintptr_t)INT_MAX) return SALTS_EINVAL;
  if (getsockopt((int)native_socket, SOL_SOCKET, SO_TYPE, &socket_type, &option_length) != 0)
    return -errno;
  if (socket_type == SOCK_STREAM)
    resource_kind = SALTS_IO_RESOURCE_STREAM_SOCKET;
  else if (socket_type == SOCK_DGRAM)
    resource_kind = SALTS_IO_RESOURCE_DATAGRAM_SOCKET;
  else
    return SALTS_ENOTSUP;
  if (resource_kind == SALTS_IO_RESOURCE_STREAM_SOCKET) {
    if (getpeername((int)native_socket, (struct sockaddr *)&peer_address, &peer_address_length) == 0)
      connected = true;
    else if (errno != ENOTCONN)
      return -errno;
  }
  return uring_attach_endpoint(impl, (int)native_socket, resource_kind, connected, out_endpoint);
}

static int uring_attach_pipe(cmeta_io_impl *base, uintptr_t native_handle, uint32_t flags,
                             native_io_endpoint *out_endpoint) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  struct stat descriptor_stat;
  int status;
  int fd;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (flags != NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE) return SALTS_EINVAL;
  if (native_handle > (uintptr_t)INT_MAX) return SALTS_EINVAL;
  fd = (int)native_handle;
  do {
    status = fstat(fd, &descriptor_stat);
  } while (status < 0 && errno == EINTR);
  if (status < 0) return -errno;
  if (!S_ISFIFO(descriptor_stat.st_mode)) return SALTS_EINVAL;
  return uring_attach_endpoint(impl, fd, SALTS_IO_RESOURCE_BYTE_PIPE, false, out_endpoint);
}

static int uring_release_endpoint(cmeta_io_uring_impl *impl, native_io_endpoint endpoint_handle,
                                  bool socket_endpoint) {
  cmeta_io_uring_endpoint *endpoint = uring_endpoint(impl, endpoint_handle);
  uint32_t index;
  if (endpoint == NULL) return SALTS_ENOENT;
  if (socket_endpoint ? !native_io_resource_kind_is_socket(endpoint->resource_kind)
                      : endpoint->resource_kind != SALTS_IO_RESOURCE_BYTE_PIPE)
    return SALTS_EINVAL;
  if (endpoint->active_requests != 0u) return SALTS_EBUSY;
  if (endpoint->retained_accepts != 0u) {
    int retire_status;
    if (impl->admission_open)
      return SALTS_EBUSY;
    retire_status = uring_discard_listener_accepts(
        impl, endpoint_handle);
    if (endpoint->retained_accepts != 0u)
      return retire_status == SALTS_OK
                 ? SALTS_EPROTO
                 : retire_status;
    if (retire_status != SALTS_OK)
      return retire_status;
  }
  index = endpoint_handle.slot - 1u;
  endpoint->active = false;
  endpoint->fd = -1;
  endpoint->resource_kind = (cmeta_io_resource_kind)0;
  endpoint->retained_accepts = 0u;
  endpoint->connected = false;
  endpoint->connect_active = false;
  impl->free_endpoints[impl->free_endpoint_count++] = index;
  --impl->endpoint_count;
  return SALTS_OK;
}

static int uring_release_socket(cmeta_io_impl *base, native_io_endpoint endpoint_handle) {
  return uring_release_endpoint((cmeta_io_uring_impl *)base, endpoint_handle, true);
}

static int uring_release_pipe(cmeta_io_impl *base, native_io_endpoint endpoint_handle) {
  return uring_release_endpoint((cmeta_io_uring_impl *)base, endpoint_handle, false);
}

static int uring_admit(cmeta_io_impl *base, const native_io_operation *operation,
                       const native_io_vector_operation *vector_operation,
                       native_io_request *out_request, bool prepared) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  cmeta_io_uring_endpoint *endpoint;
  cmeta_io_uring_request_record *request;
  cmeta_io_uring_lane *lane;
  uint32_t index;
  int status;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  endpoint = uring_endpoint(impl, operation->endpoint);
  if (endpoint == NULL) return SALTS_ENOENT;
  if (native_io_operation_resource_kind(operation->kind) != endpoint->resource_kind)
    return SALTS_EINVAL;
  if (operation->kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    if (endpoint->connected || endpoint->connect_active) return SALTS_EALREADY;
    if (endpoint->active_requests != 0u) return SALTS_EBUSY;
  } else if (operation->kind == NATIVE_IO_OPERATION_STREAM_RECV ||
             operation->kind == NATIVE_IO_OPERATION_STREAM_SEND) {
    if (endpoint->connect_active) return SALTS_EBUSY;
    if (!endpoint->connected) return SALTS_EINVAL;
  }
  if (impl->free_request_count == 0u) {
    uring_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }
  index = impl->free_requests[--impl->free_request_count];
  request = &impl->requests[index];
  request->phase = SALTS_IO_URING_PENDING;
  request->request =
      (native_io_request){index + 1u, uring_next_generation(request->request.generation)};
  request->endpoint = operation->endpoint;
  request->operation = *operation;
  request->vector_count = vector_operation != NULL ? vector_operation->span_count : 0u;
  request->vector_write = vector_operation != NULL;
  if (vector_operation != NULL) {
    for (size_t cursor = 0u; cursor < vector_operation->span_count; ++cursor) {
      request->vectors[cursor].iov_base = vector_operation->spans[cursor].data;
      request->vectors[cursor].iov_len = vector_operation->spans[cursor].length;
    }
  }
  request->native_token = uring_request_token(index, request->request.generation);
  request->completion = (native_io_completion){0};
  request->previous = SALTS_IO_URING_INDEX_NONE;
  request->next = SALTS_IO_URING_INDEX_NONE;
  request->write_lane = uring_is_write(operation->kind);
  request->in_flight = false;
  request->cancel_requested = false;
  ++endpoint->active_requests;
  request->staged = false;
  ++impl->active_requests;
  if (operation->kind == NATIVE_IO_OPERATION_STREAM_CONNECT) endpoint->connect_active = true;
  lane = uring_lane(endpoint, request->write_lane);
  uring_lane_push(impl, endpoint, index);
  if (lane->head == index) {
    if (prepared) {
      uring_stage(impl, index);
      status = SALTS_OK;
    } else status = uring_start_request(impl, request, endpoint->fd);
    if (status != SALTS_OK) {
      uring_lane_remove(impl, endpoint, index);
      endpoint->connect_active = false;
      uring_release_request(impl, request, index);
      uring_counter_increment(&impl->native_submit_errors);
      return status;
    }
  }
  uring_counter_increment(&impl->submitted);
  *out_request = request->request;
  return SALTS_OK;
}

static int uring_submit(cmeta_io_impl *base, const native_io_operation *operation,
                        native_io_request *out_request) {
  return uring_admit(base, operation, NULL, out_request, false);
}

static int uring_submit_vector(cmeta_io_impl *base,
                               const native_io_vector_operation *operation,
                               native_io_request *out_request) {
  native_io_operation scalar = {
      .kind = operation->kind,
      .endpoint = operation->endpoint,
      .buffer = operation->spans[0].data,
      .length = operation->spans[0].length,
      .user_data = operation->user_data};
  return uring_admit(base, &scalar, operation, out_request, false);
}

static int uring_prepare(cmeta_io_impl *base, const native_io_operation *operation,
                         native_io_request *out_request) {
  return uring_admit(base, operation, NULL, out_request, true);
}

static int uring_submit_stream_accept(
    cmeta_io_impl *base,
    native_io_endpoint listener_handle,
    native_io_request *out_request) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  cmeta_io_uring_endpoint *listener;
  cmeta_io_uring_request_record *request;
  cmeta_io_uring_lane *lane;
  int accepting = 0;
  socklen_t option_length = (socklen_t)sizeof(accepting);
  uint32_t index;
  int status;

  if (out_request != NULL)
    *out_request = (native_io_request){0};
  if (!impl->admission_open || out_request == NULL)
    return !impl->admission_open
               ? SALTS_ESHUTDOWN
               : SALTS_EINVAL;
  listener = uring_endpoint(impl, listener_handle);
  if (listener == NULL)
    return SALTS_ENOENT;
  if (listener->resource_kind != SALTS_IO_RESOURCE_STREAM_SOCKET ||
      listener->connected || listener->connect_active)
    return SALTS_EINVAL;
  if (getsockopt(listener->fd, SOL_SOCKET, SO_ACCEPTCONN,
                 &accepting, &option_length) != 0)
    return -errno;
  if (!accepting)
    return SALTS_EINVAL;
  if (listener->active_requests != 0u)
    return SALTS_EBUSY;
  if (impl->free_request_count == 0u) {
    uring_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }

  index = impl->free_requests[--impl->free_request_count];
  request = &impl->requests[index];
  request->phase = SALTS_IO_URING_PENDING;
  request->request = (native_io_request){
      index + 1u,
      uring_next_generation(request->request.generation)};
  request->endpoint = listener_handle;
  request->operation = (native_io_operation){0};
  request->vector_count = 0u;
  request->vector_write = false;
  request->native_token =
      uring_request_token(index, request->request.generation);
  request->completion = (native_io_completion){0};
  request->previous = SALTS_IO_URING_INDEX_NONE;
  request->next = SALTS_IO_URING_INDEX_NONE;
  request->staged_previous = SALTS_IO_URING_INDEX_NONE;
  request->staged_next = SALTS_IO_URING_INDEX_NONE;
  request->staged = false;
  request->write_lane = false;
  request->in_flight = false;
  request->cancel_requested = false;
  request->stream_accept = true;
  ++listener->active_requests;
  ++impl->active_requests;

  lane = &listener->read_lane;
  uring_lane_push(impl, listener, index);
  if (lane->head != index) {
    uring_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }

  status = uring_start_request(impl, request, listener->fd);
  if (status != SALTS_OK) {
    uring_lane_remove(impl, listener, index);
    uring_release_request(impl, request, index);
    uring_counter_increment(&impl->native_submit_errors);
    return status;
  }

  uring_counter_increment(&impl->submitted);
  *out_request = request->request;
  return SALTS_OK;
}

static int uring_take_stream_accept(
    cmeta_io_impl *base,
    native_io_request request,
    uintptr_t *out_transport) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  uintptr_t token = UINTPTR_MAX;
  cmeta_io_uring_accept_result *result;
  cmeta_io_uring_endpoint *listener;
  int child_fd;
  int status;

  if (out_transport == NULL)
    return SALTS_EINVAL;
  *out_transport = UINTPTR_MAX;
  status = native_io_accept_escrow_take(
      &impl->accept_escrow, request, &token);
  if (status != SALTS_OK)
    return status;
  result = uring_accept_result_from_token(impl, token);
  if (result == NULL || !result->live ||
      result->request.slot != request.slot ||
      result->request.generation != request.generation)
    return SALTS_EPROTO;

  listener = uring_endpoint(impl, result->listener);
  child_fd = result->child_fd;
  if (listener != NULL && listener->retained_accepts != 0u)
    --listener->retained_accepts;
  memset(result, 0, sizeof(*result));
  result->child_fd = -1;
  if (child_fd < 0)
    return SALTS_EPROTO;
  *out_transport = (uintptr_t)child_fd;
  return SALTS_OK;
}

static bool uring_supports_vector_write(const cmeta_io_impl *base,
                                        native_io_endpoint endpoint_handle) {
  const cmeta_io_uring_impl *impl = (const cmeta_io_uring_impl *)base;
  const cmeta_io_uring_endpoint *endpoint;
  if (!native_io_endpoint_valid(endpoint_handle) ||
      endpoint_handle.slot > impl->endpoint_capacity)
    return false;
  endpoint = &impl->endpoints[endpoint_handle.slot - 1u];
  return endpoint->active && endpoint->generation == endpoint_handle.generation &&
         (endpoint->resource_kind == SALTS_IO_RESOURCE_STREAM_SOCKET ||
          endpoint->resource_kind == SALTS_IO_RESOURCE_BYTE_PIPE);
}

static int uring_cancel(cmeta_io_impl *base, native_io_request request_handle) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  cmeta_io_uring_request_record *request = uring_request(impl, request_handle);
  cmeta_io_uring_endpoint *endpoint;
  struct io_uring_sqe sqe;
  uint32_t index;
  int status;
  if (request == NULL) return SALTS_ENOENT;
  if (request->phase == SALTS_IO_URING_TERMINAL) return SALTS_EALREADY;
  if (request->cancel_requested) return SALTS_EALREADY;
  request->cancel_requested = true;
  if (!request->in_flight) {
    endpoint = uring_endpoint(impl, request->endpoint);
    if (endpoint == NULL) return SALTS_ENOENT;
    index = request->request.slot - 1u;
    cmeta_io_uring_lane *lane = uring_lane(endpoint, request->write_lane);
    uring_unstage(impl, index);
    uring_lane_remove(impl, endpoint, index);
    uring_queue_terminal(impl, request, index, -ECANCELED);
    uring_start_lane(impl, lane);
    return SALTS_OK;
  }
  memset(&sqe, 0, sizeof(sqe));
  sqe.opcode = IORING_OP_ASYNC_CANCEL;
  sqe.fd = -1;
  sqe.addr = request->native_token;
  sqe.user_data = SALTS_IO_URING_CANCEL_TOKEN;
  status = uring_publish_sqe(impl, &sqe);
  if (status != SALTS_OK) {
    request->cancel_requested = false;
    uring_counter_increment(&impl->native_cancel_errors);
    return status;
  }
  return SALTS_OK;
}

static int uring_drain_wake_fd(cmeta_io_uring_impl *impl) {
  uint64_t wake_count;
  ssize_t read_status;
  do {
    read_status = read(impl->wake_fd, &wake_count, sizeof(wake_count));
  } while (read_status < 0 && errno == EINTR);
  if (read_status < 0 && errno != EAGAIN) return -errno;
  atomic_store_explicit(&impl->wake_pending, false, memory_order_release);
  return SALTS_OK;
}

static int uring_process_cq(cmeta_io_uring_impl *impl, bool *saw_wake) {
  unsigned head = atomic_load_explicit((_Atomic unsigned *)impl->cq_head, memory_order_relaxed);
  const unsigned tail =
      atomic_load_explicit((_Atomic unsigned *)impl->cq_tail, memory_order_acquire);
  bool wake_completed = false;
  int wake_status = SALTS_OK;
  while (head != tail) {
    const struct io_uring_cqe *cqe = &impl->cqes[head & *impl->cq_mask];
    const uint64_t token = cqe->user_data;
    const int result = cqe->res;
    ++head;
    if (token == SALTS_IO_URING_CANCEL_TOKEN) {
      if (result < 0 && result != -ENOENT && result != -EALREADY)
        uring_counter_increment(&impl->native_cancel_errors);
      continue;
    }
    if (token == SALTS_IO_URING_WAKE_TOKEN) {
      impl->wake_poll_in_flight = false;
      if (result >= 0) {
        wake_status = uring_drain_wake_fd(impl);
        if (wake_status == SALTS_OK) wake_completed = true;
      } else if (result != -ECANCELED) {
        wake_status = result;
      }
      continue;
    }
    {
      cmeta_io_uring_request_record *request = uring_record_for_token(impl, token);
      if (request != NULL) {
#if defined(SALTS_NATIVE_IO_URING_CQE_TEST_HOOK)
        /* Real CQE token and result have been read from kernel shared CQ.
         * Same Owner calls cancellation before resolving this terminal;
         * NEVER fabricate CQEs or call the backend from another thread. */
        salts_native_io_test_before_uring_dispatch(
            &impl->base, request->request, result);
#endif
        const uint32_t index = request->request.slot - 1u;
        cmeta_io_uring_endpoint *endpoint = uring_endpoint(impl, request->endpoint);
        cmeta_io_uring_lane *lane;
        if (endpoint == NULL) continue;
        lane = uring_lane(endpoint, request->write_lane);
        request->in_flight = false;
        uring_lane_remove(impl, endpoint, index);
        uring_queue_terminal(impl, request, index, result);
        uring_start_lane(impl, lane);
      }
    }
  }
  atomic_store_explicit((_Atomic unsigned *)impl->cq_head, head, memory_order_release);
  if (wake_status != SALTS_OK) return wake_status;
  if (wake_completed) {
    if (saw_wake != NULL) *saw_wake = true;
    if (impl->admission_open) return uring_arm_wake_poll(impl);
  }
  return SALTS_OK;
}

static void uring_drain_terminals(cmeta_io_uring_impl *impl, native_io_completion *events,
                                  size_t limit, size_t *out_count) {
  while (impl->terminal_count != 0u && *out_count < limit) {
    const uint32_t index = impl->terminal_requests[impl->terminal_head];
    cmeta_io_uring_request_record *request = &impl->requests[index];
    events[*out_count] = request->completion;
    ++*out_count;
    impl->terminal_head = (impl->terminal_head + 1u) % impl->request_capacity;
    --impl->terminal_count;
    uring_release_request(impl, request, index);
  }
}

static int uring_progress_cq(cmeta_io_uring_impl *impl, bool *saw_wake) {
  return uring_process_cq(impl, saw_wake);
}

static int uring_flush_promoted(cmeta_io_uring_impl *impl, bool *saw_wake) {
  while (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
    int status = uring_flush(&impl->base);
    if (status != SALTS_OK) return status;
    status = uring_progress_cq(impl, saw_wake);
    if (status != SALTS_OK) return status;
  }
  return SALTS_OK;
}

static int uring_submit_staged_and_wait(cmeta_io_uring_impl *impl, uint32_t timeout_ms,
                                        bool *timed_out) {
#if SALTS_IO_URING_HAS_EXT_ARG_WAIT
  struct {
    int64_t tv_sec;
    int64_t tv_nsec;
  } timeout;
  struct io_uring_getevents_arg argument;
#endif
  unsigned enter_flags = IORING_ENTER_GETEVENTS;
  const void *enter_argument = NULL;
  size_t enter_argument_size = 0u;
  unsigned count = 0u;
  unsigned head = atomic_load_explicit((_Atomic unsigned *)impl->sq_head, memory_order_acquire);
  const unsigned tail = atomic_load_explicit((_Atomic unsigned *)impl->sq_tail, memory_order_relaxed);
  const unsigned available = *impl->sq_entries - (tail - head);
  uint32_t index = impl->staged_head;
  bool pipe_write = false;
  cmeta_io_uring_sigpipe_guard guard = {0};
  int status;

  *timed_out = false;
  while (index != SALTS_IO_URING_INDEX_NONE && count < available) {
    cmeta_io_uring_request_record *request = &impl->requests[index];
    const unsigned slot = (tail + count) & *impl->sq_mask;
    uring_prepare_operation(request, &impl->sqes[slot],
                            impl->endpoints[request->endpoint.slot - 1u].fd);
    impl->sq_array[slot] = slot;
    pipe_write |= request->operation.kind == NATIVE_IO_OPERATION_PIPE_WRITE;
    index = request->staged_next;
    ++count;
  }

  if (timeout_ms != UINT32_MAX) {
#if SALTS_IO_URING_HAS_EXT_ARG_WAIT
    timeout.tv_sec = (int64_t)(timeout_ms / 1000u);
    timeout.tv_nsec = (int64_t)(timeout_ms % 1000u) * 1000000ll;
    memset(&argument, 0, sizeof(argument));
    argument.sigmask_sz = (uint32_t)(_NSIG / 8u);
    argument.ts = (uint64_t)(uintptr_t)&timeout;
    enter_flags |= IORING_ENTER_EXT_ARG;
    enter_argument = &argument;
    enter_argument_size = sizeof(argument);
#else
    return SALTS_ENOTSUP;
#endif
  }

  status = pipe_write ? uring_sigpipe_guard_begin(&guard) : SALTS_OK;
  if (status != SALTS_OK) return status;
  if (count != 0u)
    atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, tail + count, memory_order_release);

  {
    unsigned remaining = count;
    unsigned consumed_head = head;
    bool waited = false;
    do {
      status = uring_enter_once(impl, remaining, waited ? 0u : 1u,
                                waited ? 0u : enter_flags,
                                waited ? NULL : enter_argument,
                                waited ? 0u : enter_argument_size);
      const unsigned next_head =
          atomic_load_explicit((_Atomic unsigned *)impl->sq_head, memory_order_acquire);
      const unsigned consumed = next_head - consumed_head;
      if (consumed > remaining) {
        uring_sigpipe_guard_end(&guard);
        return SALTS_EPROTO;
      }
      for (unsigned i = 0u; i < consumed; ++i) {
        const uint32_t accepted = impl->staged_head;
        uring_unstage(impl, accepted);
        impl->requests[accepted].in_flight = true;
      }
      remaining -= consumed;
      consumed_head = next_head;
      if (status == -ETIME) {
        *timed_out = true;
        if (remaining != 0u)
          atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, next_head, memory_order_release);
        break;
      }
      if (status == -EINTR) {
        if (remaining != 0u)
          atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, next_head, memory_order_release);
        break;
      }
      if (status < 0) {
        if (remaining != 0u)
          atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, next_head, memory_order_release);
        uring_fail_staged(impl, status);
        break;
      }
      if (remaining == 0u) break;
      if (consumed == 0u) {
        atomic_store_explicit((_Atomic unsigned *)impl->sq_tail, next_head, memory_order_release);
        status = SALTS_EIO;
        uring_fail_staged(impl, status);
        break;
      }
      waited = true;
    } while (remaining != 0u);
  }
  uring_sigpipe_guard_end(&guard);
  return status >= 0 ? SALTS_OK : status;
}

static int uring_observe_poll_fallback(cmeta_io_uring_impl *impl,
                                       native_io_completion *events, size_t limit,
                                       uint64_t started_ms, uint32_t timeout_ms,
                                       size_t *out_count) {
  uint32_t wait_timeout = timeout_ms;
  for (;;) {
    struct pollfd descriptors[2] = {{impl->ring_fd, POLLIN, 0}, {impl->wake_fd, POLLIN, 0}};
    const int native_timeout = wait_timeout == UINT32_MAX         ? -1
                               : wait_timeout > (uint32_t)INT_MAX ? INT_MAX
                                                                  : (int)wait_timeout;
    int status = poll(descriptors, 2u, native_timeout);
    if ((status < 0 && errno == EINTR) || status == 0) {
      wait_timeout = native_io_remaining_timeout(started_ms, timeout_ms);
      if (wait_timeout == 0u) return SALTS_ETIMEDOUT;
      continue;
    }
    if (status < 0) return -errno;
    if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0 ||
        (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0)
      return SALTS_EIO;
    {
      bool saw_wake = false;
      status = uring_progress_cq(impl, &saw_wake);
      if (status != SALTS_OK) return status;
      while (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
        status = uring_flush(&impl->base);
        if (status != SALTS_OK) return status;
        status = uring_progress_cq(impl, &saw_wake);
        if (status != SALTS_OK) return status;
      }
      uring_drain_terminals(impl, events, limit, out_count);
      if (*out_count != 0u || saw_wake) return SALTS_OK;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      status = uring_drain_wake_fd(impl);
      if (status != SALTS_OK) return status;
      return SALTS_OK;
    }
    wait_timeout = native_io_remaining_timeout(started_ms, timeout_ms);
    if (wait_timeout == 0u) return SALTS_ETIMEDOUT;
  }
}

static bool uring_nonblocking_enter_required(const cmeta_io_uring_impl *impl) {
#if SALTS_IO_URING_HAS_TASKRUN_FLAG
  if (impl->taskrun_flag && impl->sq_flags != NULL) {
    const unsigned flags =
        atomic_load_explicit((const _Atomic unsigned *)impl->sq_flags, memory_order_acquire);
    return (flags & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) != 0u;
  }
#else
  (void)impl;
#endif
  /* Without TASKRUN_FLAG the backend cannot distinguish idle from deferred
   * task-work, so preserve the existing conservative GETEVENTS enter. */
  return true;
}

static int uring_observe(cmeta_io_impl *base, native_io_completion *events, size_t event_capacity,
                         uint32_t timeout_ms, size_t *out_count) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  const size_t limit = event_capacity < impl->completion_batch_capacity
                           ? event_capacity
                           : impl->completion_batch_capacity;
  const uint64_t started_ms = cmeta_monotonic_ms();
  bool saw_wake = false;
  int status = uring_progress_cq(impl, &saw_wake);
  if (status != SALTS_OK) return status;
  if (impl->terminal_count != 0u || saw_wake) {
    status = uring_flush_promoted(impl, &saw_wake);
    if (status != SALTS_OK) return status;
    uring_drain_terminals(impl, events, limit, out_count);
    if (*out_count != 0u || saw_wake) return SALTS_OK;
  }

  if (!impl->ring_native_wait) {
    while (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
      status = uring_flush(&impl->base);
      if (status != SALTS_OK) return status;
      status = uring_progress_cq(impl, &saw_wake);
      if (status != SALTS_OK) return status;
      uring_drain_terminals(impl, events, limit, out_count);
      if (*out_count != 0u || saw_wake) return SALTS_OK;
    }
    return uring_observe_poll_fallback(impl, events, limit, started_ms, timeout_ms, out_count);
  }

  if (timeout_ms == 0u) {
    if (impl->defer_taskrun) {
      if (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
        status = uring_flush(&impl->base);
        if (status != SALTS_OK) return status;

        /* A just-submitted request may already have a visible CQE. Return it
         * before consulting SQ task-work hints or making another kernel entry. */
        saw_wake = false;
        status = uring_progress_cq(impl, &saw_wake);
        if (status != SALTS_OK) return status;
        if (impl->terminal_count != 0u || saw_wake) {
          status = uring_flush_promoted(impl, &saw_wake);
          if (status != SALTS_OK) return status;
          uring_drain_terminals(impl, events, limit, out_count);
          if (*out_count != 0u || saw_wake) return SALTS_OK;
        }
      }

      if (!uring_nonblocking_enter_required(impl)) return SALTS_ETIMEDOUT;

      status = uring_enter(impl, 0u, 0u, IORING_ENTER_GETEVENTS);
      if (status < 0) return status;
      saw_wake = false;
      status = uring_progress_cq(impl, &saw_wake);
      if (status != SALTS_OK) return status;
      if (impl->terminal_count != 0u || saw_wake) {
        status = uring_flush_promoted(impl, &saw_wake);
        if (status != SALTS_OK) return status;
        uring_drain_terminals(impl, events, limit, out_count);
        if (*out_count != 0u || saw_wake) return SALTS_OK;
      }
      return SALTS_ETIMEDOUT;
    }
    if (impl->staged_head != SALTS_IO_URING_INDEX_NONE) {
      status = uring_flush(&impl->base);
      if (status != SALTS_OK) return status;
      status = uring_progress_cq(impl, &saw_wake);
      if (status != SALTS_OK) return status;
      uring_drain_terminals(impl, events, limit, out_count);
      if (*out_count != 0u || saw_wake) return SALTS_OK;
    }
    return SALTS_ETIMEDOUT;
  }

  for (;;) {
    uint32_t wait_timeout = native_io_remaining_timeout(started_ms, timeout_ms);
    bool timed_out = false;
    if (timeout_ms == UINT32_MAX) wait_timeout = UINT32_MAX;
    if (wait_timeout == 0u) return SALTS_ETIMEDOUT;
    status = uring_submit_staged_and_wait(impl, wait_timeout, &timed_out);
    if (status != SALTS_OK && status != -EINTR && !timed_out) return status;

    saw_wake = false;
    {
      const int progress_status = uring_progress_cq(impl, &saw_wake);
      if (progress_status != SALTS_OK) return progress_status;
    }
    if (impl->terminal_count != 0u || saw_wake) {
      const int flush_status = uring_flush_promoted(impl, &saw_wake);
      if (flush_status != SALTS_OK) return flush_status;
      uring_drain_terminals(impl, events, limit, out_count);
      if (*out_count != 0u) return SALTS_OK;
      if (saw_wake) return SALTS_OK;
    }
    if (timed_out) return SALTS_ETIMEDOUT;
    if (status == -EINTR) continue;
  }
}

static int uring_wake(cmeta_io_impl *base) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  const uint64_t signal = 1u;
  bool expected = false;
  ssize_t status;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (!atomic_compare_exchange_strong_explicit(&impl->wake_pending, &expected, true,
                                               memory_order_acq_rel, memory_order_acquire))
    return SALTS_OK;
  do {
    status = write(impl->wake_fd, &signal, sizeof(signal));
  } while (status < 0 && errno == EINTR);
  if (status == (ssize_t)sizeof(signal) || (status < 0 && errno == EAGAIN)) return SALTS_OK;
  atomic_store_explicit(&impl->wake_pending, false, memory_order_release);
  return status < 0 ? -errno : SALTS_EIO;
}

static int uring_close(cmeta_io_impl *base) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  if (!impl->admission_open) return SALTS_EALREADY;
  impl->admission_open = false;
  return SALTS_OK;
}

static void uring_unmap(cmeta_io_uring_impl *impl) {
  if (impl->sqes != NULL && impl->sqes != MAP_FAILED) (void)munmap(impl->sqes, impl->sqes_size);
  if (!impl->single_mmap && impl->cq_ring != NULL && impl->cq_ring != MAP_FAILED)
    (void)munmap(impl->cq_ring, impl->cq_ring_size);
  if (impl->sq_ring != NULL && impl->sq_ring != MAP_FAILED)
    (void)munmap(impl->sq_ring, impl->sq_ring_size);
}

static int uring_destroy(cmeta_io_impl *base) {
  cmeta_io_uring_impl *impl = (cmeta_io_uring_impl *)base;
  int escrow_status;
  if (impl->admission_open || impl->active_requests != 0u || impl->endpoint_count != 0u)
    return SALTS_EBUSY;
  escrow_status = native_io_accept_escrow_destroy(
      &impl->accept_escrow);
  if (escrow_status != SALTS_OK)
    return escrow_status;
  uring_unmap(impl);
  if (impl->ring_fd >= 0) (void)close(impl->ring_fd);
  if (impl->wake_fd >= 0) (void)close(impl->wake_fd);
  free(impl->free_requests);
  free(impl->free_endpoints);
  free(impl->terminal_requests);
  free(impl->accept_results);
  free(impl->requests);
  free(impl->endpoints);
  free(impl);
  return SALTS_OK;
}

static bool uring_get_stats(const cmeta_io_impl *base, native_io_backend_stats *out_stats) {
  const cmeta_io_uring_impl *impl = (const cmeta_io_uring_impl *)base;
  *out_stats = (native_io_backend_stats){impl->endpoint_capacity,
                                        impl->endpoint_count,
                                        impl->request_capacity,
                                        impl->active_requests,
                                        impl->submitted,
                                        impl->completed,
                                        impl->cancelled,
                                        impl->failed,
                                        impl->rejected_full,
                                        impl->native_submit_errors,
                                        impl->native_cancel_errors,
                                        impl->admission_open};
  return true;
}

static const cmeta_io_impl_ops uring_ops = {
    .attach_socket = uring_attach_socket,
    .release_socket = uring_release_socket,
    .submit = uring_submit,
    .cancel = uring_cancel,
    .observe = uring_observe,
    .wake = uring_wake,
    .close = uring_close,
    .destroy = uring_destroy,
    .get_stats = uring_get_stats,
    .attach_pipe = uring_attach_pipe,
    .release_pipe = uring_release_pipe,
    .prepare = uring_prepare,
    .flush = uring_flush,
    .submit_vector = uring_submit_vector,
    .supports_vector_write = uring_supports_vector_write,
    .submit_stream_accept = uring_submit_stream_accept,
    .take_stream_accept = uring_take_stream_accept};

static bool uring_mapped_extent(size_t offset, size_t count, size_t element_size, size_t *out) {
  if (element_size == 0u || count > (SIZE_MAX - offset) / element_size) return false;
  *out = offset + count * element_size;
  return true;
}

static bool uring_field_fits(size_t offset, size_t field_size, size_t mapped_size) {
  return offset <= mapped_size && field_size <= mapped_size - offset;
}

static void *uring_field(void *mapping, unsigned offset) {
  return (void *)((unsigned char *)mapping + (size_t)offset);
}

static int uring_map(cmeta_io_uring_impl *impl, const struct io_uring_params *params) {
  size_t shared_size;
  if (params->sq_entries == 0u || params->cq_entries == 0u ||
      !uring_mapped_extent(params->sq_off.array, params->sq_entries, sizeof(unsigned),
                           &impl->sq_ring_size) ||
      !uring_mapped_extent(params->cq_off.cqes, params->cq_entries, sizeof(struct io_uring_cqe),
                           &impl->cq_ring_size) ||
      !uring_mapped_extent(0u, params->sq_entries, sizeof(struct io_uring_sqe), &impl->sqes_size) ||
      !uring_field_fits(params->sq_off.head, sizeof(unsigned), impl->sq_ring_size) ||
      !uring_field_fits(params->sq_off.tail, sizeof(unsigned), impl->sq_ring_size) ||
      !uring_field_fits(params->sq_off.ring_mask, sizeof(unsigned), impl->sq_ring_size) ||
      !uring_field_fits(params->sq_off.ring_entries, sizeof(unsigned), impl->sq_ring_size) ||
      !uring_field_fits(params->sq_off.flags, sizeof(unsigned), impl->sq_ring_size) ||
      !uring_field_fits(params->sq_off.array, sizeof(unsigned), impl->sq_ring_size) ||
      !uring_field_fits(params->cq_off.head, sizeof(unsigned), impl->cq_ring_size) ||
      !uring_field_fits(params->cq_off.tail, sizeof(unsigned), impl->cq_ring_size) ||
      !uring_field_fits(params->cq_off.ring_mask, sizeof(unsigned), impl->cq_ring_size) ||
      !uring_field_fits(params->cq_off.cqes, sizeof(struct io_uring_cqe), impl->cq_ring_size))
    return SALTS_ERANGE;
  impl->single_mmap = (params->features & IORING_FEAT_SINGLE_MMAP) != 0u;
  shared_size = impl->sq_ring_size > impl->cq_ring_size ? impl->sq_ring_size : impl->cq_ring_size;
  if (impl->single_mmap) impl->sq_ring_size = impl->cq_ring_size = shared_size;
  impl->sq_ring = mmap(NULL, impl->sq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                       impl->ring_fd, IORING_OFF_SQ_RING);
  if (impl->sq_ring == MAP_FAILED) return -errno;
  if (impl->single_mmap) {
    impl->cq_ring = impl->sq_ring;
  } else {
    impl->cq_ring = mmap(NULL, impl->cq_ring_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, impl->ring_fd, IORING_OFF_CQ_RING);
    if (impl->cq_ring == MAP_FAILED) return -errno;
  }
  impl->sqes = mmap(NULL, impl->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                    impl->ring_fd, IORING_OFF_SQES);
  if (impl->sqes == MAP_FAILED) return -errno;
  impl->sq_head = uring_field(impl->sq_ring, params->sq_off.head);
  impl->sq_tail = uring_field(impl->sq_ring, params->sq_off.tail);
  impl->sq_mask = uring_field(impl->sq_ring, params->sq_off.ring_mask);
  impl->sq_entries = uring_field(impl->sq_ring, params->sq_off.ring_entries);
  impl->sq_flags = uring_field(impl->sq_ring, params->sq_off.flags);
  impl->sq_array = uring_field(impl->sq_ring, params->sq_off.array);
  impl->cq_head = uring_field(impl->cq_ring, params->cq_off.head);
  impl->cq_tail = uring_field(impl->cq_ring, params->cq_off.tail);
  impl->cq_mask = uring_field(impl->cq_ring, params->cq_off.ring_mask);
  impl->cqes = uring_field(impl->cq_ring, params->cq_off.cqes);
  return SALTS_OK;
}

static void uring_free_partial(cmeta_io_uring_impl *impl) {
  uring_unmap(impl);
  if (impl->ring_fd >= 0) (void)close(impl->ring_fd);
  if (impl->wake_fd >= 0) (void)close(impl->wake_fd);
  free(impl->free_requests);
  free(impl->free_endpoints);
  (void)native_io_accept_escrow_destroy(&impl->accept_escrow);
  free(impl->terminal_requests);
  free(impl->accept_results);
  free(impl->requests);
  free(impl->endpoints);
  free(impl);
}

int cmeta_io_uring_backend_init(native_io_backend *backend, const native_io_backend_config *config) {
  cmeta_io_uring_impl *impl;
  struct io_uring_params params;
  unsigned entries;
  int status;
  if (config->kind != NATIVE_IO_BACKEND_IO_URING) return SALTS_ENOTSUP;
  if (config->request_capacity > UINT32_MAX / 2u ||
      config->endpoint_capacity > SIZE_MAX / sizeof(cmeta_io_uring_endpoint) ||
      config->request_capacity > SIZE_MAX / sizeof(cmeta_io_uring_request_record) ||
      config->request_capacity > SIZE_MAX / sizeof(cmeta_io_uring_accept_result) ||
      config->endpoint_capacity > SIZE_MAX / sizeof(uint32_t) ||
      config->request_capacity > SIZE_MAX / sizeof(uint32_t))
    return SALTS_ERANGE;
  entries = (unsigned)config->request_capacity * 2u;
  impl = (cmeta_io_uring_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->ring_fd = -1;
  impl->wake_fd = -1;
  impl->staged_head = impl->staged_tail = SALTS_IO_URING_INDEX_NONE;
  impl->sq_ring = MAP_FAILED;
  impl->cq_ring = MAP_FAILED;
  impl->sqes = MAP_FAILED;
  impl->endpoints =
      (cmeta_io_uring_endpoint *)calloc(config->endpoint_capacity, sizeof(*impl->endpoints));
  impl->requests =
      (cmeta_io_uring_request_record *)calloc(config->request_capacity, sizeof(*impl->requests));
  impl->accept_results =
      (cmeta_io_uring_accept_result *)calloc(
          config->request_capacity, sizeof(*impl->accept_results));
  impl->free_endpoints = (uint32_t *)calloc(config->endpoint_capacity, sizeof(uint32_t));
  impl->free_requests = (uint32_t *)calloc(config->request_capacity, sizeof(uint32_t));
  impl->terminal_requests = (uint32_t *)calloc(config->request_capacity, sizeof(uint32_t));
  if (impl->endpoints == NULL || impl->requests == NULL ||
      impl->accept_results == NULL || impl->free_endpoints == NULL ||
      impl->free_requests == NULL || impl->terminal_requests == NULL) {
    uring_free_partial(impl);
    return SALTS_ENOMEM;
  }
  impl->base.ops = &uring_ops;
  impl->base.kind = config->kind;
  impl->endpoint_capacity = config->endpoint_capacity;
  impl->request_capacity = config->request_capacity;
  impl->completion_batch_capacity = config->completion_batch_capacity;
  impl->free_endpoint_count = config->endpoint_capacity;
  impl->free_request_count = config->request_capacity;
  impl->admission_open = true;
  status = native_io_accept_escrow_init(
      &impl->accept_escrow, config->request_capacity,
      uring_retire_accept_result, impl);
  if (status != SALTS_OK) {
    uring_free_partial(impl);
    return status;
  }
  atomic_init(&impl->wake_pending, false);
  for (size_t index = 0u; index < config->endpoint_capacity; ++index) {
    impl->free_endpoints[index] = (uint32_t)(config->endpoint_capacity - index - 1u);
    impl->endpoints[index].fd = -1;
  }
  for (size_t index = 0u; index < config->request_capacity; ++index) {
    impl->free_requests[index] =
        (uint32_t)(config->request_capacity - index - 1u);
    impl->accept_results[index].child_fd = -1;
  }
  impl->wake_fd = eventfd(0u, EFD_CLOEXEC | EFD_NONBLOCK);
  if (impl->wake_fd < 0) {
    status = -errno;
    uring_free_partial(impl);
    return status;
  }
  memset(&params, 0, sizeof(params));
#if SALTS_IO_URING_HAS_SINGLE_ISSUER
  params.flags = IORING_SETUP_SINGLE_ISSUER;
#endif
#if SALTS_IO_URING_HAS_DEFER_TASKRUN
  params.flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
#if SALTS_IO_URING_HAS_TASKRUN_FLAG
  params.flags |= IORING_SETUP_TASKRUN_FLAG;
#endif
  impl->ring_fd = (int)syscall(__NR_io_uring_setup, entries, &params);
#if SALTS_IO_URING_HAS_TASKRUN_FLAG
  if (impl->ring_fd < 0 && errno == EINVAL) {
    /* TASKRUN_FLAG is an optional userspace progress hint. Keep the accepted
     * SINGLE_ISSUER | DEFER_TASKRUN owner policy when older kernels reject it. */
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    impl->ring_fd = (int)syscall(__NR_io_uring_setup, entries, &params);
  }
#endif
#if SALTS_IO_URING_HAS_DEFER_TASKRUN
  if (impl->ring_fd < 0 && errno == EINVAL) {
    /* Linux 6.0 supports SINGLE_ISSUER before DEFER_TASKRUN (6.1). */
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER;
    impl->ring_fd = (int)syscall(__NR_io_uring_setup, entries, &params);
  }
  if (impl->ring_fd >= 0 && (params.flags & IORING_SETUP_DEFER_TASKRUN) != 0u &&
      (params.features & IORING_FEAT_EXT_ARG) == 0u) {
    /* DEFER_TASKRUN needs an owner-driven GETEVENTS path for finite waits and
     * nonblocking progress. Recreate without defer if EXT_ARG wait support is
     * unexpectedly absent. */
    (void)close(impl->ring_fd);
    impl->ring_fd = -1;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER;
    impl->ring_fd = (int)syscall(__NR_io_uring_setup, entries, &params);
  }
#endif
#if SALTS_IO_URING_HAS_SINGLE_ISSUER
  if (impl->ring_fd < 0 && errno == EINVAL && params.flags != 0u) {
    /* Older kernels reject unknown setup flags with EINVAL. Retry the same
     * explicitly selected io_uring backend conservatively; never fall back
     * to a readiness backend. */
    memset(&params, 0, sizeof(params));
    impl->ring_fd = (int)syscall(__NR_io_uring_setup, entries, &params);
  }
#endif
  if (impl->ring_fd < 0) {
    status = -errno;
    uring_free_partial(impl);
    return status;
  }
  status = uring_map(impl, &params);
  if (status != SALTS_OK) {
    uring_free_partial(impl);
    return status;
  }
#if SALTS_IO_URING_HAS_EXT_ARG_WAIT
  impl->ring_native_wait = (params.features & IORING_FEAT_EXT_ARG) != 0u;
#else
  impl->ring_native_wait = false;
#endif
#if SALTS_IO_URING_HAS_DEFER_TASKRUN
  impl->defer_taskrun = (params.flags & IORING_SETUP_DEFER_TASKRUN) != 0u;
#else
  impl->defer_taskrun = false;
#endif
#if SALTS_IO_URING_HAS_TASKRUN_FLAG
  impl->taskrun_flag = (params.flags & IORING_SETUP_TASKRUN_FLAG) != 0u;
#else
  impl->taskrun_flag = false;
#endif
  if (impl->ring_native_wait) {
    status = uring_arm_wake_poll(impl);
    if (status != SALTS_OK) {
      uring_free_partial(impl);
      return status;
    }
  }
  backend->impl = impl;
  return SALTS_OK;
}
