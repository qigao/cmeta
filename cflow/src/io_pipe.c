#include <cflow/io_pipe.h>

#include <error_codes.h>
#include <native_ipc.h>

#include <stdlib.h>

typedef struct cflow_io_pipe_server_impl {
  cmeta_ipc_pipe_server native;
  cflow_io_pipe_accept_completion_fn completion;
  void *completion_user;
} cflow_io_pipe_server_impl;

static cflow_io_pipe_submit_result pipe_submit_result(cflow_io_pipe_submit_status status,
                                                      cflow_io_request_id request_id, int error) {
  cflow_io_pipe_submit_result result;
  result.status = status;
  result.request_id = request_id;
  result.error = error;
  return result;
}

static bool pipe_direction_to_native(cflow_io_pipe_direction direction,
                                     cmeta_ipc_pipe_direction *out) {
  if (out == NULL) return false;
  switch (direction) {
  case CFLOW_IO_PIPE_READ:
    *out = SALTS_IPC_PIPE_READ;
    return true;
  case CFLOW_IO_PIPE_WRITE:
    *out = SALTS_IPC_PIPE_WRITE;
    return true;
  case CFLOW_IO_PIPE_DUPLEX:
    *out = SALTS_IPC_PIPE_DUPLEX;
    return true;
  }
  return false;
}

static bool pipe_capability_to_native(cflow_io_pipe_capability capability,
                                      cmeta_ipc_pipe_capability *out) {
  if (out == NULL) return false;
  switch (capability) {
  case CFLOW_IO_PIPE_WINDOWS_SERVER_ACCEPT:
    *out = SALTS_IPC_WINDOWS_SERVER_ACCEPT;
    return true;
  case CFLOW_IO_PIPE_WINDOWS_CLIENT_CONNECT:
    *out = SALTS_IPC_WINDOWS_CLIENT_CONNECT;
    return true;
  case CFLOW_IO_PIPE_POSIX_FIFO_OPEN:
    *out = SALTS_IPC_POSIX_FIFO_OPEN;
    return true;
  }
  return false;
}

static cmeta_ipc_pipe_endpoint pipe_endpoint_to_native(const cflow_io_pipe_endpoint *endpoint) {
  cmeta_ipc_pipe_endpoint native;
  cmeta_ipc_pipe_endpoint_init(&native);
  if (endpoint != NULL) {
    native.handle = endpoint->handle;
    native.native_io_flags = endpoint->flags;
  }
  return native;
}

static cflow_io_completion pipe_completion_from_native(const cmeta_ipc_completion *native) {
  cflow_io_completion completion = {CFLOW_IO_COMPLETION_FAILED, 0u, native->status};
  switch (native->kind) {
  case SALTS_IPC_COMPLETION_OK:
    completion.kind = CFLOW_IO_COMPLETION_OK;
    completion.error = SALTS_OK;
    break;
  case SALTS_IPC_COMPLETION_CANCELLED:
    completion.kind = CFLOW_IO_COMPLETION_CANCELLED;
    completion.error = SALTS_OK;
    break;
  case SALTS_IPC_COMPLETION_FAILED:
    break;
  }
  return completion;
}

static void pipe_accept_completion(void *user, const cmeta_ipc_completion *native_completion,
                                   cmeta_ipc_pipe_endpoint native_endpoint) {
  cflow_io_pipe_server_impl *impl = (cflow_io_pipe_server_impl *)user;
  cflow_io_pipe_endpoint endpoint;
  cflow_io_completion completion = pipe_completion_from_native(native_completion);

  cflow_io_pipe_endpoint_init(&endpoint);
  if (cmeta_ipc_pipe_endpoint_valid(&native_endpoint)) {
    endpoint.handle = native_endpoint.handle;
    endpoint.flags = native_endpoint.native_io_flags;
    cmeta_ipc_pipe_endpoint_init(&native_endpoint);
  }
  impl->completion(impl->completion_user, (cflow_io_request_id)native_completion->request_id,
                   &completion, endpoint);
}

bool cflow_io_pipe_capability_supported(cflow_io_pipe_capability capability) {
  cmeta_ipc_pipe_capability native;
  return pipe_capability_to_native(capability, &native) &&
         cmeta_ipc_pipe_capability_supported(native);
}

void cflow_io_pipe_endpoint_init(cflow_io_pipe_endpoint *endpoint) {
  cmeta_ipc_pipe_endpoint native;
  if (endpoint == NULL) return;
  cmeta_ipc_pipe_endpoint_init(&native);
  endpoint->handle = native.handle;
  endpoint->flags = native.native_io_flags;
}

