#ifndef CNET_TRANSPORT_H
#define CNET_TRANSPORT_H

#include <cnet/cnet.h>
#include <salts/native_io.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum cnet_transport_resource_kind {
  CNET_TRANSPORT_RESOURCE_NONE = 0,
  CNET_TRANSPORT_RESOURCE_SOCKET,
  CNET_TRANSPORT_RESOURCE_PIPE
} cnet_transport_resource_kind;

typedef struct cnet_transport {
  uintptr_t native_handle;
  native_io_endpoint endpoint;
  uintptr_t write_native_handle;
  native_io_endpoint write_endpoint;
  cnet_transport_resource_kind resource_kind;
  bool native_open;
  bool attached;
  bool write_native_open;
  bool write_attached;
} cnet_transport;

/** Applies one validated policy to an unattached TCP socket. */
int cnet_transport_apply_stream_socket_options(
    uintptr_t native_socket, const cnet_stream_socket_options *options);

/**
 * Converts an IPv4 or IPv6 literal plus host-order port into native sockaddr
 * storage. Returns `SALTS_ENOENT` for a hostname so the caller can route it to
 * the resolver. `out_address_length` is cleared on every failure.
 */
int cnet_transport_parse_numeric_address(const char *host, uint16_t port, void *out_address,
                                         size_t address_capacity, size_t *out_address_length);

/** Validates one IPv6 literal without consulting DNS or creating a socket. */
int cnet_transport_validate_ipv6_literal(const char *host);

/** Listener variant that additionally accepts port zero for ephemeral bind. */
int cnet_transport_parse_bind_address(const char *host, uint16_t port, void *out_address,
                                      size_t address_capacity, size_t *out_address_length);

/** Converts one portable TCP peer to native sockaddr storage. */
int cnet_transport_stream_peer_address(const cnet_stream_peer *peer, bool allow_zero_port,
                                       void *out_address, size_t address_capacity,
                                       size_t *out_address_length);

int cnet_transport_stream_endpoint_address(
    const cnet_stream_endpoint *endpoint, bool allow_zero_port,
    void *out_address, size_t address_capacity,
    size_t *out_address_length);

/** Creates and attaches a TCP socket, then describes its connect operation. */
int cnet_transport_tcp_prepare_connect(cnet_transport *transport, native_io_backend *backend,
                                       native_io_backend_kind backend_kind, const void *address,
                                       size_t address_length,
                                       const cnet_stream_socket_options *socket_options,
                                       uintptr_t user_data,
                                       native_io_operation *out_operation);

/**
 * TCP connect variant that binds an optional numeric local endpoint before
 * attaching/submitting the asynchronous connect.
 */
int cnet_transport_tcp_prepare_connect_bound(
    cnet_transport *transport, native_io_backend *backend,
    native_io_backend_kind backend_kind, const void *remote_address,
    size_t remote_address_length, const void *local_address,
    size_t local_address_length,
    const cnet_stream_socket_options *socket_options,
    uintptr_t user_data, native_io_operation *out_operation);

/** Returns whether this build can drive Linux AF_VSOCK on the selected backend. */
bool cnet_transport_vsock_supported(native_io_backend_kind backend_kind);

/** Converts host-order CID/port values into copied native VSOCK address storage. */
int cnet_transport_parse_vsock_address(uint32_t cid, uint32_t port, bool allow_any,
                                       void *out_address, size_t address_capacity,
                                       size_t *out_address_length);

/** Creates and attaches a VSOCK stream, then describes its connect operation. */
int cnet_transport_vsock_prepare_connect(cnet_transport *transport, native_io_backend *backend,
                                         native_io_backend_kind backend_kind, const void *address,
                                         size_t address_length,
                                         const cnet_stream_socket_options *socket_options,
                                         uintptr_t user_data,
                                         native_io_operation *out_operation);

/** Takes ownership of one connected AF_VSOCK SOCK_STREAM socket. */
int cnet_transport_adopt_vsock(cnet_transport *transport, native_io_backend *backend,
                               uintptr_t native_socket,
                               const cnet_stream_socket_options *socket_options);

/** Internal socket-family-neutral stream connect implementation. */
int cnet_transport_stream_prepare_connect(cnet_transport *transport, native_io_backend *backend,
                                          native_io_backend_kind backend_kind, int family,
                                          int protocol, bool socket_options_supported,
                                          const void *address, size_t address_length,
                                          const cnet_stream_socket_options *socket_options,
                                          uintptr_t user_data,
                                          native_io_operation *out_operation);

/** Internal connected stream adoption implementation. */
int cnet_transport_adopt_stream(cnet_transport *transport, native_io_backend *backend,
                                uintptr_t native_socket, bool socket_options_supported,
                                const cnet_stream_socket_options *socket_options);

/**
 * Creates a CNet-owned stream socket, attaches it to NativeIO, and submits one
 * asynchronous connect. `address` stays borrowed until the request completion.
 */
