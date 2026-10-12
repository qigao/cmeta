#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#include "native_io_readiness.h"
#include "native_io_accept_escrow.h"

#include <salts/clock.h>
#include <salts/error_codes.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

enum { SALTS_IO_INDEX_NONE = UINT32_MAX };

typedef enum cmeta_io_readiness_phase {
  SALTS_IO_READINESS_FREE = 0,
  SALTS_IO_READINESS_PENDING,
  SALTS_IO_READINESS_TERMINAL
} cmeta_io_readiness_phase;

typedef struct cmeta_io_readiness_lane {
  uint32_t head;
  uint32_t tail;
} cmeta_io_readiness_lane;

typedef struct cmeta_io_readiness_endpoint {
  int fd;
  uint32_t generation;
  size_t active_requests;
  size_t retained_accepts;
  uint32_t interests;
  cmeta_io_readiness_lane read_lane;
  cmeta_io_readiness_lane write_lane;
  cmeta_io_resource_kind resource_kind;
  bool connected;
  bool connect_active;
  bool active;
} cmeta_io_readiness_endpoint;

typedef struct cmeta_io_readiness_request {
  cmeta_io_readiness_phase phase;
  native_io_request request;
  native_io_endpoint endpoint;
  native_io_operation operation;
  struct iovec vectors[NATIVE_IO_VECTOR_MAX];
  size_t vector_count;
  native_io_completion completion;
  uint32_t previous;
  uint32_t next;
  bool vector_write;
  bool write_lane;
  bool connect_started;
  bool stream_accept;
  int accepted_fd;
} cmeta_io_readiness_request;

typedef struct cmeta_io_readiness_accept_result {
  native_io_request request;
  native_io_endpoint listener;
  int child_fd;
  bool live;
} cmeta_io_readiness_accept_result;

typedef struct cmeta_io_readiness_impl {
  cmeta_io_impl base;
  const cmeta_io_readiness_driver_ops *driver_ops;
  void *driver_state;
  cmeta_io_readiness_endpoint *endpoints;
  cmeta_io_readiness_request *requests;
  cmeta_io_readiness_accept_result *accept_results;
  native_io_accept_escrow accept_escrow;
  cmeta_io_ready_event *ready_events;
  uint32_t *free_endpoints;
  uint32_t *free_requests;
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
  uint64_t completed;
  uint64_t cancelled;
  uint64_t failed;
  uint64_t rejected_full;
  uint64_t native_submit_errors;
  uint64_t native_cancel_errors;
  bool admission_open;
  atomic_bool wake_pending;
} cmeta_io_readiness_impl;

typedef struct cmeta_io_sigpipe_guard {
  sigset_t blocked;
  sigset_t previous;
  bool active;
  bool had_pending;
} cmeta_io_sigpipe_guard;

#if defined(SALTS_NATIVE_IO_TEST_DISPATCH)
static cmeta_io_readiness_test_dispatch_hook g_dispatch_test_hook;
static void *g_dispatch_test_user;

void cmeta_io_readiness_test_set_dispatch_hook(
    cmeta_io_readiness_test_dispatch_hook hook, void *user) {
  /* Registration and invocation are Owner-only; tests register before
   * observing and clear the hook before backend destruction. */
  g_dispatch_test_hook = hook;
  g_dispatch_test_user = user;
}
#endif

static void readiness_counter_increment(uint64_t *counter) {
  if (*counter != UINT64_MAX) ++*counter;
}