bool cflow_io_pipe_endpoint_is_valid(const cflow_io_pipe_endpoint *endpoint) {
  cmeta_ipc_pipe_endpoint native;
  if (endpoint == NULL) return false;
  native = pipe_endpoint_to_native(endpoint);
  return cmeta_ipc_pipe_endpoint_valid(&native);
}

int cflow_io_pipe_endpoint_close(cflow_io_pipe_endpoint *endpoint) {
  cmeta_ipc_pipe_endpoint native;
  if (endpoint == NULL) return SALTS_EINVAL;
  native = pipe_endpoint_to_native(endpoint);
  cflow_io_pipe_endpoint_init(endpoint);
  return cmeta_ipc_pipe_endpoint_close(&native);
}

int cflow_io_pipe_server_init(cflow_io_pipe_server *server,
                              const cflow_io_pipe_server_config *config) {
  cflow_io_pipe_server_impl *impl;
  cmeta_ipc_pipe_server_config native_config;
  cmeta_ipc_pipe_direction native_direction;
  int status;

  if (!cmeta_ipc_pipe_capability_supported(SALTS_IPC_WINDOWS_SERVER_ACCEPT)) return SALTS_ENOTSUP;
  if (server == NULL || server->impl != NULL || config == NULL || config->completion == NULL ||
      !pipe_direction_to_native(config->direction, &native_direction))
    return SALTS_EINVAL;
  impl = (cflow_io_pipe_server_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  native_config = (cmeta_ipc_pipe_server_config){config->name,
                                                 native_direction,
                                                 config->request_capacity,
                                                 config->input_buffer_size,
                                                 config->output_buffer_size,
                                                 pipe_accept_completion,
                                                 impl};
  impl->completion = config->completion;
  impl->completion_user = config->completion_user;
  status = cmeta_ipc_pipe_server_init(&impl->native, &native_config);
  if (status != SALTS_OK) {
    free(impl);
    return status;
  }
  server->impl = impl;
  return SALTS_OK;
}

cflow_io_pipe_submit_result cflow_io_pipe_server_try_accept(cflow_io_pipe_server *server) {
  cflow_io_pipe_server_impl *impl =
      server != NULL ? (cflow_io_pipe_server_impl *)server->impl : NULL;
  cmeta_ipc_request_id request_id = 0u;
  int status;

  if (!cmeta_ipc_pipe_capability_supported(SALTS_IPC_WINDOWS_SERVER_ACCEPT))
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_UNSUPPORTED, 0u, SALTS_ENOTSUP);
  if (impl == NULL)
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_INVALID_ARGUMENT, 0u, SALTS_EINVAL);
  status = cmeta_ipc_pipe_server_try_accept(&impl->native, &request_id);
  switch (status) {
  case SALTS_OK:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_ACCEPTED, (cflow_io_request_id)request_id,
                              SALTS_OK);
  case SALTS_EINVAL:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_INVALID_ARGUMENT, 0u, status);
  case SALTS_ENOTSUP:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_UNSUPPORTED, 0u, status);
  case SALTS_ENOBUFS:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_FULL, 0u, SALTS_EBUSY);
  case SALTS_ESHUTDOWN:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_CLOSED, 0u, SALTS_EALREADY);
  case SALTS_ERANGE:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_ID_EXHAUSTED, 0u, status);
  default:
    return pipe_submit_result(CFLOW_IO_PIPE_SUBMIT_NATIVE_ERROR, 0u, status);
  }
}

cflow_io_cancel_status cflow_io_pipe_server_try_cancel(cflow_io_pipe_server *server,
                                                       cflow_io_request_id request_id) {
  cflow_io_pipe_server_impl *impl =
      server != NULL ? (cflow_io_pipe_server_impl *)server->impl : NULL;
  int status;
  if (impl == NULL || request_id == 0u) return CFLOW_IO_CANCEL_INVALID_ARGUMENT;
  status = cmeta_ipc_pipe_server_cancel(&impl->native, (cmeta_ipc_request_id)request_id);
  return status == SALTS_OK ? CFLOW_IO_CANCEL_ACCEPTED : CFLOW_IO_CANCEL_NOT_FOUND;
}

