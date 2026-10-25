#include "cnet_module.h"
#include "cnet_transport.h"
#include "cnet_uri.h"
#include <cnet/ipc.h>
#include <limits.h>
#include <salts/native_ipc.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
  #include <errno.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

/* One owner; fixed-capacity accepted-handle FIFO, not a reflection/CSTL container.
 * No allocation after listener init. The queue owns accepted handles until
 * the caller takes them or stop drains and closes them. */
typedef struct cnet_ipc_ready_queue {
  cnet_ipc_accepted *items;
  size_t capacity;
  size_t front;
  size_t count;
} cnet_ipc_ready_queue;

static int cnet_ipc_queue_init(cnet_ipc_ready_queue *q, size_t capacity) {
  if (q == NULL || capacity == 0u) return SALTS_EINVAL;
  if (capacity > SIZE_MAX / sizeof(cnet_ipc_accepted)) return SALTS_ERANGE;
  q->items = (cnet_ipc_accepted *)calloc(capacity, sizeof(*q->items));
  if (q->items == NULL) return SALTS_ENOMEM;
  q->capacity = capacity;
  q->front = 0u;
  q->count = 0u;
  return SALTS_OK;
}

static void cnet_ipc_queue_destroy(cnet_ipc_ready_queue *q) {
  if (q == NULL) return;
  /* Callers close all owned native handles before invoking destroy. */
  free(q->items);
  *q = (cnet_ipc_ready_queue){0};
}

static size_t cnet_ipc_queue_size(const cnet_ipc_ready_queue *q) {
  return q != NULL ? q->count : 0u;
}

static bool cnet_ipc_queue_empty(const cnet_ipc_ready_queue *q) {
  return cnet_ipc_queue_size(q) == 0u;
}

static int cnet_ipc_queue_push(cnet_ipc_ready_queue *q,
                               const cnet_ipc_accepted *accepted) {
  size_t slot;
  if (q == NULL || accepted == NULL || q->items == NULL) return SALTS_EINVAL;
  if (q->count == q->capacity) return SALTS_ENOBUFS;
  slot = q->front + q->count;
  if (slot >= q->capacity) slot -= q->capacity;
  q->items[slot] = *accepted;
  ++q->count;
  return SALTS_OK;
}

static int cnet_ipc_queue_pop(cnet_ipc_ready_queue *q, cnet_ipc_accepted *out) {
  if (q == NULL || out == NULL || q->items == NULL) return SALTS_EINVAL;
  if (q->count == 0u) return SALTS_ENOENT;
  *out = q->items[q->front];
  q->items[q->front] = (cnet_ipc_accepted){0};
  if (++q->front == q->capacity) q->front = 0u;
  --q->count;
  return SALTS_OK;
}

typedef struct cnet_ipc_listener_impl {
  cnet_ipc_ready_queue ready;
  size_t accept_capacity;
  size_t child_capacity;
  int progress_status;
  int stop_status;
  bool advancing;
  bool stopping;
  bool stopped;
#if defined(_WIN32)
  cmeta_ipc_pipe_server server;
  uintptr_t *wait_handles;
#else
  int socket_value;
#endif
} cnet_ipc_listener_impl;

static bool cnet_ipc_descriptor_valid(const cnet_ipc_accepted *accepted) {
  return accepted != NULL && accepted->native_handle != UINTPTR_MAX &&
         (accepted->kind == CNET_IPC_RESOURCE_PIPE || accepted->kind == CNET_IPC_RESOURCE_SOCKET);
}

int cnet_ipc_accepted_close(cnet_ipc_accepted *accepted) {
  cnet_ipc_accepted owned;
  if (accepted == NULL) return SALTS_EINVAL;
  if (accepted->kind == CNET_IPC_RESOURCE_NONE) return SALTS_EALREADY;
  if (!cnet_ipc_descriptor_valid(accepted)) return SALTS_EINVAL;
  owned = *accepted;
  *accepted = (cnet_ipc_accepted){0};
#if defined(_WIN32)
  if (owned.kind == CNET_IPC_RESOURCE_PIPE) return cnet_transport_close_ipc(owned.native_handle);
  cnet_transport_close_socket(owned.native_handle);
  return SALTS_OK;
#else
  return cnet_transport_close_ipc(owned.native_handle);
#endif
}