static uint32_t readiness_next_generation(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static cmeta_io_readiness_endpoint *readiness_endpoint(
    cmeta_io_readiness_impl *impl, native_io_endpoint endpoint);

static cmeta_io_readiness_accept_result *readiness_accept_result_from_token(
    cmeta_io_readiness_impl *impl, uintptr_t token) {
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

static int readiness_retire_accept_result(
    void *context, uintptr_t transport) {
  cmeta_io_readiness_impl *impl =
      (cmeta_io_readiness_impl *)context;
  cmeta_io_readiness_accept_result *result =
      readiness_accept_result_from_token(impl, transport);
  cmeta_io_readiness_endpoint *listener;
  int status = SALTS_OK;

  if (result == NULL || !result->live)
    return SALTS_EINVAL;
  listener = readiness_endpoint(impl, result->listener);
  if (result->child_fd >= 0 && close(result->child_fd) != 0)
    status = -errno;
  if (listener != NULL && listener->retained_accepts != 0u)
    --listener->retained_accepts;
  memset(result, 0, sizeof(*result));
  result->child_fd = -1;
  return status;
}

static cmeta_io_readiness_accept_result *
readiness_accept_result_reserve(
    cmeta_io_readiness_impl *impl,
    native_io_request request,
    native_io_endpoint listener,
    int child_fd) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    cmeta_io_readiness_accept_result *result =
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

static int readiness_publish_accept_result(
    cmeta_io_readiness_impl *impl,
    cmeta_io_readiness_request *request) {
  cmeta_io_readiness_accept_result *result;
  cmeta_io_readiness_endpoint *listener;
  int status;

  if (request->accepted_fd < 0)
    return SALTS_EPROTO;
  result = readiness_accept_result_reserve(
      impl, request->request, request->endpoint,
      request->accepted_fd);
  if (result == NULL)
    return SALTS_ENOBUFS;

  status = native_io_accept_escrow_publish(
      &impl->accept_escrow, request->request,
      (uintptr_t)result);
  if (status != SALTS_OK) {
    memset(result, 0, sizeof(*result));
    result->child_fd = -1;
    return status;
  }

  listener = readiness_endpoint(impl, request->endpoint);
  if (listener == NULL) {
    (void)native_io_accept_escrow_discard(
        &impl->accept_escrow, request->request);
    return SALTS_ENOENT;
  }
  ++listener->retained_accepts;
  request->accepted_fd = -1;
  return SALTS_OK;
}

static int readiness_discard_listener_accepts(
    cmeta_io_readiness_impl *impl,
    native_io_endpoint listener_handle) {
  int first_status = SALTS_OK;
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    cmeta_io_readiness_accept_result *result =
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

static uint64_t readiness_endpoint_token(uint32_t index, uint32_t generation) {
  return ((uint64_t)generation << 32u) | (uint64_t)(index + 1u);
}

static cmeta_io_readiness_endpoint *readiness_endpoint(cmeta_io_readiness_impl *impl,
                                                       native_io_endpoint endpoint) {
  cmeta_io_readiness_endpoint *record;
  if (!native_io_endpoint_valid(endpoint) || endpoint.slot > impl->endpoint_capacity) return NULL;
  record = &impl->endpoints[endpoint.slot - 1u];
  return record->active && record->generation == endpoint.generation ? record : NULL;
}

static cmeta_io_readiness_request *readiness_request(cmeta_io_readiness_impl *impl,
                                                     native_io_request request) {
  cmeta_io_readiness_request *record;
  if (!native_io_request_valid(request) || request.slot > impl->request_capacity) return NULL;
  record = &impl->requests[request.slot - 1u];
  return record->phase != SALTS_IO_READINESS_FREE &&
                 record->request.generation == request.generation
             ? record
             : NULL;
}

static int readiness_sigpipe_begin(cmeta_io_sigpipe_guard *guard) {
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

static void readiness_sigpipe_end(cmeta_io_sigpipe_guard *guard) {
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

static bool readiness_is_write(native_io_operation_kind kind) {
  return kind == NATIVE_IO_OPERATION_STREAM_SEND || kind == NATIVE_IO_OPERATION_UDP_SEND_TO ||
         kind == NATIVE_IO_OPERATION_PIPE_WRITE || kind == NATIVE_IO_OPERATION_STREAM_CONNECT;
}

static cmeta_io_readiness_lane *readiness_lane(cmeta_io_readiness_endpoint *endpoint,
                                               bool write_lane) {
  return write_lane ? &endpoint->write_lane : &endpoint->read_lane;
}

static uint32_t readiness_derived_interests(const cmeta_io_readiness_endpoint *endpoint) {
  uint32_t interests = 0u;
  if (endpoint->read_lane.head != SALTS_IO_INDEX_NONE) interests |= SALTS_IO_READY_READ;
  if (endpoint->write_lane.head != SALTS_IO_INDEX_NONE) interests |= SALTS_IO_READY_WRITE;
  return interests;
}

static int readiness_update_interests(cmeta_io_readiness_impl *impl,
                                      native_io_endpoint endpoint_handle,
                                      cmeta_io_readiness_endpoint *endpoint) {
  const uint32_t requested = readiness_derived_interests(endpoint);
  const uint32_t next = impl->driver_ops->persistent_interests
                            ? endpoint->interests | requested : requested;
  int status;
  if (next == endpoint->interests) return SALTS_OK;
  status = impl->driver_ops->update(
      impl->driver_state, endpoint->fd,
      readiness_endpoint_token(endpoint_handle.slot - 1u, endpoint->generation),
      endpoint->interests, next);
  if (status == SALTS_OK) endpoint->interests = next;
  return status;
}

static void readiness_lane_push(cmeta_io_readiness_impl *impl,
                                cmeta_io_readiness_endpoint *endpoint, uint32_t index) {
  cmeta_io_readiness_request *request = &impl->requests[index];
  cmeta_io_readiness_lane *lane = readiness_lane(endpoint, request->write_lane);
  request->previous = lane->tail;
  request->next = SALTS_IO_INDEX_NONE;
  if (lane->tail == SALTS_IO_INDEX_NONE) lane->head = index;
  else impl->requests[lane->tail].next = index;
  lane->tail = index;
}

static void readiness_lane_remove(cmeta_io_readiness_impl *impl,
                                  cmeta_io_readiness_endpoint *endpoint, uint32_t index) {
  cmeta_io_readiness_request *request = &impl->requests[index];
  cmeta_io_readiness_lane *lane = readiness_lane(endpoint, request->write_lane);
  if (request->previous == SALTS_IO_INDEX_NONE) lane->head = request->next;
  else impl->requests[request->previous].next = request->next;
  if (request->next == SALTS_IO_INDEX_NONE) lane->tail = request->previous;
  else impl->requests[request->next].previous = request->previous;
  request->previous = SALTS_IO_INDEX_NONE;
  request->next = SALTS_IO_INDEX_NONE;
}

static void readiness_release_request(cmeta_io_readiness_impl *impl,
                                      cmeta_io_readiness_request *request, uint32_t index) {
  cmeta_io_readiness_endpoint *endpoint = readiness_endpoint(impl, request->endpoint);
  if (endpoint != NULL && endpoint->active_requests != 0u) --endpoint->active_requests;
  if (request->accepted_fd >= 0) {
    (void)close(request->accepted_fd);
    request->accepted_fd = -1;
  }
  request->phase = SALTS_IO_READINESS_FREE;
  request->operation = (native_io_operation){0};
  request->vector_count = 0u;
  request->vector_write = false;
  request->stream_accept = false;
  request->completion = (native_io_completion){0};
  impl->free_requests[impl->free_request_count++] = index;
  --impl->active_requests;
}

static void readiness_publish_terminal(cmeta_io_readiness_impl *impl,
                                       cmeta_io_readiness_request *request, uint32_t index,
                                       native_io_completion_kind kind, size_t bytes, int status,
                                       uint32_t native_status, size_t address_length) {
  const size_t tail = (impl->terminal_head + impl->terminal_count) % impl->request_capacity;
  if (request->operation.kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    cmeta_io_readiness_endpoint *endpoint = readiness_endpoint(impl, request->endpoint);
    if (endpoint != NULL) {
      endpoint->connect_active = false;
      endpoint->connected = kind == NATIVE_IO_COMPLETION_OK;
    }
  }
  request->phase = SALTS_IO_READINESS_TERMINAL;
  request->completion = (native_io_completion){request->request,
                                              request->endpoint,
                                              kind,
                                              bytes,
                                              status,
                                              native_status,
                                              request->operation.user_data,
                                              address_length};
  impl->terminal_requests[tail] = index;
  ++impl->terminal_count;
  readiness_counter_increment(&impl->completed);
  if (kind == NATIVE_IO_COMPLETION_CANCELLED) readiness_counter_increment(&impl->cancelled);
  if (kind == NATIVE_IO_COMPLETION_FAILED) readiness_counter_increment(&impl->failed);
}

static int readiness_try_socket(cmeta_io_readiness_endpoint *endpoint,
                                cmeta_io_readiness_request *request, size_t *out_bytes,
                                size_t *out_address_length) {
  cmeta_io_sigpipe_guard guard;
  ssize_t result;
  int saved_error = 0;
  int flags = MSG_DONTWAIT;
  int guard_status = SALTS_OK;
  socklen_t address_length = (socklen_t)request->operation.address_capacity;
  if (request->operation.kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    int connect_error = 0;
    socklen_t connect_error_length = (socklen_t)sizeof(connect_error);
    if (!request->connect_started) {
      do {
        result = connect(endpoint->fd, (const struct sockaddr *)request->operation.address,
                         (socklen_t)request->operation.address_length);
      } while (result < 0 && errno == EINTR);
      if (result == 0 || (result < 0 && errno == EISCONN)) {
        *out_bytes = 0u;
        *out_address_length = 0u;
        return SALTS_OK;
      }
      if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN && errno != EWOULDBLOCK)
        return -errno;
      request->connect_started = true;
      return -EAGAIN;
    }
    if (getsockopt(endpoint->fd, SOL_SOCKET, SO_ERROR, &connect_error, &connect_error_length) != 0)
      return -errno;
    if (connect_error == EINPROGRESS || connect_error == EALREADY || connect_error == EAGAIN ||
        connect_error == EWOULDBLOCK)
      return -EAGAIN;
    if (connect_error != 0) return -connect_error;
    *out_bytes = 0u;
    *out_address_length = 0u;
    return SALTS_OK;
  }
#if defined(MSG_NOSIGNAL)
  if (request->write_lane) flags |= MSG_NOSIGNAL;
#endif
#if !defined(MSG_NOSIGNAL)
  if (request->write_lane) {
    guard_status = readiness_sigpipe_begin(&guard);
    if (guard_status != SALTS_OK) return guard_status;
  }
#else
  (void)guard;
  (void)guard_status;
#endif
  do {
    if (request->vector_write) {
      struct msghdr message = {0};
      message.msg_iov = request->vectors;
      message.msg_iovlen = request->vector_count;
      result = sendmsg(endpoint->fd, &message, flags);
    } else if (request->operation.kind == NATIVE_IO_OPERATION_STREAM_RECV)
      result = recv(endpoint->fd, request->operation.buffer, request->operation.length, flags);
    else if (request->operation.kind == NATIVE_IO_OPERATION_STREAM_SEND)
      result = send(endpoint->fd, request->operation.buffer, request->operation.length, flags);
    else if (request->operation.kind == NATIVE_IO_OPERATION_UDP_RECV_FROM) {
      struct iovec vector = {request->operation.buffer, request->operation.length};
      struct msghdr message = {0};
      message.msg_name = request->operation.address;
      message.msg_namelen = address_length;
      message.msg_iov = &vector;
      message.msg_iovlen = 1u;
      result = recvmsg(endpoint->fd, &message, flags);
      if (result >= 0 && (message.msg_flags & MSG_TRUNC) != 0) return -EMSGSIZE;
      address_length = message.msg_namelen;
    } else if (request->operation.kind == NATIVE_IO_OPERATION_UDP_SEND_TO &&
             request->operation.address != NULL)
      result = sendto(endpoint->fd, request->operation.buffer, request->operation.length, flags,
                      (const struct sockaddr *)request->operation.address,
                      (socklen_t)request->operation.address_length);
    else
      result = send(endpoint->fd, request->operation.buffer, request->operation.length, flags);
  } while (result < 0 && errno == EINTR);
  if (result < 0) saved_error = errno;
#if !defined(MSG_NOSIGNAL)
  if (request->write_lane) readiness_sigpipe_end(&guard);
#endif
  if (result < 0) return -saved_error;
  *out_bytes = (size_t)result;
  *out_address_length =
      request->operation.kind == NATIVE_IO_OPERATION_UDP_RECV_FROM &&
              request->operation.address != NULL
          ? (size_t)address_length
          : 0u;
  return SALTS_OK;
}

static int readiness_try_pipe(cmeta_io_readiness_endpoint *endpoint,
                              cmeta_io_readiness_request *request, size_t *out_bytes) {
  cmeta_io_sigpipe_guard guard;
  ssize_t result;
  int saved_error = 0;
  int guard_status = SALTS_OK;
  if (request->write_lane) {
    guard_status = readiness_sigpipe_begin(&guard);
    if (guard_status != SALTS_OK) return guard_status;
  }
  do {
    if (request->vector_write)
      result = writev(endpoint->fd, request->vectors, (int)request->vector_count);
    else
      result = request->write_lane
                   ? write(endpoint->fd, request->operation.buffer, request->operation.length)
                   : read(endpoint->fd, request->operation.buffer, request->operation.length);
  } while (result < 0 && errno == EINTR);
  if (result < 0) saved_error = errno;
  if (request->write_lane) readiness_sigpipe_end(&guard);
  if (result < 0) return -saved_error;
  *out_bytes = (size_t)result;
  return SALTS_OK;
}

static int readiness_try_operation(cmeta_io_readiness_endpoint *endpoint,
                                   cmeta_io_readiness_request *request, size_t *out_bytes,
                                   size_t *out_address_length) {
  if (request->stream_accept) {
    int child_fd;
    int flags;
    do {
      child_fd = accept(endpoint->fd, NULL, NULL);
    } while (child_fd < 0 && errno == EINTR);
    if (child_fd < 0)
      return -errno;
    flags = fcntl(child_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(child_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      const int saved = errno;
      (void)close(child_fd);
      return -saved;
    }
    request->accepted_fd = child_fd;
    *out_bytes = 0u;
    *out_address_length = 0u;
    return SALTS_OK;
  }
  if (native_io_resource_kind_is_socket(endpoint->resource_kind))
    return readiness_try_socket(endpoint, request, out_bytes, out_address_length);
  if (endpoint->resource_kind == SALTS_IO_RESOURCE_BYTE_PIPE) {
    *out_address_length = 0u;
    return readiness_try_pipe(endpoint, request, out_bytes);
  }
  return SALTS_EINVAL;
}

static bool readiness_would_block(int status) {
  return status == -EAGAIN || status == -EWOULDBLOCK;
}

static void readiness_finish_attempt(cmeta_io_readiness_impl *impl,
                                     cmeta_io_readiness_request *request, uint32_t index,
                                     int status, size_t bytes, size_t address_length) {
  if (status == SALTS_OK && request->stream_accept) {
    status = readiness_publish_accept_result(impl, request);
    if (status != SALTS_OK) {
      readiness_publish_terminal(
          impl, request, index, NATIVE_IO_COMPLETION_FAILED,
          0u, status, 0u, 0u);
      return;
    }
  }
  if (status < 0) {
    readiness_publish_terminal(impl, request, index, NATIVE_IO_COMPLETION_FAILED, 0u, status,
                               (uint32_t)(-status), 0u);
  } else if ((request->operation.kind == NATIVE_IO_OPERATION_STREAM_RECV ||
              request->operation.kind == NATIVE_IO_OPERATION_PIPE_READ) &&
             bytes == 0u) {
    readiness_publish_terminal(impl, request, index, NATIVE_IO_COMPLETION_EOF, 0u, SALTS_EOF, 0u,
                               0u);
  } else {
    readiness_publish_terminal(impl, request, index, NATIVE_IO_COMPLETION_OK, bytes, SALTS_OK, 0u,
                               address_length);
  }
}

static int readiness_attach_endpoint(cmeta_io_readiness_impl *impl, int fd,
                                     cmeta_io_resource_kind resource_kind,
                                     native_io_endpoint *out_endpoint) {
  cmeta_io_readiness_endpoint *endpoint;
  uint32_t index;
  size_t cursor;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  for (cursor = 0u; cursor < impl->endpoint_capacity; ++cursor)
    if (impl->endpoints[cursor].active && impl->endpoints[cursor].fd == fd)
      return SALTS_EALREADY;
  if (impl->free_endpoint_count == 0u) return SALTS_ENOBUFS;
  index = impl->free_endpoints[--impl->free_endpoint_count];
  endpoint = &impl->endpoints[index];
  endpoint->fd = fd;
  endpoint->generation = readiness_next_generation(endpoint->generation);
  endpoint->active_requests = 0u;
  endpoint->retained_accepts = 0u;
  endpoint->interests = 0u;
  endpoint->read_lane = (cmeta_io_readiness_lane){SALTS_IO_INDEX_NONE, SALTS_IO_INDEX_NONE};
  endpoint->write_lane = (cmeta_io_readiness_lane){SALTS_IO_INDEX_NONE, SALTS_IO_INDEX_NONE};
  endpoint->resource_kind = resource_kind;
  endpoint->connected = false;
  endpoint->connect_active = false;
  endpoint->active = true;
  ++impl->endpoint_count;
  *out_endpoint = (native_io_endpoint){index + 1u, endpoint->generation};
  return SALTS_OK;
}

static int readiness_attach_socket(cmeta_io_impl *base, uintptr_t native_socket,
                                   native_io_endpoint *out_endpoint) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  cmeta_io_resource_kind resource_kind;
  int socket_type = 0;
  struct sockaddr_storage peer_address;
  socklen_t peer_address_length = (socklen_t)sizeof(peer_address);
  bool connected = false;
  int status;
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
  status = readiness_attach_endpoint(impl, (int)native_socket, resource_kind, out_endpoint);
  if (status == SALTS_OK)
    impl->endpoints[out_endpoint->slot - 1u].connected = connected;
  return status;
}

static int readiness_attach_pipe(cmeta_io_impl *base, uintptr_t native_handle, uint32_t flags,
                                 native_io_endpoint *out_endpoint) {
  struct stat descriptor_stat;
  int descriptor_flags;
  int fd;
  (void)flags;
  if (native_handle > (uintptr_t)INT_MAX) return SALTS_EINVAL;
  fd = (int)native_handle;
  descriptor_flags = fcntl(fd, F_GETFL, 0);
  if (descriptor_flags < 0) return -errno;
  if ((descriptor_flags & O_NONBLOCK) == 0) return SALTS_EINVAL;
  if (fstat(fd, &descriptor_stat) != 0) return -errno;
  if (!S_ISFIFO(descriptor_stat.st_mode)) return SALTS_EINVAL;
  return readiness_attach_endpoint((cmeta_io_readiness_impl *)base, fd,
                                   SALTS_IO_RESOURCE_BYTE_PIPE, out_endpoint);
}

static int readiness_release_endpoint(cmeta_io_readiness_impl *impl,
                                      native_io_endpoint endpoint_handle,
                                      bool socket_endpoint) {
  cmeta_io_readiness_endpoint *endpoint = readiness_endpoint(impl, endpoint_handle);
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
    retire_status = readiness_discard_listener_accepts(
        impl, endpoint_handle);
    if (endpoint->retained_accepts != 0u)
      return retire_status == SALTS_OK
                 ? SALTS_EPROTO
                 : retire_status;
    if (retire_status != SALTS_OK)
      return retire_status;
  }
  if (endpoint->interests != 0u) {
    if (!impl->driver_ops->persistent_interests) return SALTS_EBUSY;
    /* Socket release follows caller close. Watch lifetime belongs to the kernel;
     * another owner may already have reused the fd, so only metadata is ours now.
     * A drained pipe can still be open, so its watch must be removed explicitly. */
    if (!socket_endpoint) {
      const int status = impl->driver_ops->update(impl->driver_state, endpoint->fd,
          readiness_endpoint_token(endpoint_handle.slot - 1u, endpoint->generation),
          endpoint->interests, 0u);
      if (status != SALTS_OK) return status;
    }
    endpoint->interests = 0u;
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

static int readiness_release_socket(cmeta_io_impl *base, native_io_endpoint endpoint_handle) {
  return readiness_release_endpoint((cmeta_io_readiness_impl *)base, endpoint_handle, true);
}

static int readiness_release_pipe(cmeta_io_impl *base, native_io_endpoint endpoint_handle) {
  return readiness_release_endpoint((cmeta_io_readiness_impl *)base, endpoint_handle, false);
}

static int readiness_submit(cmeta_io_impl *base, const native_io_operation *operation,
                            native_io_request *out_request) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  cmeta_io_readiness_endpoint *endpoint;
  cmeta_io_readiness_request *request;
  uint32_t index;
  size_t bytes = 0u;
  size_t address_length = 0u;
  int status;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  endpoint = readiness_endpoint(impl, operation->endpoint);
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
  if (operation->kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    const int descriptor_flags = fcntl(endpoint->fd, F_GETFL, 0);
    if (descriptor_flags < 0) return -errno;
    if ((descriptor_flags & O_NONBLOCK) == 0) return SALTS_EINVAL;
  }
  if (impl->free_request_count == 0u) {
    readiness_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }
  index = impl->free_requests[--impl->free_request_count];
  request = &impl->requests[index];
  request->phase = SALTS_IO_READINESS_PENDING;
  request->request =
      (native_io_request){index + 1u, readiness_next_generation(request->request.generation)};
  request->endpoint = operation->endpoint;
  request->operation = *operation;
  request->vector_count = 0u;
  request->vector_write = false;
  request->previous = SALTS_IO_INDEX_NONE;
  request->next = SALTS_IO_INDEX_NONE;
  request->write_lane = readiness_is_write(operation->kind);
  request->connect_started = false;
  ++endpoint->active_requests;
  ++impl->active_requests;
  if (operation->kind == NATIVE_IO_OPERATION_STREAM_CONNECT) endpoint->connect_active = true;
  /* A newer operation must not consume bytes ahead of a queued lane head. */
  if (readiness_lane(endpoint, request->write_lane)->head != SALTS_IO_INDEX_NONE) {
    readiness_lane_push(impl, endpoint, index);
    readiness_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }
  status = readiness_try_operation(endpoint, request, &bytes, &address_length);
  if (readiness_would_block(status)) {
    readiness_lane_push(impl, endpoint, index);
    status = readiness_update_interests(impl, operation->endpoint, endpoint);
    if (status != SALTS_OK) {
      readiness_lane_remove(impl, endpoint, index);
      endpoint->connect_active = false;
      readiness_release_request(impl, request, index);
      readiness_counter_increment(&impl->native_submit_errors);
      return status;
    }
  } else if (status < 0 && operation->kind != NATIVE_IO_OPERATION_STREAM_CONNECT) {
    readiness_release_request(impl, request, index);
    readiness_counter_increment(&impl->native_submit_errors);
    return status;
  } else {
    readiness_finish_attempt(impl, request, index, status, bytes, address_length);
  }
  readiness_counter_increment(&impl->submitted);
  *out_request = request->request;
  return SALTS_OK;
}

static int readiness_submit_vector(cmeta_io_impl *base,
                                   const native_io_vector_operation *operation,
                                   native_io_request *out_request) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  cmeta_io_readiness_endpoint *endpoint;
  cmeta_io_readiness_request *request;
  uint32_t index;
  size_t bytes = 0u;
  size_t address_length = 0u;
  int status;

  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  endpoint = readiness_endpoint(impl, operation->endpoint);
  if (endpoint == NULL) return SALTS_ENOENT;
  if (operation->kind == NATIVE_IO_OPERATION_STREAM_SEND) {
    if (endpoint->resource_kind != SALTS_IO_RESOURCE_STREAM_SOCKET) return SALTS_EINVAL;
    if (endpoint->connect_active) return SALTS_EBUSY;
    if (!endpoint->connected) return SALTS_EINVAL;
  } else if (operation->kind == NATIVE_IO_OPERATION_PIPE_WRITE) {
    if (endpoint->resource_kind != SALTS_IO_RESOURCE_BYTE_PIPE) return SALTS_EINVAL;
  } else {
    return SALTS_EINVAL;
  }
  if (impl->free_request_count == 0u) {
    readiness_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }

  index = impl->free_requests[--impl->free_request_count];
  request = &impl->requests[index];
  request->phase = SALTS_IO_READINESS_PENDING;
  request->request =
      (native_io_request){index + 1u, readiness_next_generation(request->request.generation)};
  request->endpoint = operation->endpoint;
  request->operation = (native_io_operation){
      .kind = operation->kind, .endpoint = operation->endpoint, .user_data = operation->user_data};
  request->vector_count = operation->span_count;
  for (size_t cursor = 0u; cursor < operation->span_count; ++cursor) {
    request->vectors[cursor].iov_base = operation->spans[cursor].data;
    request->vectors[cursor].iov_len = operation->spans[cursor].length;
  }
  request->vector_write = true;
  request->previous = SALTS_IO_INDEX_NONE;
  request->next = SALTS_IO_INDEX_NONE;
  request->write_lane = true;
  request->connect_started = false;
  ++endpoint->active_requests;
  ++impl->active_requests;

  if (readiness_lane(endpoint, true)->head != SALTS_IO_INDEX_NONE) {
    readiness_lane_push(impl, endpoint, index);
    readiness_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }
  status = readiness_try_operation(endpoint, request, &bytes, &address_length);
  if (readiness_would_block(status)) {
    readiness_lane_push(impl, endpoint, index);
    status = readiness_update_interests(impl, operation->endpoint, endpoint);
    if (status != SALTS_OK) {
      readiness_lane_remove(impl, endpoint, index);
      readiness_release_request(impl, request, index);
      readiness_counter_increment(&impl->native_submit_errors);
      return status;
    }
  } else if (status < 0) {
    readiness_release_request(impl, request, index);
    readiness_counter_increment(&impl->native_submit_errors);
    return status;
  } else {
    readiness_finish_attempt(impl, request, index, status, bytes, address_length);
  }
  readiness_counter_increment(&impl->submitted);
  *out_request = request->request;
  return SALTS_OK;
}

static int readiness_submit_stream_accept(
    cmeta_io_impl *base,
    native_io_endpoint listener_handle,
    native_io_request *out_request) {
  cmeta_io_readiness_impl *impl =
      (cmeta_io_readiness_impl *)base;
  cmeta_io_readiness_endpoint *listener;
  cmeta_io_readiness_request *request;
  uint32_t index;
  size_t bytes = 0u;
  size_t address_length = 0u;
  int descriptor_flags;
  int status;

  if (out_request != NULL)
    *out_request = (native_io_request){0};
  if (!impl->admission_open || out_request == NULL)
    return !impl->admission_open
               ? SALTS_ESHUTDOWN
               : SALTS_EINVAL;
  listener = readiness_endpoint(impl, listener_handle);
  if (listener == NULL)
    return SALTS_ENOENT;
  if (listener->resource_kind != SALTS_IO_RESOURCE_STREAM_SOCKET ||
      listener->connected || listener->connect_active)
    return SALTS_EINVAL;

  /*
   * Darwin does not provide SO_ACCEPTCONN. The immediate nonblocking accept
   * below is the portable listener validation: no peer yields would-block;
   * a non-listening stream socket fails the operation.
   */
  descriptor_flags = fcntl(listener->fd, F_GETFL, 0);
  if (descriptor_flags < 0)
    return -errno;
  if ((descriptor_flags & O_NONBLOCK) == 0)
    return SALTS_EINVAL;
  if (listener->active_requests != 0u)
    return SALTS_EBUSY;
  if (impl->free_request_count == 0u) {
    readiness_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }

  index = impl->free_requests[--impl->free_request_count];
  request = &impl->requests[index];
  request->phase = SALTS_IO_READINESS_PENDING;
  request->request = (native_io_request){
      index + 1u,
      readiness_next_generation(request->request.generation)};
  request->endpoint = listener_handle;
  request->operation = (native_io_operation){0};
  request->vector_count = 0u;
  request->vector_write = false;
  request->previous = SALTS_IO_INDEX_NONE;
  request->next = SALTS_IO_INDEX_NONE;
  request->write_lane = false;
  request->connect_started = false;
  request->stream_accept = true;
  request->accepted_fd = -1;
  ++listener->active_requests;
  ++impl->active_requests;

  status = readiness_try_operation(
      listener, request, &bytes, &address_length);
  if (readiness_would_block(status)) {
    readiness_lane_push(impl, listener, index);
    status = readiness_update_interests(
        impl, listener_handle, listener);
    if (status != SALTS_OK) {
      readiness_lane_remove(impl, listener, index);
      readiness_release_request(impl, request, index);
      readiness_counter_increment(&impl->native_submit_errors);
      return status;
    }
  } else {
    readiness_finish_attempt(
        impl, request, index, status, bytes, address_length);
  }

  readiness_counter_increment(&impl->submitted);
  *out_request = request->request;
  return SALTS_OK;
}

static int readiness_take_stream_accept(
    cmeta_io_impl *base,
    native_io_request request,
    uintptr_t *out_transport) {
  cmeta_io_readiness_impl *impl =
      (cmeta_io_readiness_impl *)base;
  uintptr_t token = UINTPTR_MAX;
  cmeta_io_readiness_accept_result *result;
  cmeta_io_readiness_endpoint *listener;
  int child_fd;
  int status;

  if (out_transport == NULL)
    return SALTS_EINVAL;
  *out_transport = UINTPTR_MAX;
  status = native_io_accept_escrow_take(
      &impl->accept_escrow, request, &token);
  if (status != SALTS_OK)
    return status;
  result = readiness_accept_result_from_token(impl, token);
  if (result == NULL || !result->live ||
      result->request.slot != request.slot ||
      result->request.generation != request.generation)
    return SALTS_EPROTO;

  listener = readiness_endpoint(impl, result->listener);
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

static bool readiness_supports_vector_write(const cmeta_io_impl *base,
                                            native_io_endpoint endpoint_handle) {
  const cmeta_io_readiness_impl *impl = (const cmeta_io_readiness_impl *)base;
  const cmeta_io_readiness_endpoint *endpoint;
  if (!native_io_endpoint_valid(endpoint_handle) ||
      endpoint_handle.slot > impl->endpoint_capacity)
    return false;
  endpoint = &impl->endpoints[endpoint_handle.slot - 1u];
  return endpoint->active && endpoint->generation == endpoint_handle.generation &&
         (endpoint->resource_kind == SALTS_IO_RESOURCE_STREAM_SOCKET ||
          endpoint->resource_kind == SALTS_IO_RESOURCE_BYTE_PIPE);
}

static int readiness_cancel(cmeta_io_impl *base, native_io_request request_handle) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  cmeta_io_readiness_request *request = readiness_request(impl, request_handle);
  cmeta_io_readiness_endpoint *endpoint;
  uint32_t index;
  int status;
  if (request == NULL) return SALTS_ENOENT;
  if (request->phase == SALTS_IO_READINESS_TERMINAL) return SALTS_EALREADY;
  endpoint = readiness_endpoint(impl, request->endpoint);
  if (endpoint == NULL) return SALTS_ENOENT;
  index = request_handle.slot - 1u;
  readiness_lane_remove(impl, endpoint, index);
  status = readiness_update_interests(impl, request->endpoint, endpoint);
  if (status != SALTS_OK) {
    readiness_lane_push(impl, endpoint, index);
    readiness_counter_increment(&impl->native_cancel_errors);
    return status;
  }
  readiness_publish_terminal(impl, request, index, NATIVE_IO_COMPLETION_CANCELLED, 0u,
                             SALTS_ECANCELED, (uint32_t)ECANCELED, 0u);
  return SALTS_OK;
}

static int readiness_drive_lane(cmeta_io_readiness_impl *impl, native_io_endpoint endpoint_handle,
                                cmeta_io_readiness_endpoint *endpoint, bool write_lane) {
  cmeta_io_readiness_lane *lane = readiness_lane(endpoint, write_lane);
  while (lane->head != SALTS_IO_INDEX_NONE) {
    const uint32_t index = lane->head;
    cmeta_io_readiness_request *request = &impl->requests[index];
    size_t bytes = 0u;
    size_t address_length = 0u;
    const int status = readiness_try_operation(endpoint, request, &bytes, &address_length);
    if (readiness_would_block(status)) break;
    readiness_lane_remove(impl, endpoint, index);
    readiness_finish_attempt(impl, request, index, status, bytes, address_length);
  }
  return readiness_update_interests(impl, endpoint_handle, endpoint);
}

static void readiness_drain_terminals(cmeta_io_readiness_impl *impl, native_io_completion *events,
                                      size_t limit, size_t *out_count) {
  while (*out_count < limit && impl->terminal_count != 0u) {
    const uint32_t index = impl->terminal_requests[impl->terminal_head];
    cmeta_io_readiness_request *request = &impl->requests[index];
    impl->terminal_head = (impl->terminal_head + 1u) % impl->request_capacity;
    --impl->terminal_count;
    events[(*out_count)++] = request->completion;
    readiness_release_request(impl, request, index);
  }
}

static int readiness_observe(cmeta_io_impl *base, native_io_completion *events,
                             size_t event_capacity, uint32_t timeout_ms, size_t *out_count) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  const size_t limit = event_capacity < impl->completion_batch_capacity
                           ? event_capacity
                           : impl->completion_batch_capacity;
  const uint64_t started_ms = cmeta_monotonic_ms();
  uint32_t wait_timeout = timeout_ms;
  readiness_drain_terminals(impl, events, limit, out_count);
  if (*out_count != 0u) return SALTS_OK;
  for (;;) {
    size_t ready_count = 0u;
    bool saw_wake = false;
    int status =
        impl->driver_ops->wait(impl->driver_state, impl->ready_events,
                               impl->completion_batch_capacity, wait_timeout, &ready_count);
    if (status != SALTS_OK) return status;
    for (size_t cursor = 0u; cursor < ready_count; ++cursor) {
      const cmeta_io_ready_event *ready = &impl->ready_events[cursor];
      if ((ready->interests & SALTS_IO_READY_WAKE) != 0u) {
        atomic_store_explicit(&impl->wake_pending, false, memory_order_release);
        saw_wake = true;
        continue;
      }
      const uint32_t slot = (uint32_t)ready->token;
      const uint32_t generation = (uint32_t)(ready->token >> 32u);
      native_io_endpoint handle = {slot, generation};
      cmeta_io_readiness_endpoint *endpoint = readiness_endpoint(impl, handle);
      if (endpoint == NULL) continue;
#if defined(SALTS_NATIVE_IO_TEST_DISPATCH)
      /* The event just came from the real OS wait. Test owners may now
       * cancel the matching pending NativeIO request BEFORE it is tried;
       * the normal backend still publishes the authoritative terminal. */
      if (g_dispatch_test_hook != NULL)
        g_dispatch_test_hook(g_dispatch_test_user, impl->base.kind, handle,
                             ready->interests);
#endif
      if ((ready->interests & (SALTS_IO_READY_READ | SALTS_IO_READY_ERROR)) != 0u) {
        status = readiness_drive_lane(impl, handle, endpoint, false);
        if (status != SALTS_OK) return status;
      }
      if ((ready->interests & (SALTS_IO_READY_WRITE | SALTS_IO_READY_ERROR)) != 0u) {
        status = readiness_drive_lane(impl, handle, endpoint, true);
        if (status != SALTS_OK) return status;
      }
    }
    readiness_drain_terminals(impl, events, limit, out_count);
    if (*out_count != 0u) return SALTS_OK;
    if (saw_wake) return SALTS_OK;
    wait_timeout = native_io_remaining_timeout(started_ms, timeout_ms);
    if (wait_timeout == 0u) return SALTS_ETIMEDOUT;
  }
}

static int readiness_wake(cmeta_io_impl *base) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  bool expected = false;
  int status;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (!atomic_compare_exchange_strong_explicit(&impl->wake_pending, &expected, true,
                                               memory_order_acq_rel, memory_order_acquire))
    return SALTS_OK;
  status = impl->driver_ops->wake(impl->driver_state);
  if (status == SALTS_OK) return SALTS_OK;
  atomic_store_explicit(&impl->wake_pending, false, memory_order_release);
  return status;
}

static int readiness_close(cmeta_io_impl *base) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  if (!impl->admission_open) return SALTS_EALREADY;
  impl->admission_open = false;
  return SALTS_OK;
}

