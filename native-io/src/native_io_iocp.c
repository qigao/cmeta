#include "native_io_internal.h"
#include "native_io_accept_escrow.h"

#include <salts/error_codes.h>

#if defined(interface)
  #undef interface
#endif
#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <winternl.h>

#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum cmeta_iocp_record_phase {
  SALTS_IOCP_RECORD_FREE = 0,
  SALTS_IOCP_RECORD_PENDING
} cmeta_iocp_record_phase;

typedef struct cmeta_iocp_endpoint_record {
  uintptr_t native_handle;
  uint32_t generation;
  size_t active_requests;
  size_t retained_accepts;
  cmeta_io_resource_kind resource_kind;
  LPFN_CONNECTEX connect_ex;
  LPFN_ACCEPTEX accept_ex;
  bool connected;
  bool connect_active;
  bool active;
} cmeta_iocp_endpoint_record;

enum {
  SALTS_IOCP_ACCEPT_ADDRESS_BYTES = sizeof(SOCKADDR_STORAGE) + 16u
};

typedef struct cmeta_iocp_request_record {
  OVERLAPPED overlapped;
  WSABUF buffer;
  WSABUF vector_buffers[NATIVE_IO_VECTOR_MAX];
  DWORD vector_buffer_count;
  cmeta_iocp_record_phase phase;
  native_io_request request;
  native_io_endpoint endpoint;
  native_io_operation_kind operation_kind;
  uintptr_t native_handle;
  DWORD flags;
  void *address;
  int address_length;
  uintptr_t user_data;
  SOCKET accepted_socket;
  unsigned char accept_address[SALTS_IOCP_ACCEPT_ADDRESS_BYTES * 2u];
  bool stream_accept;
} cmeta_iocp_request_record;

typedef struct cmeta_iocp_accept_result {
  native_io_request request;
  native_io_endpoint listener;
  SOCKET child;
  bool live;
} cmeta_iocp_accept_result;

_Static_assert(offsetof(cmeta_iocp_request_record, overlapped) == 0u,
               "OVERLAPPED must remain the stable request prefix");

typedef struct cmeta_iocp_impl {
  cmeta_io_impl base;
  HANDLE port;
  cmeta_iocp_endpoint_record *endpoints;
  cmeta_iocp_request_record *requests;
  cmeta_iocp_accept_result *accept_results;
  native_io_accept_escrow accept_escrow;
  uint32_t *free_endpoints;
  uint32_t *free_requests;
  size_t endpoint_capacity;
  size_t request_capacity;
  size_t completion_batch_capacity;
  size_t free_endpoint_count;
  size_t free_request_count;
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
  bool winsock_started;
  atomic_bool wake_pending;
} cmeta_iocp_impl;

typedef struct cmeta_iocp_file_mode_information {
  ULONG mode;
} cmeta_iocp_file_mode_information;

typedef NTSTATUS(NTAPI *cmeta_iocp_query_file_fn)(HANDLE file, PIO_STATUS_BLOCK status,
                                                  PVOID information, ULONG length,
                                                  FILE_INFORMATION_CLASS information_class);

enum { SALTS_IOCP_FILE_MODE_INFORMATION_CLASS = 16 };

#define SALTS_IOCP_WAKE_KEY ((ULONG_PTR)UINTPTR_MAX)

static void iocp_counter_increment(uint64_t *counter) {
  if (*counter != UINT64_MAX) ++*counter;
}

static uint32_t iocp_next_generation(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static int iocp_native_error(DWORD error) {
  if (error == ERROR_SUCCESS || error > (DWORD)INT_MAX) return SALTS_EIO;
  return -(int)error;
}

static cmeta_iocp_endpoint_record *iocp_endpoint(
    cmeta_iocp_impl *impl, native_io_endpoint endpoint);

static cmeta_iocp_accept_result *iocp_accept_result_from_token(
    cmeta_iocp_impl *impl, uintptr_t token) {
  const uintptr_t base = (uintptr_t)impl->accept_results;
  const uintptr_t value = token;
  const size_t bytes = impl->request_capacity * sizeof(*impl->accept_results);
  uintptr_t offset;
  if (impl->accept_results == NULL || value < base || value - base >= bytes)
    return NULL;
  offset = value - base;
  if (offset % sizeof(*impl->accept_results) != 0u)
    return NULL;
  return &impl->accept_results[offset / sizeof(*impl->accept_results)];
}

static int iocp_retire_accept_result(void *context, uintptr_t transport) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)context;
  cmeta_iocp_accept_result *result =
      iocp_accept_result_from_token(impl, transport);
  cmeta_iocp_endpoint_record *listener;
  int status = SALTS_OK;

  if (result == NULL || !result->live)
    return SALTS_EINVAL;

  listener = iocp_endpoint(impl, result->listener);
  if (result->child != INVALID_SOCKET &&
      closesocket(result->child) == SOCKET_ERROR)
    status = iocp_native_error((DWORD)WSAGetLastError());
  if (listener != NULL && listener->retained_accepts != 0u)
    --listener->retained_accepts;
  memset(result, 0, sizeof(*result));
  result->child = INVALID_SOCKET;
  return status;
}

static cmeta_iocp_accept_result *iocp_accept_result_reserve(
    cmeta_iocp_impl *impl, native_io_request request,
    native_io_endpoint listener, SOCKET child) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    cmeta_iocp_accept_result *result = &impl->accept_results[index];
    if (result->live)
      continue;
    result->request = request;
    result->listener = listener;
    result->child = child;
    result->live = true;
    return result;
  }
  return NULL;
}