#if defined(_WIN32)
static void cnet_ipc_accept_terminal(void *user, const cmeta_ipc_completion *completion,
                                     cmeta_ipc_pipe_endpoint endpoint) {
  cnet_ipc_listener_impl *impl = (cnet_ipc_listener_impl *)user;
  int status = SALTS_OK;
  if (cmeta_ipc_pipe_endpoint_valid(&endpoint)) {
    cnet_ipc_accepted accepted = {endpoint.handle, CNET_IPC_RESOURCE_PIPE};
    if (!impl->stopping) status = cnet_ipc_queue_push(&impl->ready, &accepted);
    if (impl->stopping || status != SALTS_OK) {
      const int close_status = cnet_ipc_accepted_close(&accepted);
      if (status == SALTS_OK) status = close_status;
    }
  } else if (!(impl->stopping && completion->kind == SALTS_IPC_COMPLETION_CANCELLED)) {
    status = completion->status < SALTS_OK ? completion->status : SALTS_EPROTO;
  }
  if (impl->progress_status == SALTS_OK) impl->progress_status = status;
}
#else
static int cnet_ipc_nonblocking(int descriptor) {
  int flags = fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) return -errno;
  flags = fcntl(descriptor, F_GETFD, 0);
  if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) return -errno;
  return SALTS_OK;
}
#endif

int cnet_ipc_listener_init(cnet_ipc_listener *listener, const cnet_ipc_listener_config *config) {
  cnet_ipc_listener_impl *impl;
  cnet_uri uri;
  int status;
  if (listener == NULL || config == NULL || config->size != sizeof(*config) ||
      config->version != CNET_IPC_VERSION || config->accept_capacity == 0u ||
      config->child_capacity < config->accept_capacity || config->input_buffer_bytes == 0u ||
      config->output_buffer_bytes == 0u)
    return SALTS_EINVAL;
  if (listener->impl != NULL) return SALTS_EALREADY;
  if (!native_io_backend_kind_supported(config->backend)) return SALTS_ENOTSUP;
  if (config->child_capacity > SIZE_MAX / sizeof(cnet_ipc_accepted) ||
      config->accept_capacity > SIZE_MAX / sizeof(uintptr_t))
    return SALTS_ERANGE;
#if defined(_WIN32)
  if (config->backend != NATIVE_IO_BACKEND_IOCP) return SALTS_ENOTSUP;
  if (config->accept_capacity > 255u || config->input_buffer_bytes > UINT32_MAX ||
      config->output_buffer_bytes > UINT32_MAX)
    return SALTS_ERANGE;
#else
  if (config->accept_capacity > INT_MAX) return SALTS_ERANGE;
#endif
  status = cnet_uri_parse(config->uri, &uri);
  if (status != SALTS_OK) return status;
  if (uri.scheme != CNET_URI_IPC) return SALTS_EINVAL;
  status = cnet_module_init();
  if (status != SALTS_OK) return status;
  impl = (cnet_ipc_listener_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) {
    (void)cnet_module_shutdown();
    return SALTS_ENOMEM;
  }
#if !defined(_WIN32)
  impl->socket_value = -1;
#endif
  impl->accept_capacity = config->accept_capacity;
  impl->child_capacity = config->child_capacity;
  status = cnet_ipc_queue_init(&impl->ready, config->child_capacity);
  if (status != SALTS_OK) goto fail;
#if defined(_WIN32)
  {
    tstr name = NULL;
    cmeta_ipc_pipe_server_config native_config = {0};
    impl->wait_handles = (uintptr_t *)calloc(config->accept_capacity, sizeof(uintptr_t));
    if (impl->wait_handles == NULL) {
      status = SALTS_ENOMEM;
      goto fail;
    }
    status = cnet_transport_ipc_native_name(uri.path, &name);
    if (status != SALTS_OK) goto fail;
    native_config.name = name;
    native_config.direction = SALTS_IPC_PIPE_DUPLEX;
    native_config.request_capacity = config->accept_capacity;
    native_config.input_buffer_size = config->input_buffer_bytes;
    native_config.output_buffer_size = config->output_buffer_bytes;
    native_config.completion = cnet_ipc_accept_terminal;
    native_config.completion_user = impl;
    status = cmeta_ipc_pipe_server_init(&impl->server, &native_config);
    tstr_free(name);
    if (status != SALTS_OK) goto fail;
  }
#else
  {
    struct sockaddr_un address;
    size_t length = 0u;
    status = cnet_transport_ipc_address(uri.path, &address, sizeof(address), &length);
    if (status != SALTS_OK) goto fail;
    impl->socket_value = socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl->socket_value < 0) {
      status = -errno;
      goto fail;
    }
    status = cnet_ipc_nonblocking(impl->socket_value);
    if (status != SALTS_OK) goto fail;
    if (bind(impl->socket_value, (const struct sockaddr *)&address, (socklen_t)length) != 0 ||
        listen(impl->socket_value, (int)config->accept_capacity) != 0) {
      status = -errno;
      goto fail;
    }
  }