int cflow_io_pipe_server_run_ready(cflow_io_pipe_server *server, size_t max_steps,
                                   size_t *progressed) {
  cflow_io_pipe_server_impl *impl =
      server != NULL ? (cflow_io_pipe_server_impl *)server->impl : NULL;
  if (!cmeta_ipc_pipe_capability_supported(SALTS_IPC_WINDOWS_SERVER_ACCEPT)) {
    if (progressed != NULL) *progressed = 0u;
    return SALTS_ENOTSUP;
  }
  if (impl == NULL) {
    if (progressed != NULL) *progressed = 0u;
    return SALTS_EINVAL;
  }
  return cmeta_ipc_pipe_server_observe(&impl->native, max_steps, progressed);
}

int cflow_io_pipe_server_close(cflow_io_pipe_server *server) {
  cflow_io_pipe_server_impl *impl =
      server != NULL ? (cflow_io_pipe_server_impl *)server->impl : NULL;
  if (!cmeta_ipc_pipe_capability_supported(SALTS_IPC_WINDOWS_SERVER_ACCEPT)) return SALTS_ENOTSUP;
  return impl != NULL ? cmeta_ipc_pipe_server_close(&impl->native) : SALTS_EINVAL;
}

bool cflow_io_pipe_server_is_quiescent(const cflow_io_pipe_server *server) {
  const cflow_io_pipe_server_impl *impl =
      server != NULL ? (const cflow_io_pipe_server_impl *)server->impl : NULL;
  return impl != NULL && cmeta_ipc_pipe_server_is_quiescent(&impl->native);
}

bool cflow_io_pipe_server_get_stats(const cflow_io_pipe_server *server,
                                    cflow_io_pipe_server_stats *out) {
  const cflow_io_pipe_server_impl *impl =
      server != NULL ? (const cflow_io_pipe_server_impl *)server->impl : NULL;
  cmeta_ipc_pipe_server_stats native;
  if (impl == NULL || out == NULL || !cmeta_ipc_pipe_server_get_stats(&impl->native, &native))
    return false;
  if (native.completed < native.cancelled) return false;
  out->request_capacity = native.request_capacity;
  out->active_requests = native.active_requests;
  out->submitted = native.submitted;
  out->completed = native.completed - native.cancelled;
  out->cancelled = native.cancelled;
  out->rejected_full = native.rejected_full;
  out->admission_open = native.admission_open;
  return true;
}

int cflow_io_pipe_server_destroy(cflow_io_pipe_server *server) {
  cflow_io_pipe_server_impl *impl =
      server != NULL ? (cflow_io_pipe_server_impl *)server->impl : NULL;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  status = cmeta_ipc_pipe_server_destroy(&impl->native);
  if (status != SALTS_OK) return status;
  free(impl);
  server->impl = NULL;
  return SALTS_OK;
}

int cflow_io_pipe_client_connect(const char *name, cflow_io_pipe_direction direction,
                                 cflow_io_pipe_endpoint *out) {
  cmeta_ipc_pipe_direction native_direction;
  cmeta_ipc_pipe_endpoint native;
  int status;
  if (!cmeta_ipc_pipe_capability_supported(SALTS_IPC_WINDOWS_CLIENT_CONNECT)) return SALTS_ENOTSUP;
  if (!pipe_direction_to_native(direction, &native_direction) || out == NULL) return SALTS_EINVAL;
  native = pipe_endpoint_to_native(out);
  status = cmeta_ipc_named_pipe_connect(name, native_direction, &native);
  if (status != SALTS_OK) return status;
  out->handle = native.handle;
  out->flags = native.native_io_flags;
  cmeta_ipc_pipe_endpoint_init(&native);
  return SALTS_OK;
}

int cflow_io_fifo_open(const char *path, cflow_io_pipe_direction direction,
                       cflow_io_pipe_endpoint *out) {
  cmeta_ipc_pipe_direction native_direction;
  cmeta_ipc_pipe_endpoint native;
  int status;
  if (!cmeta_ipc_pipe_capability_supported(SALTS_IPC_POSIX_FIFO_OPEN)) return SALTS_ENOTSUP;
  if (!pipe_direction_to_native(direction, &native_direction) || out == NULL) return SALTS_EINVAL;
  native = pipe_endpoint_to_native(out);
  status = cmeta_ipc_fifo_open(path, native_direction, &native);
  if (status != SALTS_OK) return status;
  out->handle = native.handle;
  out->flags = native.native_io_flags;
  cmeta_ipc_pipe_endpoint_init(&native);
  return SALTS_OK;
}