static int iocp_publish_accept_result(
    cmeta_iocp_impl *impl, cmeta_iocp_request_record *request) {
  cmeta_iocp_accept_result *result;
  cmeta_iocp_endpoint_record *listener;
  int status;

  if (request->accepted_socket == INVALID_SOCKET)
    return SALTS_EPROTO;
  result = iocp_accept_result_reserve(
      impl, request->request, request->endpoint,
      request->accepted_socket);
  if (result == NULL)
    return SALTS_ENOBUFS;

  status = native_io_accept_escrow_publish(
      &impl->accept_escrow, request->request,
      (uintptr_t)result);
  if (status != SALTS_OK) {
    memset(result, 0, sizeof(*result));
    result->child = INVALID_SOCKET;
    return status;
  }

  listener = iocp_endpoint(impl, request->endpoint);
  if (listener == NULL) {
    (void)native_io_accept_escrow_discard(
        &impl->accept_escrow, request->request);
    return SALTS_ENOENT;
  }
  ++listener->retained_accepts;
  request->accepted_socket = INVALID_SOCKET;
  return SALTS_OK;
}

static int iocp_discard_listener_accepts(
    cmeta_iocp_impl *impl, native_io_endpoint listener_handle) {
  int first_status = SALTS_OK;
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    cmeta_iocp_accept_result *result = &impl->accept_results[index];
    int status;
    if (!result->live ||
        result->listener.slot != listener_handle.slot ||
        result->listener.generation != listener_handle.generation)
      continue;
    status = native_io_accept_escrow_discard(
        &impl->accept_escrow, result->request);
    if (first_status == SALTS_OK && status != SALTS_OK)
      first_status = status;
  }
  return first_status;
}

static cmeta_iocp_endpoint_record *iocp_endpoint(cmeta_iocp_impl *impl,
                                                 native_io_endpoint endpoint) {
  cmeta_iocp_endpoint_record *record;
  if (!native_io_endpoint_valid(endpoint) || endpoint.slot > impl->endpoint_capacity) return NULL;
  record = &impl->endpoints[endpoint.slot - 1u];
  return record->active && record->generation == endpoint.generation ? record : NULL;
}

static cmeta_iocp_request_record *iocp_request(cmeta_iocp_impl *impl, native_io_request request) {
  cmeta_iocp_request_record *record;
  if (!native_io_request_valid(request) || request.slot > impl->request_capacity) return NULL;
  record = &impl->requests[request.slot - 1u];
  return record->phase == SALTS_IOCP_RECORD_PENDING &&
                 record->request.generation == request.generation
             ? record
             : NULL;
}

static int iocp_attach_endpoint(cmeta_iocp_impl *impl, uintptr_t native_handle,
                                cmeta_io_resource_kind resource_kind,
                                native_io_endpoint *out_endpoint) {
  uint32_t index;
  cmeta_iocp_endpoint_record *endpoint;
  HANDLE associated;
  size_t cursor;

  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  for (cursor = 0u; cursor < impl->endpoint_capacity; ++cursor) {
    if (impl->endpoints[cursor].active && impl->endpoints[cursor].native_handle == native_handle)
      return SALTS_EALREADY;
  }
  if (impl->free_endpoint_count == 0u) return SALTS_ENOBUFS;

  index = impl->free_endpoints[impl->free_endpoint_count - 1u];
  endpoint = &impl->endpoints[index];
  associated =
      CreateIoCompletionPort((HANDLE)native_handle, impl->port, (ULONG_PTR)(index + 1u), 0u);
  if (associated != impl->port) return iocp_native_error(GetLastError());

  --impl->free_endpoint_count;
  endpoint->generation = iocp_next_generation(endpoint->generation);
  endpoint->native_handle = native_handle;
  endpoint->active_requests = 0u;
  endpoint->retained_accepts = 0u;
  endpoint->resource_kind = resource_kind;
  endpoint->connect_ex = NULL;
  endpoint->accept_ex = NULL;
  endpoint->connected = false;
  endpoint->connect_active = false;
  endpoint->active = true;
  ++impl->endpoint_count;
  *out_endpoint = (native_io_endpoint){index + 1u, endpoint->generation};
  return SALTS_OK;
}

static int iocp_release_endpoint(cmeta_iocp_impl *impl, native_io_endpoint endpoint_handle,
                                 bool socket_endpoint) {
  cmeta_iocp_endpoint_record *endpoint = iocp_endpoint(impl, endpoint_handle);
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
    retire_status = iocp_discard_listener_accepts(
        impl, endpoint_handle);
    if (endpoint->retained_accepts != 0u)
      return retire_status == SALTS_OK ? SALTS_EPROTO : retire_status;
    if (retire_status != SALTS_OK)
      return retire_status;
  }

  index = endpoint_handle.slot - 1u;
  endpoint->native_handle = UINTPTR_MAX;
  endpoint->resource_kind = (cmeta_io_resource_kind)0;
  endpoint->connect_ex = NULL;
  endpoint->accept_ex = NULL;
  endpoint->retained_accepts = 0u;
  endpoint->connected = false;
  endpoint->connect_active = false;
  endpoint->active = false;
  impl->free_endpoints[impl->free_endpoint_count] = index;
  ++impl->free_endpoint_count;
  --impl->endpoint_count;
  return SALTS_OK;
}