#endif
  listener->impl = impl;
  return SALTS_OK;
fail:
#if defined(_WIN32)
  free(impl->wait_handles);
#else
  if (impl->socket_value >= 0) (void)close(impl->socket_value);
#endif
  cnet_ipc_queue_destroy(&impl->ready);
  free(impl);
  (void)cnet_module_shutdown();
  return status;
}

int cnet_ipc_listener_advance(cnet_ipc_listener *listener, size_t max_events, size_t *out_events) {
  cnet_ipc_listener_impl *impl = listener != NULL ? listener->impl : NULL;
  int status = SALTS_OK;
  if (out_events != NULL) *out_events = 0u;
  if (impl == NULL || out_events == NULL || max_events == 0u) return SALTS_EINVAL;
  if (impl->advancing) return SALTS_EBUSY;
  if (impl->stopped) return SALTS_OK;
  impl->advancing = true;
  impl->progress_status = SALTS_OK;
#if defined(_WIN32)
  status = cmeta_ipc_pipe_server_observe(&impl->server, max_events, out_events);
  if (status == SALTS_OK) status = impl->progress_status;
  if (!impl->stopping && status == SALTS_OK) {
    cmeta_ipc_pipe_server_stats stats;
    if (!cmeta_ipc_pipe_server_get_stats(&impl->server, &stats)) status = SALTS_EPROTO;
    else {
      size_t active = stats.active_requests;
      const size_t ready = cnet_ipc_queue_size(&impl->ready);
      for (size_t count = 0u; count < max_events && active < impl->accept_capacity &&
                              active < impl->child_capacity - ready;
           ++count, ++active) {
        cmeta_ipc_request_id request;
        status = cmeta_ipc_pipe_server_try_accept(&impl->server, &request);
        if (status != SALTS_OK) break;
      }
    }
  }
#else
  while (!impl->stopping && *out_events < max_events &&
         cnet_ipc_queue_size(&impl->ready) < impl->child_capacity) {
    int descriptor = accept(impl->socket_value, NULL, NULL);
    cnet_ipc_accepted accepted;
    if (descriptor < 0) {
      status = errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? SALTS_OK : -errno;
      break;
    }
    accepted = (cnet_ipc_accepted){(uintptr_t)descriptor, CNET_IPC_RESOURCE_SOCKET};
    status = cnet_ipc_nonblocking(descriptor);
    if (status == SALTS_OK)
      status = cnet_ipc_queue_push(&impl->ready, &accepted);
    if (status != SALTS_OK) {
      (void)cnet_ipc_accepted_close(&accepted);
      break;
    }
    ++*out_events;
  }
#endif
  if (impl->stopping && status != SALTS_OK && impl->stop_status == SALTS_OK)
    impl->stop_status = status;
  impl->advancing = false;
  return status;
}

int cnet_ipc_listener_accept_detached(cnet_ipc_listener *listener,
                                      cnet_ipc_accepted *out_accepted) {
  cnet_ipc_listener_impl *impl = listener != NULL ? listener->impl : NULL;
  if (impl == NULL || out_accepted == NULL) return SALTS_EINVAL;
  if (out_accepted->kind != CNET_IPC_RESOURCE_NONE) return SALTS_EALREADY;
  if (impl->advancing) return SALTS_EBUSY;
  if (impl->stopping) return SALTS_ESHUTDOWN;
  return cnet_ipc_queue_pop(&impl->ready, out_accepted);
}