static int readiness_destroy(cmeta_io_impl *base) {
  cmeta_io_readiness_impl *impl = (cmeta_io_readiness_impl *)base;
  int escrow_status;
  if (impl->admission_open || impl->active_requests != 0u || impl->endpoint_count != 0u)
    return SALTS_EBUSY;
  escrow_status = native_io_accept_escrow_destroy(
      &impl->accept_escrow);
  if (escrow_status != SALTS_OK)
    return escrow_status;
  impl->driver_ops->destroy(impl->driver_state);
  free(impl->terminal_requests);
  free(impl->free_requests);
  free(impl->free_endpoints);
  free(impl->ready_events);
  free(impl->accept_results);
  free(impl->requests);
  free(impl->endpoints);
  free(impl->driver_state);
  free(impl);
  return SALTS_OK;
}

static bool readiness_get_stats(const cmeta_io_impl *base, native_io_backend_stats *out_stats) {
  const cmeta_io_readiness_impl *impl = (const cmeta_io_readiness_impl *)base;
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

static const cmeta_io_impl_ops readiness_ops = {
    .attach_socket = readiness_attach_socket,
    .release_socket = readiness_release_socket,
    .submit = readiness_submit,
    .cancel = readiness_cancel,
    .observe = readiness_observe,
    .wake = readiness_wake,
    .close = readiness_close,
    .destroy = readiness_destroy,
    .get_stats = readiness_get_stats,
    .attach_pipe = readiness_attach_pipe,
    .release_pipe = readiness_release_pipe,
    .prepare = readiness_submit,
    .flush = NULL,
    .submit_vector = readiness_submit_vector,
    .supports_vector_write = readiness_supports_vector_write,
    .submit_stream_accept = readiness_submit_stream_accept,
    .take_stream_accept = readiness_take_stream_accept};

static bool readiness_array_fits(size_t count, size_t element_size) {
  return element_size != 0u && count <= SIZE_MAX / element_size;
}

int cmeta_io_readiness_backend_init(native_io_backend *backend,
                                    const native_io_backend_config *config,
                                    const cmeta_io_readiness_driver_ops *driver_ops,
                                    size_t driver_state_size) {
  cmeta_io_readiness_impl *impl;
  int status;
  if (driver_ops == NULL || driver_ops->init == NULL || driver_ops->update == NULL ||
      driver_ops->wait == NULL || driver_ops->wake == NULL || driver_ops->destroy == NULL ||
      driver_state_size == 0u ||
      !readiness_array_fits(config->endpoint_capacity, sizeof(cmeta_io_readiness_endpoint)) ||
      !readiness_array_fits(config->request_capacity, sizeof(cmeta_io_readiness_request)) ||
      !readiness_array_fits(config->request_capacity, sizeof(cmeta_io_readiness_accept_result)) ||
      !readiness_array_fits(config->completion_batch_capacity, sizeof(cmeta_io_ready_event)) ||
      !readiness_array_fits(config->endpoint_capacity, sizeof(uint32_t)) ||
      !readiness_array_fits(config->request_capacity, sizeof(uint32_t)))
    return SALTS_ERANGE;
  impl = (cmeta_io_readiness_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->driver_state = calloc(1u, driver_state_size);
  impl->endpoints =
      (cmeta_io_readiness_endpoint *)calloc(config->endpoint_capacity, sizeof(*impl->endpoints));
  impl->requests =
      (cmeta_io_readiness_request *)calloc(config->request_capacity, sizeof(*impl->requests));
  impl->accept_results =
      (cmeta_io_readiness_accept_result *)calloc(
          config->request_capacity, sizeof(*impl->accept_results));
  impl->ready_events = (cmeta_io_ready_event *)calloc(config->completion_batch_capacity,
                                                      sizeof(*impl->ready_events));
  impl->free_endpoints = (uint32_t *)calloc(config->endpoint_capacity, sizeof(uint32_t));
  impl->free_requests = (uint32_t *)calloc(config->request_capacity, sizeof(uint32_t));
  impl->terminal_requests = (uint32_t *)calloc(config->request_capacity, sizeof(uint32_t));
  if (impl->driver_state == NULL || impl->endpoints == NULL ||
      impl->requests == NULL || impl->accept_results == NULL ||
      impl->ready_events == NULL || impl->free_endpoints == NULL ||
      impl->free_requests == NULL || impl->terminal_requests == NULL) {
    status = SALTS_ENOMEM;
    goto failed;
  }
  impl->base.ops = &readiness_ops;
  impl->base.kind = config->kind;
  impl->driver_ops = driver_ops;
  impl->endpoint_capacity = config->endpoint_capacity;
  impl->request_capacity = config->request_capacity;
  impl->completion_batch_capacity = config->completion_batch_capacity;
  impl->free_endpoint_count = config->endpoint_capacity;
  impl->free_request_count = config->request_capacity;
  impl->admission_open = true;
  status = native_io_accept_escrow_init(
      &impl->accept_escrow, config->request_capacity,
      readiness_retire_accept_result, impl);
  if (status != SALTS_OK)
    goto failed;
  atomic_init(&impl->wake_pending, false);
  for (size_t index = 0u; index < config->endpoint_capacity; ++index) {
    impl->free_endpoints[index] = (uint32_t)(config->endpoint_capacity - index - 1u);
    impl->endpoints[index].fd = -1;
  }
  for (size_t index = 0u; index < config->request_capacity; ++index) {
    impl->free_requests[index] =
        (uint32_t)(config->request_capacity - index - 1u);
    impl->requests[index].accepted_fd = -1;
    impl->accept_results[index].child_fd = -1;
  }
  status = driver_ops->init(impl->driver_state, config->completion_batch_capacity);
  if (status != SALTS_OK) goto failed;
  backend->impl = impl;
  return SALTS_OK;

failed:
  (void)native_io_accept_escrow_destroy(&impl->accept_escrow);
  free(impl->terminal_requests);
  free(impl->free_requests);
  free(impl->free_endpoints);
  free(impl->ready_events);
  free(impl->accept_results);
  free(impl->requests);
  free(impl->endpoints);
  free(impl->driver_state);
  free(impl);
  return status;
}