static int iocp_attach_socket(cmeta_io_impl *base, uintptr_t native_socket,
                              native_io_endpoint *out_endpoint) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  cmeta_io_resource_kind resource_kind;
  LPFN_CONNECTEX connect_ex = NULL;
  GUID connect_ex_id = WSAID_CONNECTEX;
  DWORD extension_bytes = 0u;
  SOCKADDR_STORAGE peer_address;
  int peer_address_length = (int)sizeof(peer_address);
  bool connected = false;
  int status;
  int socket_type = 0;
  int option_length = (int)sizeof(socket_type);
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (getsockopt((SOCKET)native_socket, SOL_SOCKET, SO_TYPE, (char *)&socket_type,
                 &option_length) == SOCKET_ERROR)
    return iocp_native_error((DWORD)WSAGetLastError());
  if (socket_type == SOCK_STREAM)
    resource_kind = SALTS_IO_RESOURCE_STREAM_SOCKET;
  else if (socket_type == SOCK_DGRAM)
    resource_kind = SALTS_IO_RESOURCE_DATAGRAM_SOCKET;
  else
    return SALTS_ENOTSUP;
  if (resource_kind == SALTS_IO_RESOURCE_STREAM_SOCKET &&
      WSAIoctl((SOCKET)native_socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_ex_id,
               (DWORD)sizeof(connect_ex_id), &connect_ex, (DWORD)sizeof(connect_ex),
               &extension_bytes, NULL, NULL) == SOCKET_ERROR)
    return iocp_native_error((DWORD)WSAGetLastError());
  if (resource_kind == SALTS_IO_RESOURCE_STREAM_SOCKET) {
    if (getpeername((SOCKET)native_socket, (SOCKADDR *)&peer_address, &peer_address_length) == 0)
      connected = true;
    else if (WSAGetLastError() != WSAENOTCONN)
      return iocp_native_error((DWORD)WSAGetLastError());
  }
  status = iocp_attach_endpoint(impl, native_socket, resource_kind, out_endpoint);
  if (status == SALTS_OK) {
    cmeta_iocp_endpoint_record *endpoint = iocp_endpoint(impl, *out_endpoint);
    endpoint->connect_ex = connect_ex;
    endpoint->connected = connected;
  }
  return status;
}

static int iocp_release_socket(cmeta_io_impl *base, native_io_endpoint endpoint_handle) {
  return iocp_release_endpoint((cmeta_iocp_impl *)base, endpoint_handle, true);
}

