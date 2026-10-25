#include "cnet_transport.h"
#include <salts/native_ipc.h>
#include <string.h>
#include <stdlib.h>

#if !defined(_WIN32)
  #include <errno.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

int cnet_transport_ipc_validate_name(const char *name, size_t length) {
  if (name == NULL || length == 0u) return SALTS_EINVAL;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = (unsigned char)name[i];
    if (c <= 0x20u || c == 0x7fu || c == '?' || c == '#' || c == '%') return SALTS_EINVAL;
#if defined(_WIN32)
    if (c == '/' || c == '\\' || c == ':') return SALTS_EINVAL;
#endif
  }
#if defined(_WIN32)
  if (length >= 256u - (sizeof("\\\\.\\pipe\\") - 1u)) return SALTS_ERANGE;
#else
  if (name[0] != '/') return SALTS_EINVAL;
  if (length >= sizeof(((struct sockaddr_un *)0)->sun_path)) return SALTS_ERANGE;
#endif
  return SALTS_OK;
}

/* Private Windows/POSIX native name construction.
 * The caller owns *out_name and must free it; no SDS/tstr dependency. */
int cnet_transport_ipc_native_name(const char *name, char **out_name) {
  size_t name_size;
  size_t prefix_size;
  char *owned;
#if defined(_WIN32)
  static const char prefix[] = "\\\\.\\pipe\\";
#else
  static const char prefix[] = "";
#endif
  if (name == NULL || out_name == NULL) return SALTS_EINVAL;
  *out_name = NULL;
  name_size = strlen(name);
  const int status = cnet_transport_ipc_validate_name(name, name_size);
  if (status != SALTS_OK) return status;
  prefix_size = sizeof(prefix) - 1u;
  if (name_size > SIZE_MAX - prefix_size - 1u) return SALTS_ERANGE;
  owned = (char *)malloc(prefix_size + name_size + 1u);
  if (owned == NULL) return SALTS_ENOMEM;
  memcpy(owned, prefix, prefix_size);
  memcpy(owned + prefix_size, name, name_size + 1u);
  *out_name = owned;
  return SALTS_OK;
}

int cnet_transport_ipc_address(const char *name, void *address, size_t capacity,
                               size_t *out_length) {
  if (out_length == NULL) return SALTS_EINVAL;
  *out_length = 0u;
#if defined(_WIN32)
  (void)name;
  (void)address;
  (void)capacity;
  return SALTS_ENOTSUP;
#else
  struct sockaddr_un value;
  size_t length;
  int status;
  if (name == NULL || address == NULL) return SALTS_EINVAL;
  length = strlen(name);
  status = cnet_transport_ipc_validate_name(name, length);
  if (status != SALTS_OK) return status;
  length += offsetof(struct sockaddr_un, sun_path) + 1u;
  if (capacity < length) return SALTS_ERANGE;
  memset(&value, 0, sizeof(value));
  value.sun_family = AF_UNIX;
  #if defined(__APPLE__) || defined(__FreeBSD__)
  value.sun_len = (unsigned char)length;
  #endif
  memcpy(value.sun_path, name, strlen(name) + 1u);
  memcpy(address, &value, length);
  *out_length = length;
  return SALTS_OK;
#endif
}

int cnet_transport_close_ipc(uintptr_t native_handle) {
#if defined(_WIN32)
  cmeta_ipc_pipe_endpoint endpoint = {native_handle, NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE};
  return cmeta_ipc_pipe_endpoint_close(&endpoint);
#else
  return close((int)native_handle) == 0 ? SALTS_OK : -errno;
#endif
}

int cnet_transport_adopt_ipc(cnet_transport *transport, native_io_backend *backend,
                             uintptr_t native_handle) {
#if defined(_WIN32)
  const int status = cnet_transport_adopt_pipe(transport, backend, native_handle, native_handle);
  if (status != SALTS_OK) (void)cnet_transport_close_ipc(native_handle);
  return status;
#else
  const cnet_stream_socket_options options = CNET_STREAM_SOCKET_OPTIONS_INIT;
  return cnet_transport_adopt_stream(transport, backend, native_handle, false, &options);
#endif
}

int cnet_transport_ipc_prepare_connect(cnet_transport *transport, native_io_backend *backend,
                                       native_io_backend_kind backend_kind, const char *name,
                                       void *address, size_t address_length,
                                       native_io_operation *out_operation, bool *out_immediate) {
  if (out_immediate == NULL) return SALTS_EINVAL;
  *out_immediate = false;
#if defined(_WIN32)
  char *native_name = NULL;
  cmeta_ipc_pipe_endpoint endpoint;
  int status;
  (void)address;
  (void)address_length;
  (void)out_operation;
  if (backend_kind != NATIVE_IO_BACKEND_IOCP) return SALTS_ENOTSUP;
  status = cnet_transport_ipc_native_name(name, &native_name);
  if (status != SALTS_OK) return status;
  cmeta_ipc_pipe_endpoint_init(&endpoint);
  status = cmeta_ipc_named_pipe_connect(native_name, SALTS_IPC_PIPE_DUPLEX, &endpoint);
  free(native_name);
  *out_immediate = true;
  if (status != SALTS_OK) return status;
  return cnet_transport_adopt_ipc(transport, backend, endpoint.handle);
#else
  const cnet_stream_socket_options options = CNET_STREAM_SOCKET_OPTIONS_INIT;
  (void)name;
  return cnet_transport_stream_prepare_connect(transport, backend, backend_kind, AF_UNIX, 0, false,
                                               address, address_length, &options, 0u,
                                               out_operation);
#endif
}