int cnet_transport_tcp_connect(cnet_transport *transport, native_io_backend *backend,
                               native_io_backend_kind backend_kind, const void *address,
                               size_t address_length, uintptr_t user_data,
                               native_io_request *out_request);

/**
 * Takes ownership of one connected TCP socket and attaches it to NativeIO.
 * The handle is closed on every failure after argument validation.
 */
int cnet_transport_adopt_tcp(cnet_transport *transport, native_io_backend *backend,
                             uintptr_t native_socket,
                             const cnet_stream_socket_options *socket_options);

int cnet_transport_adopt_tcp_prepare_connect(
    cnet_transport *transport, native_io_backend *backend,
    uintptr_t native_socket, const void *remote_address,
    size_t remote_address_length,
    const cnet_stream_socket_options *socket_options,
    uintptr_t user_data, native_io_operation *out_operation);

/** Closes one unattached native socket transferred through a failed command. */
void cnet_transport_close_socket(uintptr_t native_socket);

/** Creates, connects, and attaches one CNet-owned datagram socket. */
int cnet_transport_udp_connect(cnet_transport *transport, native_io_backend *backend,
                               native_io_backend_kind backend_kind, const void *address,
                               size_t address_length);

/**
 * Adopts one connected byte-pipe connection. On success CNet owns both native
 * handles; the read and write handles may be identical for a duplex pipe. On
 * failure ownership remains with the caller.
 */
int cnet_transport_adopt_pipe(cnet_transport *transport, native_io_backend *backend,
                              uintptr_t read_handle, uintptr_t write_handle);

/**
 * Opens one platform byte-pipe client endpoint and attaches it to NativeIO.
 * Windows maps `name` to one duplex overlapped named pipe. POSIX maps `name`
 * to the nonblocking FIFO pair `name.rx` (read) and `name.tx` (write).
 */
int cnet_transport_pipe_connect(cnet_transport *transport, native_io_backend *backend,
                                native_io_backend_kind backend_kind, const char *name);

int cnet_transport_ipc_validate_name(const char *name, size_t length);
int cnet_transport_ipc_native_name(const char *name, char **out_name);
int cnet_transport_ipc_address(const char *name, void *address, size_t capacity, size_t *out_length);
int cnet_transport_ipc_prepare_connect(cnet_transport *transport, native_io_backend *backend,
                                        native_io_backend_kind backend_kind, const char *name,
                                        void *address, size_t address_length,
                                        native_io_operation *out_operation, bool *out_immediate);
int cnet_transport_adopt_ipc(cnet_transport *transport, native_io_backend *backend,
                              uintptr_t native_handle);
int cnet_transport_close_ipc(uintptr_t native_handle);

native_io_endpoint cnet_transport_read_endpoint(const cnet_transport *transport);
native_io_endpoint cnet_transport_write_endpoint(const cnet_transport *transport);

/** Copies portable endpoints for an attached/open TCP stream transport. */
int cnet_transport_tcp_local_peer(const cnet_transport *transport, cnet_stream_peer *out_peer);
int cnet_transport_tcp_remote_peer(const cnet_transport *transport, cnet_stream_peer *out_peer);

int cnet_transport_tcp_local_endpoint(
    const cnet_transport *transport,
    cnet_stream_endpoint *out_endpoint);
int cnet_transport_tcp_remote_endpoint(
    const cnet_transport *transport,
    cnet_stream_endpoint *out_endpoint);

int cnet_transport_tcp_option_get(const cnet_transport *transport,
                                  cnet_tcp_socket_option option,
                                  uint64_t *out_value);
int cnet_transport_tcp_option_set(cnet_transport *transport,
                                  cnet_tcp_socket_option option,
                                  uint64_t value);

/** Internal raw-owner helpers; native handles never cross the public CNet ABI. */
int cnet_transport_tcp_native_option_get(uintptr_t native_socket,
                                         cnet_tcp_socket_option option,
                                         uint64_t *out_value);
int cnet_transport_tcp_native_option_set(uintptr_t native_socket,
                                         cnet_tcp_socket_option option,
                                         uint64_t value);
/* Family-aware variants are for unbound internal TCP owners where
 * getsockname() cannot portably recover the socket family yet. */
int cnet_transport_tcp_native_option_get_family(
    uintptr_t native_socket, int native_family,
    cnet_tcp_socket_option option, uint64_t *out_value);
int cnet_transport_tcp_native_option_set_family(
    uintptr_t native_socket, int native_family,
    cnet_tcp_socket_option option, uint64_t value);

int cnet_transport_tcp_shutdown(cnet_transport *transport,
                                cnet_tcp_shutdown how);

bool cnet_transport_active(const cnet_transport *transport);

/** Closes owned native resources, then releases drained NativeIO metadata. */
int cnet_transport_close(cnet_transport *transport, native_io_backend *backend);

#endif /* CNET_TRANSPORT_H */