static int iocp_validate_pipe_handle(HANDLE native_handle) {
  HMODULE native_library;
  cmeta_iocp_query_file_fn query_file;
  IO_STATUS_BLOCK query_status = {0};
  cmeta_iocp_file_mode_information mode = {0};
  DWORD pipe_flags = 0u;
  NTSTATUS status;
  if (native_handle == NULL || native_handle == INVALID_HANDLE_VALUE) return SALTS_EINVAL;
  if (GetFileType(native_handle) != FILE_TYPE_PIPE) return SALTS_EINVAL;
  if (!GetNamedPipeInfo(native_handle, &pipe_flags, NULL, NULL, NULL)) {
    const DWORD pipe_info_error = GetLastError();
    if (pipe_info_error != ERROR_ACCESS_DENIED) return iocp_native_error(pipe_info_error);
  } else if ((pipe_flags & PIPE_TYPE_MESSAGE) != 0u) {
    return SALTS_EINVAL;
  }

  /* Win32 exposes no public query for the synchronous-open mode retained by a handle. */
  native_library = GetModuleHandleW(L"ntdll.dll");
  if (native_library == NULL) return SALTS_ENOTSUP;
  query_file =
      (cmeta_iocp_query_file_fn)(void *)GetProcAddress(native_library, "NtQueryInformationFile");
  if (query_file == NULL) return SALTS_ENOTSUP;
  status = query_file(native_handle, &query_status, &mode, (ULONG)sizeof(mode),
                      (FILE_INFORMATION_CLASS)SALTS_IOCP_FILE_MODE_INFORMATION_CLASS);
  if (status < 0) return SALTS_EIO;
  if ((mode.mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0u)
    return SALTS_ENOTSUP;
  return SALTS_OK;
}

static int iocp_attach_pipe(cmeta_io_impl *base, uintptr_t native_handle, uint32_t flags,
                            native_io_endpoint *out_endpoint) {
  int status;
  if (flags != NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE) return SALTS_EINVAL;
  status = iocp_validate_pipe_handle((HANDLE)native_handle);
  if (status != SALTS_OK) return status;
  return iocp_attach_endpoint((cmeta_iocp_impl *)base, native_handle, SALTS_IO_RESOURCE_BYTE_PIPE,
                              out_endpoint);
}

static int iocp_release_pipe(cmeta_io_impl *base, native_io_endpoint endpoint_handle) {
  return iocp_release_endpoint((cmeta_iocp_impl *)base, endpoint_handle, false);
}

static void iocp_release_request(cmeta_iocp_impl *impl, cmeta_iocp_request_record *request,
                                 uint32_t index) {
  cmeta_iocp_endpoint_record *endpoint = iocp_endpoint(impl, request->endpoint);
  if (endpoint != NULL && endpoint->active_requests != 0u) --endpoint->active_requests;
  if (request->accepted_socket != INVALID_SOCKET) {
    (void)closesocket(request->accepted_socket);
    request->accepted_socket = INVALID_SOCKET;
  }
  request->phase = SALTS_IOCP_RECORD_FREE;
  request->native_handle = UINTPTR_MAX;
  request->flags = 0u;
  request->vector_buffer_count = 0u;
  request->address = NULL;
  request->address_length = 0;
  request->user_data = 0u;
  request->stream_accept = false;
  impl->free_requests[impl->free_request_count] = index;
  ++impl->free_request_count;
  --impl->active_requests;
}

static int iocp_bind_connect_socket(SOCKET socket_value, const SOCKADDR *remote_address,
                                    int remote_address_length) {
  SOCKADDR_STORAGE local_address;
  SOCKADDR_STORAGE current_address;
  int local_address_length;
  int current_address_length = (int)sizeof(current_address);
  int family;

  if (remote_address == NULL || remote_address_length < (int)sizeof(remote_address->sa_family))
    return SALTS_EINVAL;
  family = remote_address->sa_family;
  memset(&local_address, 0, sizeof(local_address));
  if (family == AF_INET) {
    if (remote_address_length < (int)sizeof(SOCKADDR_IN)) return SALTS_EINVAL;
    ((SOCKADDR_IN *)&local_address)->sin_family = AF_INET;
    local_address_length = (int)sizeof(SOCKADDR_IN);
  } else if (family == AF_INET6) {
    if (remote_address_length < (int)sizeof(SOCKADDR_IN6)) return SALTS_EINVAL;
    ((SOCKADDR_IN6 *)&local_address)->sin6_family = AF_INET6;
    local_address_length = (int)sizeof(SOCKADDR_IN6);
  } else {
    return SALTS_EINVAL;
  }

  if (getsockname(socket_value, (SOCKADDR *)&current_address, &current_address_length) == 0)
    return current_address.ss_family == family ? SALTS_OK : SALTS_EINVAL;
  if (WSAGetLastError() != WSAEINVAL) return iocp_native_error((DWORD)WSAGetLastError());
  if (bind(socket_value, (const SOCKADDR *)&local_address, local_address_length) == SOCKET_ERROR)
    return iocp_native_error((DWORD)WSAGetLastError());
  return SALTS_OK;
}

static int iocp_submit(cmeta_io_impl *base, const native_io_operation *operation,
                       native_io_request *out_request) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  cmeta_iocp_endpoint_record *endpoint;
  cmeta_iocp_request_record *request;
  uint32_t index;
  uint32_t generation;
  DWORD immediate_bytes = 0u;
  int native_status;
  DWORD native_error;

  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  endpoint = iocp_endpoint(impl, operation->endpoint);
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
    iocp_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }

  index = impl->free_requests[impl->free_request_count - 1u];
  request = &impl->requests[index];
  generation = iocp_next_generation(request->request.generation);
  memset(&request->overlapped, 0, sizeof(request->overlapped));
  request->buffer.buf = (CHAR *)operation->buffer;
  request->buffer.len = (ULONG)operation->length;
  request->vector_buffer_count = 0u;
  request->phase = SALTS_IOCP_RECORD_PENDING;
  request->request = (native_io_request){index + 1u, generation};
  request->endpoint = operation->endpoint;
  request->operation_kind = operation->kind;
  request->native_handle = endpoint->native_handle;
  request->flags = 0u;
  request->address = operation->address;
  request->address_length = operation->kind == NATIVE_IO_OPERATION_UDP_RECV_FROM
                                ? (int)operation->address_capacity
                                : (int)operation->address_length;
  request->user_data = operation->user_data;
  --impl->free_request_count;
  ++endpoint->active_requests;
  ++impl->active_requests;

  if (operation->kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    const int bind_status =
        iocp_bind_connect_socket((SOCKET)request->native_handle,
                                 (const SOCKADDR *)request->address, request->address_length);
    BOOL started;
    endpoint->connect_active = true;
    if (bind_status != SALTS_OK) {
      endpoint->connect_active = false;
      iocp_release_request(impl, request, index);
      iocp_counter_increment(&impl->native_submit_errors);
      return bind_status;
    }
    if (endpoint->connect_ex == NULL) {
      endpoint->connect_active = false;
      iocp_release_request(impl, request, index);
      iocp_counter_increment(&impl->native_submit_errors);
      return SALTS_ENOTSUP;
    }
    started = endpoint->connect_ex((SOCKET)request->native_handle,
                                   (const SOCKADDR *)request->address,
                                   request->address_length, NULL, 0u, NULL,
                                   &request->overlapped);
    if (started) {
      iocp_counter_increment(&impl->submitted);
      *out_request = request->request;
      return SALTS_OK;
    }
    native_error = (DWORD)WSAGetLastError();
    if (native_error == WSA_IO_PENDING) {
      iocp_counter_increment(&impl->submitted);
      *out_request = request->request;
      return SALTS_OK;
    }
    iocp_release_request(impl, request, index);
    endpoint->connect_active = false;
    iocp_counter_increment(&impl->native_submit_errors);
    return iocp_native_error(native_error);
  }

  if (operation->kind == NATIVE_IO_OPERATION_PIPE_READ || operation->kind == NATIVE_IO_OPERATION_PIPE_WRITE) {
    BOOL started = operation->kind == NATIVE_IO_OPERATION_PIPE_READ
                       ? ReadFile((HANDLE)request->native_handle, request->buffer.buf,
                                  request->buffer.len, &immediate_bytes, &request->overlapped)
                       : WriteFile((HANDLE)request->native_handle, request->buffer.buf,
                                   request->buffer.len, &immediate_bytes, &request->overlapped);
    if (started) {
      iocp_counter_increment(&impl->submitted);
      *out_request = request->request;
      return SALTS_OK;
    }
    native_error = GetLastError();
    if (native_error == ERROR_IO_PENDING) {
      iocp_counter_increment(&impl->submitted);
      *out_request = request->request;
      return SALTS_OK;
    }
    if (operation->kind == NATIVE_IO_OPERATION_PIPE_READ &&
        (native_error == ERROR_BROKEN_PIPE || native_error == ERROR_HANDLE_EOF)) {
      /* Immediate EOF queues no OS packet. Retain the lease until observe,
       * using the same zero-byte read completion as asynchronous EOF. */
      if (PostQueuedCompletionStatus(impl->port, 0u, 0u, &request->overlapped)) {
        iocp_counter_increment(&impl->submitted);
        *out_request = request->request;
        return SALTS_OK;
      }
      native_error = GetLastError();
    }
    iocp_release_request(impl, request, index);
    iocp_counter_increment(&impl->native_submit_errors);
    return iocp_native_error(native_error);
  }

  if (operation->kind == NATIVE_IO_OPERATION_STREAM_RECV) {
    native_status = WSARecv((SOCKET)request->native_handle, &request->buffer, 1u, &immediate_bytes,
                            &request->flags, &request->overlapped, NULL);
  } else if (operation->kind == NATIVE_IO_OPERATION_STREAM_SEND) {
    native_status = WSASend((SOCKET)request->native_handle, &request->buffer, 1u, &immediate_bytes,
                            0u, &request->overlapped, NULL);
  } else if (operation->kind == NATIVE_IO_OPERATION_UDP_RECV_FROM &&
             operation->address != NULL) {
    native_status = WSARecvFrom((SOCKET)request->native_handle, &request->buffer, 1u,
                                &immediate_bytes, &request->flags, (SOCKADDR *)request->address,
                                &request->address_length, &request->overlapped, NULL);
  } else if (operation->kind == NATIVE_IO_OPERATION_UDP_SEND_TO &&
             operation->address != NULL) {
    native_status = WSASendTo((SOCKET)request->native_handle, &request->buffer, 1u,
                              &immediate_bytes, 0u, (const SOCKADDR *)request->address,
                              request->address_length, &request->overlapped, NULL);
  } else if (operation->kind == NATIVE_IO_OPERATION_UDP_RECV_FROM) {
    native_status = WSARecv((SOCKET)request->native_handle, &request->buffer, 1u,
                            &immediate_bytes, &request->flags, &request->overlapped, NULL);
  } else {
    native_status = WSASend((SOCKET)request->native_handle, &request->buffer, 1u,
                            &immediate_bytes, 0u, &request->overlapped, NULL);
  }
  if (native_status == 0) {
    iocp_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }
  native_error = (DWORD)WSAGetLastError();
  if (native_error == WSA_IO_PENDING) {
    iocp_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }
  iocp_release_request(impl, request, index);
  iocp_counter_increment(&impl->native_submit_errors);
  return iocp_native_error(native_error);
}