int cnet_ipc_listener_wait_sources(const cnet_ipc_listener *listener,
                                   cnet_ipc_wait_source *out_sources, size_t capacity,
                                   size_t *out_count, bool *out_ready) {
  cnet_ipc_listener_impl *impl = listener != NULL ? listener->impl : NULL;
  size_t count = 0u;
  if (out_count != NULL) *out_count = 0u;
  if (out_ready != NULL) *out_ready = false;
  if (impl == NULL || out_count == NULL || out_ready == NULL ||
      (capacity != 0u && out_sources == NULL))
    return SALTS_EINVAL;
  if (impl->advancing) return SALTS_EBUSY;
  if (impl->stopped) return SALTS_OK;
#if defined(_WIN32)
  {
    const int status = cmeta_ipc_pipe_server_wait_sources(&impl->server, impl->wait_handles,
                                                          impl->accept_capacity, &count, out_ready);
    if (status != SALTS_OK) return status;
  }
#else
  if (!impl->stopping && cnet_ipc_queue_size(&impl->ready) < impl->child_capacity) count = 1u;
#endif
  *out_ready = *out_ready || !cnet_ipc_queue_empty(&impl->ready);
  *out_count = count;
  if (count > capacity) return SALTS_ENOBUFS;
  for (size_t index = 0; index < count; ++index) {
#if defined(_WIN32)
    out_sources[index] =
        (cnet_ipc_wait_source){CNET_IPC_WAIT_WINDOWS_EVENT, impl->wait_handles[index]};
#else
    out_sources[index] =
        (cnet_ipc_wait_source){CNET_IPC_WAIT_READABLE_FD, (uintptr_t)impl->socket_value};
#endif
  }
  return SALTS_OK;
}

int cnet_ipc_listener_stop(cnet_ipc_listener *listener, bool *out_stopped) {
  cnet_ipc_listener_impl *impl = listener != NULL ? listener->impl : NULL;
  int status = SALTS_OK;
  if (out_stopped != NULL) *out_stopped = false;
  if (impl == NULL || out_stopped == NULL) return SALTS_EINVAL;
  if (impl->advancing) return SALTS_EBUSY;
  impl->stopping = true;
  while (!cnet_ipc_queue_empty(&impl->ready)) {
    cnet_ipc_accepted accepted = {0};
    status = cnet_ipc_queue_pop(&impl->ready, &accepted);
    if (status != SALTS_OK) return status;
    status = cnet_ipc_accepted_close(&accepted);
    if (status != SALTS_OK && impl->stop_status == SALTS_OK) impl->stop_status = status;
  }
#if defined(_WIN32)
  if (!impl->stopped) {
    status = cmeta_ipc_pipe_server_close(&impl->server);
    if (status != SALTS_OK && impl->stop_status == SALTS_OK) impl->stop_status = status;
    impl->stopped = cmeta_ipc_pipe_server_is_quiescent(&impl->server);
  }
#else
  if (impl->socket_value >= 0) {
    status = close(impl->socket_value) == 0 ? SALTS_OK : -errno;
    impl->socket_value = -1;
    if (status != SALTS_OK && impl->stop_status == SALTS_OK) impl->stop_status = status;
  }
  impl->stopped = true;
#endif
  *out_stopped = impl->stopped;
  if (impl->stop_status != SALTS_OK) return impl->stop_status;
  return impl->stopped ? SALTS_OK : SALTS_EBUSY;
}

int cnet_ipc_listener_destroy(cnet_ipc_listener *listener) {
  cnet_ipc_listener_impl *impl;
  if (listener == NULL) return SALTS_EINVAL;
  impl = listener->impl;
  if (impl == NULL) return SALTS_OK;
  if (impl->advancing || !impl->stopped) return SALTS_EBUSY;
#if defined(_WIN32)
  {
    const int status = cmeta_ipc_pipe_server_destroy(&impl->server);
    if (status != SALTS_OK) return status;
  }
  free(impl->wait_handles);
#endif
  cnet_ipc_queue_destroy(&impl->ready);
  free(impl);
  listener->impl = NULL;
  return cnet_module_shutdown();
}