static int iocp_submit_vector(cmeta_io_impl *base,
                              const native_io_vector_operation *operation,
                              native_io_request *out_request) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  cmeta_iocp_endpoint_record *endpoint;
  cmeta_iocp_request_record *request;
  uint32_t index;
  uint32_t generation;
  DWORD immediate_bytes = 0u;
  int native_status;
  DWORD native_error;

  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  endpoint = iocp_endpoint(impl, operation->endpoint);
  if (endpoint == NULL) return SALTS_ENOENT;
  if (operation->kind == NATIVE_IO_OPERATION_PIPE_WRITE &&
      endpoint->resource_kind == SALTS_IO_RESOURCE_BYTE_PIPE)
    return SALTS_ENOTSUP;
  if (operation->kind != NATIVE_IO_OPERATION_STREAM_SEND ||
      endpoint->resource_kind != SALTS_IO_RESOURCE_STREAM_SOCKET)
    return SALTS_EINVAL;
  if (endpoint->connect_active) return SALTS_EBUSY;
  if (!endpoint->connected) return SALTS_EINVAL;
  if (impl->free_request_count == 0u) {
    iocp_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }

  index = impl->free_requests[impl->free_request_count - 1u];
  request = &impl->requests[index];
  generation = iocp_next_generation(request->request.generation);
  memset(&request->overlapped, 0, sizeof(request->overlapped));
  request->vector_buffer_count = (DWORD)operation->span_count;
  for (DWORD cursor = 0u; cursor < request->vector_buffer_count; ++cursor) {
    request->vector_buffers[cursor].buf = (CHAR *)operation->spans[cursor].data;
    request->vector_buffers[cursor].len = (ULONG)operation->spans[cursor].length;
  }
  request->phase = SALTS_IOCP_RECORD_PENDING;
  request->request = (native_io_request){index + 1u, generation};
  request->endpoint = operation->endpoint;
  request->operation_kind = operation->kind;
  request->native_handle = endpoint->native_handle;
  request->flags = 0u;
  request->address = NULL;
  request->address_length = 0;
  request->user_data = operation->user_data;
  --impl->free_request_count;
  ++endpoint->active_requests;
  ++impl->active_requests;

  native_status = WSASend((SOCKET)request->native_handle, request->vector_buffers,
                          request->vector_buffer_count, &immediate_bytes, 0u,
                          &request->overlapped, NULL);
  if (native_status == 0) {
    iocp_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }
  native_error = (DWORD)WSAGetLastError();
  if (native_error == WSA_IO_PENDING) {
    iocp_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }
  iocp_release_request(impl, request, index);
  iocp_counter_increment(&impl->native_submit_errors);
  return iocp_native_error(native_error);
}

static bool iocp_supports_vector_write(const cmeta_io_impl *base,
                                       native_io_endpoint endpoint_handle) {
  const cmeta_iocp_impl *impl = (const cmeta_iocp_impl *)base;
  const cmeta_iocp_endpoint_record *endpoint;
  if (!native_io_endpoint_valid(endpoint_handle) ||
      endpoint_handle.slot > impl->endpoint_capacity)
    return false;
  endpoint = &impl->endpoints[endpoint_handle.slot - 1u];
  return endpoint->active && endpoint->generation == endpoint_handle.generation &&
         endpoint->resource_kind == SALTS_IO_RESOURCE_STREAM_SOCKET;
}

static int iocp_submit_stream_accept(
    cmeta_io_impl *base, native_io_endpoint listener_handle,
    native_io_request *out_request) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  cmeta_iocp_endpoint_record *listener;
  cmeta_iocp_request_record *request;
  SOCKADDR_STORAGE local_address;
  int local_address_length = (int)sizeof(local_address);
  int accept_address_length;
  GUID accept_ex_id = WSAID_ACCEPTEX;
  DWORD extension_bytes = 0u;
  DWORD immediate_bytes = 0u;
  DWORD native_error;
  uint32_t index;
  uint32_t generation;
  BOOL started;

  if (out_request != NULL)
    *out_request = (native_io_request){0};
  if (!impl->admission_open || out_request == NULL)
    return !impl->admission_open ? SALTS_ESHUTDOWN : SALTS_EINVAL;
  listener = iocp_endpoint(impl, listener_handle);
  if (listener == NULL)
    return SALTS_ENOENT;
  if (listener->resource_kind != SALTS_IO_RESOURCE_STREAM_SOCKET ||
      listener->connected || listener->connect_active)
    return SALTS_EINVAL;
  {
    int accepting = 0;
    int option_length = (int)sizeof(accepting);
    if (getsockopt((SOCKET)listener->native_handle, SOL_SOCKET,
                   SO_ACCEPTCONN, (char *)&accepting,
                   &option_length) == SOCKET_ERROR)
      return iocp_native_error((DWORD)WSAGetLastError());
    if (!accepting)
      return SALTS_EINVAL;
  }
  if (listener->active_requests != 0u)
    return SALTS_EBUSY;
  if (impl->free_request_count == 0u) {
    iocp_counter_increment(&impl->rejected_full);
    return SALTS_ENOBUFS;
  }
  if (getsockname((SOCKET)listener->native_handle,
                  (SOCKADDR *)&local_address,
                  &local_address_length) == SOCKET_ERROR)
    return iocp_native_error((DWORD)WSAGetLastError());
  if (local_address.ss_family != AF_INET &&
      local_address.ss_family != AF_INET6)
    return SALTS_EAFNOSUPPORT;

  if (listener->accept_ex == NULL &&
      WSAIoctl((SOCKET)listener->native_handle,
               SIO_GET_EXTENSION_FUNCTION_POINTER,
               &accept_ex_id, (DWORD)sizeof(accept_ex_id),
               &listener->accept_ex,
               (DWORD)sizeof(listener->accept_ex),
               &extension_bytes, NULL, NULL) == SOCKET_ERROR)
    return iocp_native_error((DWORD)WSAGetLastError());
  if (listener->accept_ex == NULL)
    return SALTS_ENOTSUP;

  index = impl->free_requests[impl->free_request_count - 1u];
  request = &impl->requests[index];
  generation = iocp_next_generation(request->request.generation);
  memset(&request->overlapped, 0, sizeof(request->overlapped));
  request->accepted_socket = WSASocketW(
      local_address.ss_family, SOCK_STREAM, IPPROTO_TCP,
      NULL, 0u, WSA_FLAG_OVERLAPPED);
  if (request->accepted_socket == INVALID_SOCKET)
    return iocp_native_error((DWORD)WSAGetLastError());

  request->phase = SALTS_IOCP_RECORD_PENDING;
  request->request = (native_io_request){index + 1u, generation};
  request->endpoint = listener_handle;
  request->operation_kind = (native_io_operation_kind)0;
  request->native_handle = listener->native_handle;
  request->flags = 0u;
  request->address = NULL;
  request->address_length = 0;
  request->user_data = 0u;
  request->stream_accept = true;
  --impl->free_request_count;
  ++listener->active_requests;
  ++impl->active_requests;

  accept_address_length = SALTS_IOCP_ACCEPT_ADDRESS_BYTES;
  started = listener->accept_ex(
      (SOCKET)listener->native_handle,
      request->accepted_socket,
      request->accept_address, 0u,
      (DWORD)accept_address_length,
      (DWORD)accept_address_length,
      &immediate_bytes, &request->overlapped);
  if (started) {
    iocp_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }

  native_error = (DWORD)WSAGetLastError();
  if (native_error == WSA_IO_PENDING) {
    iocp_counter_increment(&impl->submitted);
    *out_request = request->request;
    return SALTS_OK;
  }

  iocp_release_request(impl, request, index);
  iocp_counter_increment(&impl->native_submit_errors);
  return iocp_native_error(native_error);
}

static int iocp_take_stream_accept(
    cmeta_io_impl *base, native_io_request request,
    uintptr_t *out_transport) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  uintptr_t token = UINTPTR_MAX;
  cmeta_iocp_accept_result *result;
  cmeta_iocp_endpoint_record *listener;
  SOCKET child;
  int status;

  if (out_transport == NULL)
    return SALTS_EINVAL;
  *out_transport = UINTPTR_MAX;
  status = native_io_accept_escrow_take(
      &impl->accept_escrow, request, &token);
  if (status != SALTS_OK)
    return status;
  result = iocp_accept_result_from_token(impl, token);
  if (result == NULL || !result->live ||
      result->request.slot != request.slot ||
      result->request.generation != request.generation)
    return SALTS_EPROTO;

  listener = iocp_endpoint(impl, result->listener);
  child = result->child;
  if (listener != NULL && listener->retained_accepts != 0u)
    --listener->retained_accepts;
  memset(result, 0, sizeof(*result));
  result->child = INVALID_SOCKET;
  if (child == INVALID_SOCKET)
    return SALTS_EPROTO;
  *out_transport = (uintptr_t)child;
  return SALTS_OK;
}

static int iocp_cancel(cmeta_io_impl *base, native_io_request request_handle) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  cmeta_iocp_request_record *request = iocp_request(impl, request_handle);
  DWORD error;
  if (request == NULL) return SALTS_ENOENT;
  if (CancelIoEx((HANDLE)request->native_handle, &request->overlapped)) return SALTS_OK;
  error = GetLastError();
  if (error == ERROR_NOT_FOUND) return SALTS_EALREADY;
  iocp_counter_increment(&impl->native_cancel_errors);
  return iocp_native_error(error);
}

static cmeta_iocp_request_record *
iocp_completed_request(cmeta_iocp_impl *impl, OVERLAPPED *overlapped, uint32_t *out_index) {
  const uintptr_t base = (uintptr_t)impl->requests;
  const uintptr_t value = (uintptr_t)overlapped;
  const size_t bytes = impl->request_capacity * sizeof(*impl->requests);
  uintptr_t offset;
  if (value < base || value - base >= bytes) return NULL;
  offset = value - base;
  if (offset % sizeof(*impl->requests) != 0u) return NULL;
  *out_index = (uint32_t)(offset / sizeof(*impl->requests));
  if (impl->requests[*out_index].phase != SALTS_IOCP_RECORD_PENDING) return NULL;
  return &impl->requests[*out_index];
}

static void iocp_make_completion(cmeta_iocp_impl *impl, cmeta_iocp_request_record *request,
                                 uint32_t request_index, DWORD bytes, DWORD native_error,
                                 native_io_completion *event) {
  int accept_status = SALTS_OK;
  if (native_error == ERROR_SUCCESS && request->stream_accept) {
    SOCKET listener_socket = (SOCKET)request->native_handle;
    if (setsockopt(request->accepted_socket, SOL_SOCKET,
                   SO_UPDATE_ACCEPT_CONTEXT,
                   (const char *)&listener_socket,
                   (int)sizeof(listener_socket)) == SOCKET_ERROR)
      native_error = (DWORD)WSAGetLastError();
    else
      accept_status = iocp_publish_accept_result(impl, request);
  }
  if (native_error == ERROR_SUCCESS &&
      request->operation_kind == NATIVE_IO_OPERATION_STREAM_CONNECT &&
      setsockopt((SOCKET)request->native_handle, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL,
                 0) == SOCKET_ERROR)
    native_error = (DWORD)WSAGetLastError();
  if (request->operation_kind == NATIVE_IO_OPERATION_STREAM_CONNECT) {
    cmeta_iocp_endpoint_record *endpoint = iocp_endpoint(impl, request->endpoint);
    if (endpoint != NULL) {
      endpoint->connect_active = false;
      endpoint->connected = native_error == ERROR_SUCCESS;
    }
  }
  *event = (native_io_completion){
      request->request, request->endpoint,      NATIVE_IO_COMPLETION_OK, (size_t)bytes,
      SALTS_OK,         (uint32_t)native_error, request->user_data,     0u};

  if (native_error == ERROR_SUCCESS &&
      request->stream_accept && accept_status != SALTS_OK) {
    event->kind = NATIVE_IO_COMPLETION_FAILED;
    event->bytes = 0u;
    event->status = accept_status;
    event->native_status = 0u;
    iocp_counter_increment(&impl->failed);
  } else if (native_error == ERROR_OPERATION_ABORTED) {
    event->kind = NATIVE_IO_COMPLETION_CANCELLED;
    event->bytes = 0u;
    event->status = SALTS_ECANCELED;
    iocp_counter_increment(&impl->cancelled);
  } else if (request->operation_kind == NATIVE_IO_OPERATION_PIPE_READ &&
             (native_error == ERROR_BROKEN_PIPE || native_error == ERROR_HANDLE_EOF)) {
    event->kind = NATIVE_IO_COMPLETION_EOF;
    event->bytes = 0u;
    event->status = SALTS_EOF;
  } else if (native_error != ERROR_SUCCESS) {
    event->kind = NATIVE_IO_COMPLETION_FAILED;
    event->bytes = 0u;
    event->status = iocp_native_error(native_error);
    iocp_counter_increment(&impl->failed);
  } else if ((request->operation_kind == NATIVE_IO_OPERATION_STREAM_RECV ||
              request->operation_kind == NATIVE_IO_OPERATION_PIPE_READ) &&
             bytes == 0u) {
    event->kind = NATIVE_IO_COMPLETION_EOF;
    event->status = SALTS_EOF;
  } else if (request->operation_kind == NATIVE_IO_OPERATION_UDP_RECV_FROM) {
    event->address_length = (size_t)request->address_length;
  }

  iocp_counter_increment(&impl->completed);
  iocp_release_request(impl, request, request_index);
}

static int iocp_observe(cmeta_io_impl *base, native_io_completion *events, size_t event_capacity,
                        uint32_t timeout_ms, size_t *out_count) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  const size_t limit = event_capacity < impl->completion_batch_capacity
                           ? event_capacity
                           : impl->completion_batch_capacity;
  size_t count;

  for (count = 0u; count < limit;) {
    DWORD bytes = 0u;
    ULONG_PTR completion_key = 0u;
    OVERLAPPED *overlapped = NULL;
    const DWORD wait_ms = count == 0u ? timeout_ms : 0u;
    const BOOL ok =
        GetQueuedCompletionStatus(impl->port, &bytes, &completion_key, &overlapped, wait_ms);
    const DWORD native_error = ok ? ERROR_SUCCESS : GetLastError();
    cmeta_iocp_request_record *request;
    uint32_t request_index = 0u;
    if (overlapped == NULL) {
      *out_count = count;
      if (ok && completion_key == SALTS_IOCP_WAKE_KEY) {
        atomic_store_explicit(&impl->wake_pending, false, memory_order_release);
        return SALTS_OK;
      }
      if (native_error == WAIT_TIMEOUT) return count == 0u ? SALTS_ETIMEDOUT : SALTS_OK;
      return iocp_native_error(native_error);
    }
    request = iocp_completed_request(impl, overlapped, &request_index);
    if (request == NULL) {
      *out_count = count;
      return SALTS_EPROTO;
    }
    iocp_make_completion(impl, request, request_index, bytes, native_error, &events[count]);
    ++count;
  }
  *out_count = count;
  return SALTS_OK;
}

static int iocp_wake(cmeta_io_impl *base) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  bool expected = false;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (!atomic_compare_exchange_strong_explicit(&impl->wake_pending, &expected, true,
                                               memory_order_acq_rel, memory_order_acquire))
    return SALTS_OK;
  if (PostQueuedCompletionStatus(impl->port, 0u, SALTS_IOCP_WAKE_KEY, NULL)) return SALTS_OK;
  atomic_store_explicit(&impl->wake_pending, false, memory_order_release);
  return iocp_native_error(GetLastError());
}

static int iocp_close(cmeta_io_impl *base) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  if (!impl->admission_open) return SALTS_EALREADY;
  impl->admission_open = false;
  return SALTS_OK;
}

static int iocp_destroy(cmeta_io_impl *base) {
  cmeta_iocp_impl *impl = (cmeta_iocp_impl *)base;
  int cleanup_status;
  int escrow_status;
  if (impl->admission_open || impl->active_requests != 0u || impl->endpoint_count != 0u)
    return SALTS_EBUSY;
  escrow_status = native_io_accept_escrow_destroy(
      &impl->accept_escrow);
  if (escrow_status != SALTS_OK)
    return escrow_status;
  if (impl->port != NULL) {
    if (!CloseHandle(impl->port)) return iocp_native_error(GetLastError());
    impl->port = NULL;
  }
  if (impl->winsock_started) {
    cleanup_status = WSACleanup();
    if (cleanup_status != 0) return iocp_native_error((DWORD)WSAGetLastError());
    impl->winsock_started = false;
  }
  free(impl->free_requests);
  free(impl->free_endpoints);
  free(impl->accept_results);
  free(impl->requests);
  free(impl->endpoints);
  free(impl);
  return SALTS_OK;
}

static bool iocp_get_stats(const cmeta_io_impl *base, native_io_backend_stats *out_stats) {
  const cmeta_iocp_impl *impl = (const cmeta_iocp_impl *)base;
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

static const cmeta_io_impl_ops iocp_ops = {
    .attach_socket = iocp_attach_socket,
    .release_socket = iocp_release_socket,
    .submit = iocp_submit,
    .cancel = iocp_cancel,
    .observe = iocp_observe,
    .wake = iocp_wake,
    .close = iocp_close,
    .destroy = iocp_destroy,
    .get_stats = iocp_get_stats,
    .attach_pipe = iocp_attach_pipe,
    .release_pipe = iocp_release_pipe,
    .prepare = iocp_submit,
    .flush = NULL,
    .submit_vector = iocp_submit_vector,
    .supports_vector_write = iocp_supports_vector_write,
    .submit_stream_accept = iocp_submit_stream_accept,
    .take_stream_accept = iocp_take_stream_accept};

bool native_io_platform_backend_supported(native_io_backend_kind kind) {
  return kind == NATIVE_IO_BACKEND_IOCP;
}

bool native_io_platform_pipe_supported(native_io_backend_kind kind) {
  return kind == NATIVE_IO_BACKEND_IOCP;
}

int native_io_platform_backend_init(native_io_backend *backend,
                                   const native_io_backend_config *config) {
  cmeta_iocp_impl *impl;
  WSADATA winsock_data;
  size_t index;
  int status;

  if (config->endpoint_capacity > SIZE_MAX / sizeof(cmeta_iocp_endpoint_record) ||
      config->request_capacity > SIZE_MAX / sizeof(cmeta_iocp_request_record) ||
      config->request_capacity > SIZE_MAX / sizeof(cmeta_iocp_accept_result) ||
      config->endpoint_capacity > SIZE_MAX / sizeof(uint32_t) ||
      config->request_capacity > SIZE_MAX / sizeof(uint32_t))
    return SALTS_ERANGE;

  impl = (cmeta_iocp_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->endpoints =
      (cmeta_iocp_endpoint_record *)calloc(config->endpoint_capacity, sizeof(*impl->endpoints));
  impl->requests =
      (cmeta_iocp_request_record *)calloc(config->request_capacity, sizeof(*impl->requests));
  impl->accept_results =
      (cmeta_iocp_accept_result *)calloc(config->request_capacity, sizeof(*impl->accept_results));
  impl->free_endpoints =
      (uint32_t *)calloc(config->endpoint_capacity, sizeof(*impl->free_endpoints));
  impl->free_requests = (uint32_t *)calloc(config->request_capacity, sizeof(*impl->free_requests));
  if (impl->endpoints == NULL || impl->requests == NULL ||
      impl->accept_results == NULL || impl->free_endpoints == NULL ||
      impl->free_requests == NULL) {
    free(impl->free_requests);
    free(impl->free_endpoints);
    free(impl->accept_results);
    free(impl->requests);
    free(impl->endpoints);
    free(impl);
    return SALTS_ENOMEM;
  }

  status = WSAStartup(MAKEWORD(2, 2), &winsock_data);
  if (status != 0) {
    free(impl->free_requests);
    free(impl->free_endpoints);
    free(impl->accept_results);
    free(impl->requests);
    free(impl->endpoints);
    free(impl);
    return -(int)status;
  }
  impl->winsock_started = true;
  impl->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0u, 1u);
  if (impl->port == NULL) {
    status = iocp_native_error(GetLastError());
    (void)WSACleanup();
    free(impl->free_requests);
    free(impl->free_endpoints);
    free(impl->accept_results);
    free(impl->requests);
    free(impl->endpoints);
    free(impl);
    return status;
  }

  impl->base.ops = &iocp_ops;
  impl->base.kind = config->kind;
  impl->endpoint_capacity = config->endpoint_capacity;
  impl->request_capacity = config->request_capacity;
  impl->completion_batch_capacity = config->completion_batch_capacity;
  impl->free_endpoint_count = config->endpoint_capacity;
  impl->free_request_count = config->request_capacity;
  impl->admission_open = true;
  status = native_io_accept_escrow_init(
      &impl->accept_escrow, config->request_capacity,
      iocp_retire_accept_result, impl);
  if (status != SALTS_OK) {
    (void)CloseHandle(impl->port);
    (void)WSACleanup();
    free(impl->free_requests);
    free(impl->free_endpoints);
    free(impl->accept_results);
    free(impl->requests);
    free(impl->endpoints);
    free(impl);
    return status;
  }
  atomic_init(&impl->wake_pending, false);
  for (index = 0u; index < config->endpoint_capacity; ++index) {
    impl->free_endpoints[index] = (uint32_t)(config->endpoint_capacity - index - 1u);
    impl->endpoints[index].native_handle = UINTPTR_MAX;
  }
  for (index = 0u; index < config->request_capacity; ++index) {
    impl->free_requests[index] = (uint32_t)(config->request_capacity - index - 1u);
    impl->requests[index].native_handle = UINTPTR_MAX;
    impl->requests[index].accepted_socket = INVALID_SOCKET;
    impl->accept_results[index].child = INVALID_SOCKET;
  }
  backend->impl = impl;
  return SALTS_OK;
}
