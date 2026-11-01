#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cflow/io_native.h>

#include <clock.h>
#include <error_codes.h>
#include <thread.h>

#include "tinytest.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "io_native_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <wchar.h>
typedef SOCKET native_test_socket;
#define NATIVE_TEST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int native_test_socket;
#define NATIVE_TEST_INVALID_SOCKET (-1)
#endif

static const uint64_t NATIVE_TEST_TIMEOUT_NS = UINT64_C(5000000000);

enum {
    NATIVE_TEST_CAPACITY = 4,
    NATIVE_TEST_PAYLOAD_CAPACITY = 64,
    NATIVE_TEST_LISTEN_BACKLOG = 4,
    NATIVE_TEST_CANCEL_REUSE_ITERATIONS = 32,
    NATIVE_TEST_CANCEL_SETTLE_YIELDS = 64
};

typedef struct native_test_operation {
    cflow_io_native_operation native;
    int released;
} native_test_operation;

typedef struct native_test_vector_operation {
    cflow_io_native_vector_operation native;
    int released;
} native_test_vector_operation;

typedef struct native_test_file_operation {
    cflow_io_native_file_operation native;
    int released;
} native_test_file_operation;

typedef struct native_completion_probe {
    cflow_io_request_id ids[NATIVE_TEST_CAPACITY];
    cflow_io_completion values[NATIVE_TEST_CAPACITY];
    size_t count;
} native_completion_probe;

typedef struct native_fixture {
    cflow_io_native_backend backend;
    cflow_executor executor;
    cflow_io_actor actor;
    native_completion_probe completions;
} native_fixture;

static void native_test_close_socket(native_test_socket socket_value) {
    if (socket_value == NATIVE_TEST_INVALID_SOCKET)
        return;
#if defined(_WIN32)
    (void)closesocket(socket_value);
#else
    (void)close(socket_value);
#endif
}

static int native_test_last_socket_error(void) {
#if defined(_WIN32)
    return -WSAGetLastError();
#else
    return -errno;
#endif
}

static int native_test_set_nonblocking(native_test_socket socket_value) {
#if defined(_WIN32)
    u_long enabled = 1u;
    return ioctlsocket(socket_value, FIONBIO, &enabled) == 0
               ? SALTS_OK : native_test_last_socket_error();
#else
    int flags;
    do {
        flags = fcntl(socket_value, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0)
        return -errno;
    while (fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    return SALTS_OK;
#endif
}

#if !defined(_WIN32)
static int native_test_status_flags(native_test_socket socket_value,
                                    int *flags) {
    int value;
    do {
        value = fcntl(socket_value, F_GETFL);
    } while (value < 0 && errno == EINTR);
    if (value < 0)
        return -errno;
    *flags = value;
    return SALTS_OK;
}

static int native_test_set_blocking(native_test_socket socket_value) {
    int flags = 0;
    int status = native_test_status_flags(socket_value, &flags);
    if (status != SALTS_OK)
        return status;
    while (fcntl(socket_value, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    return SALTS_OK;
}
#endif

static int native_test_bind_loopback(native_test_socket socket_value,
                                     struct sockaddr_in *address) {
    int address_length = (int)sizeof(*address);
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address->sin_port = 0;
    if (bind(socket_value, (const struct sockaddr *)address,
             (int)sizeof(*address)) != 0)
        return native_test_last_socket_error();
    if (getsockname(socket_value, (struct sockaddr *)address,
#if defined(_WIN32)
                    &address_length
#else
                    (socklen_t *)&address_length
#endif
                    ) != 0)
        return native_test_last_socket_error();
    return SALTS_OK;
}

static int native_test_make_tcp_pair(native_test_socket sockets[2]) {
    native_test_socket listener = NATIVE_TEST_INVALID_SOCKET;
    struct sockaddr_in address;
    int status;
    sockets[0] = NATIVE_TEST_INVALID_SOCKET;
    sockets[1] = NATIVE_TEST_INVALID_SOCKET;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == NATIVE_TEST_INVALID_SOCKET)
        return native_test_last_socket_error();
    status = native_test_bind_loopback(listener, &address);
    if (status == SALTS_OK && listen(listener, 1) != 0)
        status = native_test_last_socket_error();
    if (status == SALTS_OK) {
        sockets[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sockets[0] == NATIVE_TEST_INVALID_SOCKET)
            status = native_test_last_socket_error();
    }
    if (status == SALTS_OK &&
        connect(sockets[0], (const struct sockaddr *)&address,
                (int)sizeof(address)) != 0)
        status = native_test_last_socket_error();
    if (status == SALTS_OK) {
        sockets[1] = accept(listener, NULL, NULL);
        if (sockets[1] == NATIVE_TEST_INVALID_SOCKET)
            status = native_test_last_socket_error();
    }
    native_test_close_socket(listener);
    if (status == SALTS_OK)
        status = native_test_set_nonblocking(sockets[0]);
    if (status == SALTS_OK)
        status = native_test_set_nonblocking(sockets[1]);
    if (status != SALTS_OK) {
        native_test_close_socket(sockets[0]);
        native_test_close_socket(sockets[1]);
        sockets[0] = NATIVE_TEST_INVALID_SOCKET;
        sockets[1] = NATIVE_TEST_INVALID_SOCKET;
    }
    return status;
}

static int native_test_make_tcp_listener(
    native_test_socket *listener, native_test_socket *client,
    struct sockaddr_in *address) {
    int status;
    *listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    *client = NATIVE_TEST_INVALID_SOCKET;
    if (*listener == NATIVE_TEST_INVALID_SOCKET)
        return native_test_last_socket_error();
    status = native_test_bind_loopback(*listener, address);
    if (status == SALTS_OK &&
        listen(*listener, NATIVE_TEST_LISTEN_BACKLOG) != 0)
        status = native_test_last_socket_error();
    if (status == SALTS_OK)
        status = native_test_set_nonblocking(*listener);
    if (status == SALTS_OK) {
        *client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (*client == NATIVE_TEST_INVALID_SOCKET)
            status = native_test_last_socket_error();
    }
    if (status == SALTS_OK)
        status = native_test_set_nonblocking(*client);
    if (status != SALTS_OK) {
        native_test_close_socket(*client);
        native_test_close_socket(*listener);
        *client = NATIVE_TEST_INVALID_SOCKET;
        *listener = NATIVE_TEST_INVALID_SOCKET;
    }
    return status;
}

static int native_test_start_connect(native_test_socket socket_value,
                                     const struct sockaddr_in *address) {
    if (connect(socket_value, (const struct sockaddr *)address,
                (int)sizeof(*address)) == 0)
        return SALTS_OK;
#if defined(_WIN32)
    {
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS
                   ? SALTS_OK : -error;
    }
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK ||
                   errno == EALREADY
               ? SALTS_OK : -errno;
#endif
}

static bool native_test_socket_is_nonblocking(
    native_test_socket socket_value) {
#if defined(_WIN32)
    unsigned char byte = 0u;
    const int result = recv(socket_value, (char *)&byte, 1, 0);
    return result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
#else
    int flags;
    do {
        flags = fcntl(socket_value, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    return flags >= 0 && (flags & O_NONBLOCK) != 0;
#endif
}

static int native_test_receive_exact(native_test_socket socket_value,
                                     void *buffer, size_t length) {
    unsigned char *bytes = (unsigned char *)buffer;
    const uint64_t started = cmeta_hrtime();
    size_t received = 0u;
    while (received < length) {
        const size_t remaining = length - received;
        const int chunk = remaining > (size_t)INT_MAX
                              ? INT_MAX : (int)remaining;
        const int result = recv(socket_value, (char *)&bytes[received],
                                chunk, 0);
        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result == 0)
            return SALTS_EOF;
#if defined(_WIN32)
        {
            const int error = WSAGetLastError();
            if (error != WSAEINTR && error != WSAEWOULDBLOCK)
                return -error;
        }
#else
        {
            const int error = errno;
            if (error != EINTR && error != EAGAIN &&
                error != EWOULDBLOCK)
                return -error;
        }
#endif
        if (cmeta_hrtime() - started >= NATIVE_TEST_TIMEOUT_NS)
            return SALTS_ETIMEDOUT;
        cmeta_thread_yield();
    }
    return SALTS_OK;
}

static int native_test_make_udp_pair(native_test_socket sockets[2],
                                     struct sockaddr_in addresses[2]) {
    int status = SALTS_OK;
    sockets[0] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockets[1] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockets[0] == NATIVE_TEST_INVALID_SOCKET ||
        sockets[1] == NATIVE_TEST_INVALID_SOCKET)
        status = native_test_last_socket_error();
    if (status == SALTS_OK)
        status = native_test_bind_loopback(sockets[0], &addresses[0]);
    if (status == SALTS_OK)
        status = native_test_bind_loopback(sockets[1], &addresses[1]);
    if (status == SALTS_OK)
        status = native_test_set_nonblocking(sockets[0]);
    if (status == SALTS_OK)
        status = native_test_set_nonblocking(sockets[1]);
    if (status != SALTS_OK) {
        native_test_close_socket(sockets[0]);
        native_test_close_socket(sockets[1]);
        sockets[0] = NATIVE_TEST_INVALID_SOCKET;
        sockets[1] = NATIVE_TEST_INVALID_SOCKET;
    }
    return status;
}

static void native_operation_release(void *user) {
    native_test_operation *operation = (native_test_operation *)user;
    ++operation->released;
}

static void native_vector_operation_release(void *user) {
    native_test_vector_operation *operation =
        (native_test_vector_operation *)user;
    ++operation->released;
}

static void native_file_operation_release(void *user) {
    native_test_file_operation *operation =
        (native_test_file_operation *)user;
    ++operation->released;
}

static void native_completion_record(
    void *user, cflow_io_request_id request_id,
    cflow_io_lease_id lease_id, void *operation_user,
    const cflow_io_completion *completion) {
    native_completion_probe *probe = (native_completion_probe *)user;
    (void)lease_id;
    (void)operation_user;
    if (probe->count < NATIVE_TEST_CAPACITY) {
        probe->ids[probe->count] = request_id;
        probe->values[probe->count] = *completion;
        ++probe->count;
    }
}

static int native_fixture_init_with_ops(
    native_fixture *fixture, cflow_io_native_backend_kind kind,
    size_t capacity, cflow_io_backend_ops backend_ops) {
    cflow_io_native_backend_config backend_config = {
        kind, capacity, capacity};
    cflow_io_actor_config actor_config;
    int status;
    memset(fixture, 0, sizeof(*fixture));
    status = cflow_io_native_backend_init(&fixture->backend, &backend_config);
    if (status != SALTS_OK)
        return status;
    if (!cflow_executor_manual_init_with_capacity(&fixture->executor,
                                                   capacity)) {
        (void)cflow_io_native_backend_shutdown(&fixture->backend);
        (void)cflow_io_native_backend_destroy(&fixture->backend);
        return SALTS_ENOMEM;
    }
    memset(&actor_config, 0, sizeof(actor_config));
    actor_config.request_capacity = capacity;
    actor_config.command_capacity = capacity;
    actor_config.executor = &fixture->executor;
    actor_config.backend = backend_ops;
    actor_config.backend_user = &fixture->backend;
    actor_config.completion = native_completion_record;
    actor_config.completion_user = &fixture->completions;
    status = cflow_io_actor_init(&fixture->actor, &actor_config);
    if (status != SALTS_OK) {
        cflow_executor_destroy(&fixture->executor);
        (void)cflow_io_native_backend_shutdown(&fixture->backend);
        (void)cflow_io_native_backend_destroy(&fixture->backend);
    }
    return status;
}

static int native_fixture_init(native_fixture *fixture,
                               cflow_io_native_backend_kind kind,
                               size_t capacity) {
    return native_fixture_init_with_ops(
        fixture, kind, capacity, cflow_io_native_backend_actor_ops());
}

static int native_vector_fixture_init(
    native_fixture *fixture, cflow_io_native_backend_kind kind,
    size_t capacity) {
    return native_fixture_init_with_ops(
        fixture, kind, capacity,
        cflow_io_native_backend_vector_actor_ops());
}

static int native_file_fixture_init(native_fixture *fixture,
                                    cflow_io_native_backend_kind kind,
                                    size_t capacity) {
    return native_fixture_init_with_ops(
        fixture, kind, capacity,
        cflow_io_native_backend_file_actor_ops());
}

static int native_fixture_wait(native_fixture *fixture, size_t count) {
    const uint64_t started = cmeta_hrtime();
    while (fixture->completions.count < count) {
        (void)cflow_io_actor_run_ready(&fixture->actor, 64u);
        (void)cflow_executor_run_ready(&fixture->executor);
        if (cmeta_hrtime() - started >= NATIVE_TEST_TIMEOUT_NS)
            return SALTS_ETIMEDOUT;
        cmeta_thread_yield();
    }
    return SALTS_OK;
}

static int native_fixture_wait_native_submitted(native_fixture *fixture,
                                                uint64_t count) {
    const uint64_t started = cmeta_hrtime();
    cflow_io_native_backend_stats stats;
    do {
        (void)cflow_io_actor_run_ready(&fixture->actor, 64u);
        (void)cflow_executor_run_ready(&fixture->executor);
        if (cflow_io_native_backend_get_stats(&fixture->backend, &stats) &&
            stats.submitted >= count)
            return SALTS_OK;
        cmeta_thread_yield();
    } while (cmeta_hrtime() - started < NATIVE_TEST_TIMEOUT_NS);
    return SALTS_ETIMEDOUT;
}

static int native_fixture_forget_socket(
    native_fixture *fixture, uintptr_t socket_identity) {
    const uint64_t started = cmeta_hrtime();
    int status;
    do {
        status = cflow_io_native_backend_forget_socket(
            &fixture->backend, socket_identity);
        if (status != SALTS_EBUSY)
            return status;
        cmeta_thread_yield();
    } while (cmeta_hrtime() - started < NATIVE_TEST_TIMEOUT_NS);
    return SALTS_ETIMEDOUT;
}

static int native_fixture_forget_file(
    native_fixture *fixture, uintptr_t file_identity) {
    const uint64_t started = cmeta_hrtime();
    int status;
    do {
        status = cflow_io_native_backend_forget_file(
            &fixture->backend, file_identity);
        if (status != SALTS_EBUSY)
            return status;
        cmeta_thread_yield();
    } while (cmeta_hrtime() - started < NATIVE_TEST_TIMEOUT_NS);
    return SALTS_ETIMEDOUT;
}

static void native_fixture_destroy(native_fixture *fixture) {
    const int close_status = cflow_io_actor_close(&fixture->actor);
    check_true(close_status == SALTS_OK || close_status == SALTS_EALREADY);
    (void)cflow_io_actor_run_ready(&fixture->actor, 64u);
    check_true(cflow_io_actor_is_quiescent(&fixture->actor));
    check_equal(cflow_io_actor_destroy(&fixture->actor), SALTS_OK);
    check_equal(cflow_io_native_backend_shutdown(&fixture->backend), SALTS_OK);
    check_equal(cflow_io_native_backend_destroy(&fixture->backend), SALTS_OK);
    check_true(cflow_executor_shutdown(&fixture->executor));
    cflow_executor_destroy(&fixture->executor);
}

static cflow_io_submit_result native_submit(
    native_fixture *fixture, cflow_io_lease_id lease,
    native_test_operation *operation) {
    cflow_io_operation actor_operation = {
        operation, native_operation_release};
    return cflow_io_actor_try_submit(
        &fixture->actor, lease, &actor_operation);
}

static cflow_io_submit_result native_vector_submit(
    native_fixture *fixture, cflow_io_lease_id lease,
    native_test_vector_operation *operation) {
    cflow_io_operation actor_operation = {
        operation, native_vector_operation_release};
    return cflow_io_actor_try_submit(
        &fixture->actor, lease, &actor_operation);
}

static cflow_io_submit_result native_file_submit(
    native_fixture *fixture, cflow_io_lease_id lease,
    native_test_file_operation *operation) {
    cflow_io_operation actor_operation = {
        operation, native_file_operation_release};
    return cflow_io_actor_try_submit(
        &fixture->actor, lease, &actor_operation);
}

#if defined(_WIN32)
static int native_test_open_overlapped_file(char **out_path,
                                            HANDLE *out_file) {
    char *path = tt_make_temp_file("cflow-native-file-", ".bin");
    HANDLE file;
    DWORD error;
    if (path == NULL)
        return SALTS_ENOMEM;
    file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OVERLAPPED,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        (void)tt_remove_file(path);
        free(path);
        return -(int)error;
    }
    *out_path = path;
    *out_file = file;
    return SALTS_OK;
}

static void native_test_remove_file(char *path, HANDLE file) {
    if (file != NULL && file != INVALID_HANDLE_VALUE)
        (void)CloseHandle(file);
    if (path != NULL) {
        check_equal(tt_remove_file(path), 0);
        free(path);
    }
}

static void native_check_file_read_write_iocp(void) {
    static const unsigned char payload[] = {0x6eu, 0x61u, 0x74u,
                                             0x69u, 0x76u, 0x65u};
    native_fixture fixture;
    native_test_file_operation write_operation = {0};
    native_test_file_operation read_operation = {0};
    unsigned char received[sizeof(payload)] = {0};
    cflow_io_submit_result submitted;
    char *path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;

    check_equal(native_file_fixture_init(
                    &fixture, CFLOW_IO_NATIVE_IOCP, 2u), SALTS_OK);
    check_equal(native_test_open_overlapped_file(&path, &file), SALTS_OK);
    write_operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
        .handle = (uintptr_t)file,
        .buffer = (void *)payload,
        .length = sizeof(payload),
        .offset = 5u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 110u, &write_operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[0].bytes, sizeof(payload));
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(write_operation.released, 1);

    read_operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)file,
        .buffer = received,
        .length = sizeof(received),
        .offset = 5u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 111u, &read_operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[1].bytes, sizeof(payload));
    check_equal(received, payload, sizeof(payload));
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(read_operation.released, 1);

    check_true(CloseHandle(file));
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)file), SALTS_OK);
    file = INVALID_HANDLE_VALUE;
    native_fixture_destroy(&fixture);
    native_test_remove_file(path, file);
}

static void native_check_file_eof_iocp(void) {
    static const unsigned char payload[] = {0x45u, 0x4fu, 0x46u, 0x21u};
    native_fixture fixture;
    native_test_file_operation operation = {0};
    unsigned char received[sizeof(payload)] = {0};
    cflow_io_submit_result submitted;
    char *path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;

    check_equal(native_file_fixture_init(
                    &fixture, CFLOW_IO_NATIVE_IOCP, 1u), SALTS_OK);
    check_equal(native_test_open_overlapped_file(&path, &file), SALTS_OK);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
        .handle = (uintptr_t)file,
        .buffer = (void *)payload,
        .length = sizeof(payload),
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 112u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)file,
        .buffer = received,
        .length = sizeof(received),
        .offset = 2u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 113u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[1].bytes, 2u);
    check_equal(received[0], payload[2]);
    check_equal(received[1], payload[3]);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native.offset = sizeof(payload);
    submitted = native_file_submit(&fixture, 114u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 3u), SALTS_OK);
    check_equal(fixture.completions.values[2].kind,
                CFLOW_IO_COMPLETION_EOF);
    check_equal(fixture.completions.values[2].bytes, 0u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(operation.released, 3);

    check_true(CloseHandle(file));
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)file), SALTS_OK);
    file = INVALID_HANDLE_VALUE;
    native_fixture_destroy(&fixture);
    native_test_remove_file(path, file);
}

static void native_check_file_rejections_iocp(void) {
    native_fixture fixture;
    native_test_file_operation operation = {0};
    unsigned char byte = 0u;
    cflow_io_submit_result submitted;
    char *path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE event;

    check_equal(native_file_fixture_init(
                    &fixture, CFLOW_IO_NATIVE_IOCP, 1u), SALTS_OK);
    check_equal(native_test_open_overlapped_file(&path, &file), SALTS_OK);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)file,
        .buffer = &byte,
        .length = 1u};
    submitted = native_file_submit(&fixture, 115u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[0].error, SALTS_ENOTSUP);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
        .handle = (uintptr_t)file,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 116u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[1].error, SALTS_ENOTSUP);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    check_not_null(event);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)event,
        .buffer = &byte,
        .length = 1u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 117u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 3u), SALTS_OK);
    check_equal(fixture.completions.values[2].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[2].error, SALTS_EINVAL);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_true(CloseHandle(event));
    check_equal(operation.released, 3);

    native_fixture_destroy(&fixture);
    native_test_remove_file(path, file);
}

static void native_check_file_capacity_reuse_iocp(void) {
    unsigned char first_byte = 0x31u;
    unsigned char second_byte = 0x32u;
    native_fixture fixture;
    native_test_file_operation first = {0};
    native_test_file_operation second = {0};
    cflow_io_submit_result submitted;
    char *first_path = NULL;
    char *second_path = NULL;
    HANDLE first_file = INVALID_HANDLE_VALUE;
    HANDLE second_file = INVALID_HANDLE_VALUE;

    check_equal(native_file_fixture_init(
                    &fixture, CFLOW_IO_NATIVE_IOCP, 1u), SALTS_OK);
    check_equal(native_test_open_overlapped_file(
                    &first_path, &first_file), SALTS_OK);
    check_equal(native_test_open_overlapped_file(
                    &second_path, &second_file), SALTS_OK);
    first.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
        .handle = (uintptr_t)first_file,
        .buffer = &first_byte,
        .length = 1u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 118u, &first);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    second.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
        .handle = (uintptr_t)second_file,
        .buffer = &second_byte,
        .length = 1u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 119u, &second);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[1].error, SALTS_EBUSY);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    check_true(CloseHandle(first_file));
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)first_file), SALTS_OK);
    first_file = INVALID_HANDLE_VALUE;
    submitted = native_file_submit(&fixture, 120u, &second);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 3u), SALTS_OK);
    check_equal(fixture.completions.values[2].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(first.released, 1);
    check_equal(second.released, 2);

    check_true(CloseHandle(second_file));
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)second_file), SALTS_OK);
    second_file = INVALID_HANDLE_VALUE;
    native_fixture_destroy(&fixture);
    native_test_remove_file(first_path, first_file);
    native_test_remove_file(second_path, second_file);
}

static void native_check_file_cancel_race_iocp(void) {
    native_fixture fixture;
    native_test_file_operation operation = {0};
    unsigned char byte = 0u;
    cflow_io_submit_result submitted;
    char *path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    size_t iteration;

    check_equal(native_file_fixture_init(
                    &fixture, CFLOW_IO_NATIVE_IOCP, 1u), SALTS_OK);
    check_equal(native_test_open_overlapped_file(&path, &file), SALTS_OK);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)file,
        .buffer = &byte,
        .length = 1u,
        .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE};
    submitted = native_file_submit(&fixture, 121u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_true(fixture.completions.values[0].kind ==
                   CFLOW_IO_COMPLETION_CANCELLED ||
               fixture.completions.values[0].kind ==
                   CFLOW_IO_COMPLETION_EOF);
    for (iteration = 0u; iteration < NATIVE_TEST_CANCEL_SETTLE_YIELDS;
         ++iteration) {
        (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
        (void)cflow_executor_run_ready(&fixture.executor);
        cmeta_thread_yield();
    }
    check_equal(fixture.completions.count, 1u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(operation.released, 1);

    check_true(CloseHandle(file));
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)file), SALTS_OK);
    file = INVALID_HANDLE_VALUE;
    native_fixture_destroy(&fixture);
    native_test_remove_file(path, file);
}
#endif

#if !defined(_WIN32)
#endif

#if !defined(_WIN32)
static void native_check_readiness_rejects_regular_file(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x66u, 0x69u, 0x6cu, 0x65u};
    native_fixture fixture;
    native_test_file_operation read_operation = {0};
    unsigned char received = 0u;
    unsigned char observed[sizeof(payload)] = {0};
    cflow_io_submit_result submitted;
    char *path = tt_make_temp_file("cflow-native-readiness-", ".bin");
    int fd;

    check_not_null(path);
    if (path == NULL)
        return;
    fd = open(path, O_RDWR | O_TRUNC);
    check_true(fd >= 0);
    if (fd < 0) {
        (void)tt_remove_file(path);
        free(path);
        return;
    }
    check_equal(write(fd, payload, sizeof(payload)),
                (ssize_t)sizeof(payload));
    check_equal(lseek(fd, 1, SEEK_SET), (off_t)1);
    check_equal(native_file_fixture_init(&fixture, kind, 1u), SALTS_OK);

    read_operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)fd,
        .buffer = &received,
        .length = 1u,
        .offset = 0u};
    submitted = native_file_submit(&fixture, 101u, &read_operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[0].error, SALTS_ENOTSUP);
    check_equal(received, 0u);
    check_equal(lseek(fd, 0, SEEK_CUR), (off_t)1);
    check_equal(pread(fd, observed, sizeof(observed), 0),
                (ssize_t)sizeof(observed));
    check_equal(observed, payload, sizeof(payload));
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(read_operation.released, 1);

    native_fixture_destroy(&fixture);
    check_equal(close(fd), 0);
    check_equal(tt_remove_file(path), 0);
    free(path);
}
#endif

#if defined(__linux__) || defined(__APPLE__)
static int native_test_open_posix_file(char **out_path, int *out_fd) {
    char *path = tt_make_temp_file("cflow-native-file-", ".bin");
    int fd;
    if (path == NULL)
        return SALTS_ENOMEM;
    fd = open(path, O_RDWR | O_CLOEXEC | O_TRUNC);
    if (fd < 0) {
        const int status = -errno;
        (void)tt_remove_file(path);
        free(path);
        return status;
    }
    *out_path = path;
    *out_fd = fd;
    return SALTS_OK;
}

static void native_test_remove_posix_file(char *path, int fd) {
    if (fd >= 0)
        check_equal(close(fd), 0);
    if (path != NULL) {
        check_equal(tt_remove_file(path), 0);
        free(path);
    }
}

static void native_check_file_read_write_posix(cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x75u, 0x72u, 0x69u,
                                             0x6eu, 0x67u};
    native_fixture fixture;
    native_test_file_operation operation = {0};
    unsigned char received[sizeof(payload)] = {0};
    cflow_io_submit_result submitted;
    char *path = NULL;
    int fd = -1;

    check_equal(native_file_fixture_init(
                    &fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_open_posix_file(&path, &fd), SALTS_OK);
    check_equal(lseek(fd, 3, SEEK_SET), (off_t)3);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
        .handle = (uintptr_t)fd,
        .buffer = (void *)payload,
        .length = sizeof(payload),
        .offset = 5u};
    submitted = native_file_submit(&fixture, 130u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[0].bytes, sizeof(payload));
    check_equal(lseek(fd, 0, SEEK_CUR), (off_t)3);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)fd,
        .buffer = received,
        .length = sizeof(received),
        .offset = 5u};
    submitted = native_file_submit(&fixture, 131u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[1].bytes, sizeof(payload));
    check_equal(received, payload, sizeof(payload));
    check_equal(lseek(fd, 0, SEEK_CUR), (off_t)3);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
        .handle = (uintptr_t)fd};
    submitted = native_file_submit(&fixture, 132u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 3u), SALTS_OK);
    check_equal(fixture.completions.values[2].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[2].bytes, 0u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(operation.released, 3);

    check_equal(close(fd), 0);
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)fd), SALTS_OK);
    fd = -1;
    native_fixture_destroy(&fixture);
    native_test_remove_posix_file(path, fd);
}

static void native_check_file_eof_and_type_posix(cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x45u, 0x4fu, 0x46u, 0x21u};
    native_fixture fixture;
    native_test_file_operation operation = {0};
    unsigned char received[sizeof(payload)] = {0};
    cflow_io_submit_result submitted;
    char *path = NULL;
    int fd = -1;
    int pipes[2] = {-1, -1};

    check_equal(native_file_fixture_init(
                    &fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_open_posix_file(&path, &fd), SALTS_OK);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
        .handle = (uintptr_t)fd,
        .buffer = (void *)payload,
        .length = sizeof(payload)};
    submitted = native_file_submit(&fixture, 133u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)fd,
        .buffer = received,
        .length = sizeof(received),
        .offset = 2u};
    submitted = native_file_submit(&fixture, 134u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[1].bytes, 2u);
    check_equal(received[0], payload[2]);
    check_equal(received[1], payload[3]);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    operation.native.offset = sizeof(payload);
    submitted = native_file_submit(&fixture, 135u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 3u), SALTS_OK);
    check_equal(fixture.completions.values[2].kind,
                CFLOW_IO_COMPLETION_EOF);
    check_equal(fixture.completions.values[2].bytes, 0u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    check_equal(pipe(pipes), 0);
    operation.native.handle = (uintptr_t)pipes[0];
    operation.native.offset = 0u;
    submitted = native_file_submit(&fixture, 136u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 4u), SALTS_OK);
    check_equal(fixture.completions.values[3].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[3].error, SALTS_EINVAL);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(operation.released, 4);

    check_equal(close(pipes[0]), 0);
    check_equal(close(pipes[1]), 0);
    check_equal(close(fd), 0);
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)fd), SALTS_OK);
    fd = -1;
    native_fixture_destroy(&fixture);
    native_test_remove_posix_file(path, fd);
}

static void native_check_file_cancel_race_posix(cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_file_operation operation = {0};
    unsigned char byte = 0u;
    cflow_io_submit_result submitted;
    char *path = NULL;
    int fd = -1;
    size_t iteration;

    check_equal(native_file_fixture_init(
                    &fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_open_posix_file(&path, &fd), SALTS_OK);
    operation.native = (cflow_io_native_file_operation){
        .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
        .handle = (uintptr_t)fd,
        .buffer = &byte,
        .length = 1u};
    submitted = native_file_submit(&fixture, 137u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_true(fixture.completions.values[0].kind ==
                   CFLOW_IO_COMPLETION_CANCELLED ||
               fixture.completions.values[0].kind ==
                   CFLOW_IO_COMPLETION_EOF);
    for (iteration = 0u; iteration < NATIVE_TEST_CANCEL_SETTLE_YIELDS;
         ++iteration) {
        (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
        (void)cflow_executor_run_ready(&fixture.executor);
        cmeta_thread_yield();
    }
    check_equal(fixture.completions.count, 1u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(operation.released, 1);

    check_equal(close(fd), 0);
    check_equal(native_fixture_forget_file(
                    &fixture, (uintptr_t)fd), SALTS_OK);
    fd = -1;
    native_fixture_destroy(&fixture);
    native_test_remove_posix_file(path, fd);
}
#endif

static void native_check_rejected_operation(
    cflow_io_native_operation operation) {
    check_false(cflow_io_native_operation_valid(&operation));
}

static void native_check_tcp(cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x31u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_operation receive = {0};
    native_test_operation send_operation = {0};
    cflow_io_submit_result receive_result;
    cflow_io_submit_result send_result;
    unsigned char received[NATIVE_TEST_PAYLOAD_CAPACITY] = {0};

    check_equal(native_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1], received,
        sizeof(payload), NULL, 0u, 0u};
    send_operation.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_SEND, (uintptr_t)sockets[0],
        (void *)payload, sizeof(payload), NULL, 0u, 0u};

    receive_result = native_submit(&fixture, 11u, &receive);
    send_result = native_submit(&fixture, 12u, &send_operation);
    check_equal(receive_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(send_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(received, payload, sizeof(payload));
    for (size_t i = 0u; i < fixture.completions.count; ++i) {
        check_equal(fixture.completions.values[i].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(fixture.completions.values[i].bytes, sizeof(payload));
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, fixture.completions.ids[i]),
                    CFLOW_IO_ACK_RELEASED);
    }
    check_equal(receive.released, 1);
    check_equal(send_operation.released, 1);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_tcp(cflow_io_native_backend_kind kind) {
    static const unsigned char first_payload[] = {0x31u, 0x32u};
    static const unsigned char second_payload[] = {0x33u, 0x34u, 0x35u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    native_test_vector_operation send_operation = {0};
    cflow_io_native_buffer_span receive_spans[2];
    cflow_io_native_buffer_span send_spans[2];
    cflow_io_submit_result receive_result;
    cflow_io_submit_result send_result;
    unsigned char first_received[sizeof(first_payload)] = {0};
    unsigned char second_received[sizeof(second_payload)] = {0};
    const size_t total = sizeof(first_payload) + sizeof(second_payload);

    check_equal(native_vector_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    receive_spans[0] = (cflow_io_native_buffer_span){
        first_received, sizeof(first_received)};
    receive_spans[1] = (cflow_io_native_buffer_span){
        second_received, sizeof(second_received)};
    send_spans[0] = (cflow_io_native_buffer_span){
        (void *)first_payload, sizeof(first_payload)};
    send_spans[1] = (cflow_io_native_buffer_span){
        (void *)second_payload, sizeof(second_payload)};
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1],
        receive_spans, 2u};
    send_operation.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_SEND_VECTOR, (uintptr_t)sockets[0],
        send_spans, 2u};

    receive_result = native_vector_submit(&fixture, 141u, &receive);
    send_result = native_vector_submit(&fixture, 142u, &send_operation);
    check_equal(receive_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(send_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(first_received, first_payload, sizeof(first_payload));
    check_equal(second_received, second_payload, sizeof(second_payload));
    for (size_t i = 0u; i < fixture.completions.count; ++i) {
        check_equal(fixture.completions.values[i].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(fixture.completions.values[i].bytes, total);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, fixture.completions.ids[i]),
                    CFLOW_IO_ACK_RELEASED);
    }
    check_equal(receive.released, 1);
    check_equal(send_operation.released, 1);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_descriptor_copy_and_short(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x41u, 0x42u, 0x43u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    cflow_io_native_buffer_span spans[2];
    cflow_io_submit_result submitted;
    unsigned char first[2] = {0};
    unsigned char second[4] = {0};
    unsigned char poison[6] = {0};

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    spans[0] = (cflow_io_native_buffer_span){first, sizeof(first)};
    spans[1] = (cflow_io_native_buffer_span){second, sizeof(second)};
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1], spans, 2u};
    submitted = native_vector_submit(&fixture, 143u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait_native_submitted(&fixture, 1u), SALTS_OK);

    spans[0] = (cflow_io_native_buffer_span){poison, 2u};
    spans[1] = (cflow_io_native_buffer_span){poison + 2u, 4u};
    check_equal(send(sockets[0], (const char *)payload,
                     (int)sizeof(payload), 0), (int)sizeof(payload));
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind, CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[0].bytes, sizeof(payload));
    check_equal(first, payload, sizeof(first));
    check_equal(second[0], payload[2]);
    check_equal(poison, (unsigned char[6]){0}, sizeof(poison));
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(receive.released, 1);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[1]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_eof(cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    cflow_io_native_buffer_span span;
    cflow_io_submit_result submitted;
    unsigned char received[2] = {0};

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    span = (cflow_io_native_buffer_span){received, sizeof(received)};
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1], &span, 1u};
    submitted = native_vector_submit(&fixture, 144u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait_native_submitted(&fixture, 1u), SALTS_OK);
    native_test_close_socket(sockets[0]);
    sockets[0] = NATIVE_TEST_INVALID_SOCKET;
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind, CFLOW_IO_COMPLETION_EOF);
    check_equal(fixture.completions.values[0].bytes, 0u);
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(receive.released, 1);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[1]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_cancel(cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    cflow_io_native_buffer_span span;
    cflow_io_submit_result submitted;
    unsigned char received[2] = {0};

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    span = (cflow_io_native_buffer_span){received, sizeof(received)};
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1], &span, 1u};
    submitted = native_vector_submit(&fixture, 145u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait_native_submitted(&fixture, 1u), SALTS_OK);
    check_equal(cflow_io_actor_try_cancel(&fixture.actor,
                                           submitted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(receive.released, 1);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[1]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_max_segments(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    cflow_io_native_buffer_span spans[CFLOW_IO_NATIVE_VECTOR_MAX];
    cflow_io_submit_result submitted;
    unsigned char payload[CFLOW_IO_NATIVE_VECTOR_MAX];
    unsigned char received[CFLOW_IO_NATIVE_VECTOR_MAX] = {0};

    for (size_t index = 0u; index < CFLOW_IO_NATIVE_VECTOR_MAX; ++index) {
        payload[index] = (unsigned char)(index + 1u);
        spans[index] = (cflow_io_native_buffer_span){&received[index], 1u};
    }
    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1], spans,
        CFLOW_IO_NATIVE_VECTOR_MAX};
    submitted = native_vector_submit(&fixture, 146u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(send(sockets[0], (const char *)payload, (int)sizeof(payload), 0),
                (int)sizeof(payload));
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind, CFLOW_IO_COMPLETION_OK);
    check_equal(fixture.completions.values[0].bytes, sizeof(payload));
    check_equal(received, payload, sizeof(payload));
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[1]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_same_socket_bidirectional(
    cflow_io_native_backend_kind kind) {
    static const unsigned char incoming[] = {0x51u, 0x52u};
    static const unsigned char outgoing[] = {0x61u, 0x62u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    native_test_vector_operation send_operation = {0};
    cflow_io_native_buffer_span receive_spans[2];
    cflow_io_native_buffer_span send_spans[2];
    unsigned char received[sizeof(incoming)] = {0};
    unsigned char peer_received[sizeof(outgoing)] = {0};

    check_equal(native_vector_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    check_equal(send(sockets[1], (const char *)incoming,
                     (int)sizeof(incoming), 0), (int)sizeof(incoming));
    receive_spans[0] = (cflow_io_native_buffer_span){&received[0], 1u};
    receive_spans[1] = (cflow_io_native_buffer_span){&received[1], 1u};
    send_spans[0] = (cflow_io_native_buffer_span){(void *)&outgoing[0], 1u};
    send_spans[1] = (cflow_io_native_buffer_span){(void *)&outgoing[1], 1u};
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[0],
        receive_spans, 2u};
    send_operation.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_SEND_VECTOR, (uintptr_t)sockets[0],
        send_spans, 2u};
    check_equal(native_vector_submit(&fixture, 147u, &receive).status,
                CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_vector_submit(&fixture, 148u, &send_operation).status,
                CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(native_test_receive_exact(sockets[1], peer_received,
                                          sizeof(peer_received)), SALTS_OK);
    check_equal(received, incoming, sizeof(incoming));
    check_equal(peer_received, outgoing, sizeof(outgoing));
    for (size_t index = 0u; index < fixture.completions.count; ++index) {
        check_equal(fixture.completions.values[index].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, fixture.completions.ids[index]),
                    CFLOW_IO_ACK_RELEASED);
    }
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[0]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_capacity(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation accepted_operation = {0};
    native_test_vector_operation rejected_operation = {0};
    cflow_io_native_buffer_span accepted_span;
    cflow_io_native_buffer_span rejected_span;
    cflow_io_submit_result accepted;
    cflow_io_submit_result full;
    unsigned char accepted_byte = 0u;
    unsigned char rejected_byte = 0u;

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    accepted_span = (cflow_io_native_buffer_span){&accepted_byte, 1u};
    rejected_span = (cflow_io_native_buffer_span){&rejected_byte, 1u};
    accepted_operation.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1],
        &accepted_span, 1u};
    rejected_operation.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1],
        &rejected_span, 1u};
    accepted = native_vector_submit(&fixture, 149u, &accepted_operation);
    full = native_vector_submit(&fixture, 150u, &rejected_operation);
    check_equal(accepted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(full.status, CFLOW_IO_SUBMIT_FULL);
    check_equal(rejected_operation.released, 0);
    native_vector_operation_release(&rejected_operation);
    check_equal(rejected_operation.released, 1);
    check_equal(native_fixture_wait_native_submitted(&fixture, 1u), SALTS_OK);
    check_equal(cflow_io_actor_try_cancel(&fixture.actor,
                                           accepted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            accepted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(accepted_operation.released, 1);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[1]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_identity_lifecycle(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x73u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation send_operation = {0};
    cflow_io_native_buffer_span span;
    cflow_io_submit_result submitted;

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    span = (cflow_io_native_buffer_span){(void *)payload, sizeof(payload)};
    send_operation.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_SEND_VECTOR, (uintptr_t)sockets[0], &span, 1u};
    submitted = native_vector_submit(&fixture, 151u, &send_operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[0]), SALTS_OK);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[0]),
                kind == CFLOW_IO_NATIVE_IO_URING
                    ? SALTS_OK : SALTS_ENOENT);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_shutdown_drain(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_vector_operation receive = {0};
    cflow_io_native_buffer_span span;
    cflow_io_submit_result submitted;
    unsigned char received = 0u;

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    span = (cflow_io_native_buffer_span){&received, 1u};
    receive.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, (uintptr_t)sockets[1], &span, 1u};
    submitted = native_vector_submit(&fixture, 152u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait_native_submitted(&fixture, 1u), SALTS_OK);
    check_equal(cflow_io_native_backend_shutdown(&fixture.backend),
                SALTS_EBUSY);
    check_equal(cflow_io_actor_close(&fixture.actor), SALTS_OK);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(receive.released, 1);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)sockets[1]), SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_invalid_shape_terminal(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_vector_operation operation = {0};
    cflow_io_native_buffer_span span;
    cflow_io_submit_result submitted;
    unsigned char byte = 0u;

    check_equal(native_vector_fixture_init(&fixture, kind, 1u), SALTS_OK);
    span = (cflow_io_native_buffer_span){&byte, 1u};
    operation.native = (cflow_io_native_vector_operation){
        CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, &span, 0u};
    submitted = native_vector_submit(&fixture, 153u, &operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[0].error, SALTS_EINVAL);
    check_equal(cflow_io_actor_acknowledge(&fixture.actor,
                                            submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(operation.released, 1);
    native_fixture_destroy(&fixture);
}

static void native_check_vector_contract(cflow_io_native_backend_kind kind) {
    native_check_vector_tcp(kind);
    native_check_vector_descriptor_copy_and_short(kind);
    native_check_vector_eof(kind);
    native_check_vector_cancel(kind);
    native_check_vector_max_segments(kind);
    native_check_vector_same_socket_bidirectional(kind);
    native_check_vector_capacity(kind);
    native_check_vector_identity_lifecycle(kind);
    native_check_vector_shutdown_drain(kind);
    native_check_vector_invalid_shape_terminal(kind);
}

static const cflow_io_completion *native_completion_for(
    const native_fixture *fixture, cflow_io_request_id request_id) {
    for (size_t index = 0u; index < fixture->completions.count; ++index) {
        if (fixture->completions.ids[index] == request_id)
            return &fixture->completions.values[index];
    }
    return NULL;
}

static void native_check_tcp_lifecycle(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x41u, 0x63u};
    native_fixture fixture;
    native_test_socket listener = NATIVE_TEST_INVALID_SOCKET;
    native_test_socket client = NATIVE_TEST_INVALID_SOCKET;
    native_test_socket accepted = NATIVE_TEST_INVALID_SOCKET;
    struct sockaddr_in destination;
    struct sockaddr_storage peer;
    native_test_operation accept_operation = {0};
    native_test_operation connect_operation = {0};
    native_test_operation receive_operation = {0};
    native_test_operation send_operation = {0};
    cflow_io_submit_result accept_result;
    cflow_io_submit_result connect_result;
    cflow_io_submit_result receive_result;
    cflow_io_submit_result send_result;
    const cflow_io_completion *completion;
    unsigned char received[sizeof(payload)] = {0};

    memset(&peer, 0, sizeof(peer));
    check_equal(native_fixture_init(
                    &fixture, kind, NATIVE_TEST_CAPACITY),
                SALTS_OK);
    check_equal(native_test_make_tcp_listener(
                    &listener, &client, &destination),
                SALTS_OK);
    accept_operation.native = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
        .socket = (uintptr_t)listener,
        .address = &peer,
        .address_capacity = sizeof(peer),
        .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET};
    connect_operation.native = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_CONNECT,
        .socket = (uintptr_t)client,
        .address = &destination,
        .address_capacity = sizeof(destination),
        .address_length = sizeof(destination)};

    accept_result = native_submit(&fixture, 91u, &accept_operation);
    connect_result = native_submit(&fixture, 92u, &connect_operation);
    check_equal(accept_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(connect_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    completion = native_completion_for(&fixture, accept_result.request_id);
    check_not_null(completion);
    if (completion != NULL) {
        check_equal(completion->kind, CFLOW_IO_COMPLETION_OK);
        check_equal(completion->bytes, 0u);
    }
    completion = native_completion_for(&fixture, connect_result.request_id);
    check_not_null(completion);
    if (completion != NULL) {
        check_equal(completion->kind, CFLOW_IO_COMPLETION_OK);
        check_equal(completion->bytes, 0u);
    }
    check_true(accept_operation.native.address_length > 0u);
    check_not_equal(accept_operation.native.result_socket,
                    CFLOW_IO_NATIVE_INVALID_SOCKET);
    accepted = (native_test_socket)accept_operation.native.result_socket;
    check_true(native_test_socket_is_nonblocking(accepted));
    check_equal(((const struct sockaddr *)&peer)->sa_family, AF_INET);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, accept_result.request_id),
                CFLOW_IO_ACK_RELEASED);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, connect_result.request_id),
                CFLOW_IO_ACK_RELEASED);

    fixture.completions.count = 0u;
    receive_operation.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)accepted, received,
        sizeof(received), NULL, 0u, 0u};
    send_operation.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_SEND, (uintptr_t)client, (void *)payload,
        sizeof(payload), NULL, 0u, 0u};
    receive_result = native_submit(&fixture, 93u, &receive_operation);
    send_result = native_submit(&fixture, 94u, &send_operation);
    check_equal(receive_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(send_result.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(received, payload, sizeof(payload));
    for (size_t index = 0u; index < fixture.completions.count; ++index) {
        check_equal(fixture.completions.values[index].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(fixture.completions.values[index].bytes,
                    sizeof(payload));
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, fixture.completions.ids[index]),
                    CFLOW_IO_ACK_RELEASED);
    }

    native_test_close_socket(accepted);
    native_test_close_socket(client);
    native_test_close_socket(listener);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)accepted),
                SALTS_OK);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)client),
                SALTS_OK);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)listener),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_tcp_accept_cancel_reuse(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket listener = NATIVE_TEST_INVALID_SOCKET;
    native_test_socket client = NATIVE_TEST_INVALID_SOCKET;
    native_test_socket accepted = NATIVE_TEST_INVALID_SOCKET;
    struct sockaddr_in destination;
    native_test_operation cancelled = {0};
    native_test_operation replacement = {0};
    cflow_io_submit_result submitted;

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_listener(
                    &listener, &client, &destination),
                SALTS_OK);
    cancelled.native = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
        .socket = (uintptr_t)listener,
        .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET};
    submitted = native_submit(&fixture, 95u, &cancelled);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cancelled.native.result_socket,
                CFLOW_IO_NATIVE_INVALID_SOCKET);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    fixture.completions.count = 0u;
    replacement.native = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
        .socket = (uintptr_t)listener,
        .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET};
    submitted = native_submit(&fixture, 96u, &replacement);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_equal(native_test_start_connect(client, &destination), SALTS_OK);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_OK);
    check_not_equal(replacement.native.result_socket,
                    CFLOW_IO_NATIVE_INVALID_SOCKET);
    accepted = (native_test_socket)replacement.native.result_socket;
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    native_test_close_socket(accepted);
    native_test_close_socket(client);
    native_test_close_socket(listener);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)listener),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_tcp_accept_address_overflow(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket listener = NATIVE_TEST_INVALID_SOCKET;
    native_test_socket client = NATIVE_TEST_INVALID_SOCKET;
    struct sockaddr_in destination;
    native_test_operation accept_operation = {0};
    cflow_io_submit_result submitted;
    unsigned char peer_byte = 0u;

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_listener(
                    &listener, &client, &destination),
                SALTS_OK);
    accept_operation.native = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
        .socket = (uintptr_t)listener,
        .address = &peer_byte,
        .address_capacity = sizeof(peer_byte),
        .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET};
    submitted = native_submit(&fixture, 97u, &accept_operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_equal(native_test_start_connect(client, &destination), SALTS_OK);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[0].error, SALTS_ERANGE);
    check_equal(accept_operation.native.result_socket,
                CFLOW_IO_NATIVE_INVALID_SOCKET);
    check_equal(accept_operation.native.address_length, 0u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);

    native_test_close_socket(client);
    native_test_close_socket(listener);
    check_equal(native_fixture_forget_socket(
                    &fixture, (uintptr_t)listener),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_udp(cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = "native udp payload";
    native_fixture fixture;
    native_test_socket sockets[2] = {
        NATIVE_TEST_INVALID_SOCKET, NATIVE_TEST_INVALID_SOCKET};
    struct sockaddr_in addresses[2];
    struct sockaddr_storage source_address;
    native_test_operation receive = {0};
    native_test_operation send_operation = {0};
    unsigned char received[NATIVE_TEST_PAYLOAD_CAPACITY] = {0};

    check_equal(native_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_udp_pair(sockets, addresses), SALTS_OK);
    receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_UDP_RECV_FROM, (uintptr_t)sockets[1], received,
        sizeof(payload), &source_address, sizeof(source_address), 0u};
    send_operation.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_UDP_SEND_TO, (uintptr_t)sockets[0],
        (void *)payload, sizeof(payload), &addresses[1],
        sizeof(addresses[1]), sizeof(addresses[1])};
    check_equal(native_submit(&fixture, 21u, &receive).status,
                CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_submit(&fixture, 22u, &send_operation).status,
                CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(received, payload, sizeof(payload));
    check_true(receive.native.address_length > 0u);
    for (size_t i = 0u; i < fixture.completions.count; ++i) {
        check_equal(fixture.completions.values[i].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(fixture.completions.values[i].bytes, sizeof(payload));
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, fixture.completions.ids[i]),
                    CFLOW_IO_ACK_RELEASED);
    }
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    native_fixture_destroy(&fixture);
}

static void native_check_same_tcp_socket_bidirectional(
    cflow_io_native_backend_kind kind) {
    static const unsigned char incoming[] = {0x41u};
    static const unsigned char outgoing[] = {0x42u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_operation receive = {0};
    native_test_operation send_operation = {0};
    unsigned char received[NATIVE_TEST_PAYLOAD_CAPACITY] = {0};

    check_equal(native_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    check_equal(send(sockets[1], (const char *)incoming,
                     (int)sizeof(incoming), 0), (int)sizeof(incoming));
    receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[0], received,
        sizeof(incoming), NULL, 0u, 0u};
    send_operation.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_SEND, (uintptr_t)sockets[0],
        (void *)outgoing, sizeof(outgoing), NULL, 0u, 0u};
    check_equal(native_submit(&fixture, 41u, &receive).status,
                CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_submit(&fixture, 42u, &send_operation).status,
                CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(received, incoming, sizeof(incoming));
    for (size_t index = 0u; index < fixture.completions.count; ++index) {
        check_equal(fixture.completions.values[index].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, fixture.completions.ids[index]),
                    CFLOW_IO_ACK_RELEASED);
    }
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    native_fixture_destroy(&fixture);
}

static void native_check_cancel(cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_operation receive = {0};
    unsigned char received[NATIVE_TEST_PAYLOAD_CAPACITY] = {0};
    cflow_io_submit_result submitted;

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1], received,
        sizeof(received), NULL, 0u, 0u};
    submitted = native_submit(&fixture, 31u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[1]),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}

#if defined(__linux__) || defined(__APPLE__)
static void native_check_forget_socket_identity(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x5au};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_operation send_operation = {0};
    cflow_io_submit_result submitted;

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    send_operation.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_SEND, (uintptr_t)sockets[0],
        (void *)payload, sizeof(payload), NULL, 0u, 0u};
    submitted = native_submit(&fixture, 51u, &send_operation);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[0]),
                SALTS_OK);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[0]),
                SALTS_ENOENT);
    native_fixture_destroy(&fixture);
}

static void native_check_forget_is_socket_scoped(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x63u};
    native_fixture fixture;
    native_test_socket pending_sockets[2];
    native_test_socket completed_sockets[2];
    native_test_operation pending_receive = {0};
    native_test_operation completed_send = {0};
    unsigned char received = 0u;
    cflow_io_submit_result pending;
    cflow_io_submit_result completed;

    check_equal(native_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(pending_sockets), SALTS_OK);
    check_equal(native_test_make_tcp_pair(completed_sockets), SALTS_OK);
    pending_receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)pending_sockets[1],
        &received, sizeof(received), NULL, 0u, 0u};
    completed_send.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_SEND, (uintptr_t)completed_sockets[0],
        (void *)payload, sizeof(payload), NULL, 0u, 0u};
    pending = native_submit(&fixture, 71u, &pending_receive);
    completed = native_submit(&fixture, 72u, &completed_send);
    check_equal(pending.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(completed.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.ids[0], completed.request_id);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, completed.request_id),
                CFLOW_IO_ACK_RELEASED);

    native_test_close_socket(completed_sockets[0]);
    native_test_close_socket(completed_sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend,
                    (uintptr_t)completed_sockets[0]),
                SALTS_OK);

    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, pending.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, pending.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(pending_sockets[0]);
    native_test_close_socket(pending_sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)pending_sockets[1]),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}

static void native_check_readiness_has_no_adapter_worker(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    cflow_io_native_backend_stats stats = {0};

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_true(cflow_io_native_backend_get_stats(&fixture.backend, &stats));
    check_false(stats.worker_running);
    native_fixture_destroy(&fixture);
}

static void native_check_cancel_queued_follower(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x71u};
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_operation first_receive = {0};
    native_test_operation second_receive = {0};
    unsigned char first_byte = 0u;
    unsigned char second_byte = 0u;
    cflow_io_submit_result first;
    cflow_io_submit_result second;

    check_equal(native_fixture_init(&fixture, kind, 2u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    first_receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1],
        &first_byte, sizeof(first_byte), NULL, 0u, 0u};
    second_receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1],
        &second_byte, sizeof(second_byte), NULL, 0u, 0u};
    first = native_submit(&fixture, 81u, &first_receive);
    second = native_submit(&fixture, 82u, &second_receive);
    check_equal(first.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(second.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);

    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, second.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.ids[0], second.request_id);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, second.request_id),
                CFLOW_IO_ACK_RELEASED);

    check_equal(send(sockets[0], (const char *)payload,
                     (int)sizeof(payload), 0), (int)sizeof(payload));
    check_equal(native_fixture_wait(&fixture, 2u), SALTS_OK);
    check_equal(fixture.completions.ids[1], first.request_id);
    check_equal(fixture.completions.values[1].kind,
                CFLOW_IO_COMPLETION_OK);
    check_equal(first_byte, payload[0]);
    check_equal(second_byte, 0u);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, first.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[1]),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}
#endif

#if !defined(_WIN32)
static void native_check_rejects_truncated_socket(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_operation receive = {0};
    unsigned char received = 0u;
    cflow_io_submit_result submitted;

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)INT_MAX + 1u, &received,
        sizeof(received), NULL, 0u, 0u};
    submitted = native_submit(&fixture, 61u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_FAILED);
    check_equal(fixture.completions.values[0].error, SALTS_EINVAL);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_fixture_destroy(&fixture);
}

static void native_check_preserves_caller_socket_flags(
    cflow_io_native_backend_kind kind) {
    native_fixture fixture;
    native_test_socket sockets[2];
    native_test_operation receive = {0};
    unsigned char received = 0u;
    cflow_io_submit_result submitted;
    cflow_io_native_backend_stats stats;
    int flags_before = 0;
    int flags_after = 0;

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    check_equal(native_test_set_blocking(sockets[1]), SALTS_OK);
    check_equal(native_test_status_flags(sockets[1], &flags_before), SALTS_OK);
    receive.native = (cflow_io_native_operation){
        CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1], &received,
        sizeof(received), NULL, 0u, 0u};
    submitted = native_submit(&fixture, 62u, &receive);
    check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
    (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
    check_true(cflow_io_native_backend_get_stats(&fixture.backend, &stats));
    check_equal(stats.submitted, 1u);
    check_equal(stats.active_requests, 1u);
    check_equal(native_test_status_flags(sockets[1], &flags_after), SALTS_OK);
    check_equal(flags_after, flags_before);
    check_equal(cflow_io_actor_try_cancel(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
    check_equal(fixture.completions.values[0].kind,
                CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_actor_acknowledge(
                    &fixture.actor, submitted.request_id),
                CFLOW_IO_ACK_RELEASED);
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[1]),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}
#endif

#if !defined(_WIN32)
static void native_check_cancelled_slot_reuse(
    cflow_io_native_backend_kind kind) {
    static const unsigned char payload[] = {0x5au};
    native_fixture fixture;
    native_test_socket sockets[2];
    unsigned char received[NATIVE_TEST_PAYLOAD_CAPACITY];

    check_equal(native_fixture_init(&fixture, kind, 1u), SALTS_OK);
    check_equal(native_test_make_tcp_pair(sockets), SALTS_OK);
    for (size_t iteration = 0u;
         iteration < NATIVE_TEST_CANCEL_REUSE_ITERATIONS; ++iteration) {
        native_test_operation cancelled = {0};
        native_test_operation replacement = {0};
        cflow_io_submit_result submitted;

        memset(received, 0, sizeof(received));
        cancelled.native = (cflow_io_native_operation){
            CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1], received,
            sizeof(received), NULL, 0u, 0u};
        submitted = native_submit(&fixture, 51u, &cancelled);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
        check_equal(cflow_io_actor_try_cancel(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_CANCEL_ACCEPTED);
        check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
        check_equal(fixture.completions.values[0].kind,
                    CFLOW_IO_COMPLETION_CANCELLED);
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        fixture.completions.count = 0u;

        replacement.native = (cflow_io_native_operation){
            CFLOW_IO_NATIVE_TCP_RECV, (uintptr_t)sockets[1], received,
            sizeof(payload), NULL, 0u, 0u};
        submitted = native_submit(&fixture, 52u, &replacement);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
        for (size_t attempt = 0u;
             attempt < NATIVE_TEST_CANCEL_SETTLE_YIELDS; ++attempt) {
            (void)cflow_io_actor_run_ready(&fixture.actor, 8u);
            (void)cflow_executor_run_ready(&fixture.executor);
            cmeta_thread_yield();
        }
        check_equal(fixture.completions.count, 0u);
        check_equal(send(sockets[0], (const char *)payload,
                         (int)sizeof(payload), 0), (int)sizeof(payload));
        check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
        check_equal(fixture.completions.values[0].kind,
                    CFLOW_IO_COMPLETION_OK);
        check_equal(fixture.completions.values[0].bytes, sizeof(payload));
        check_equal(received, payload, sizeof(payload));
        check_equal(cflow_io_actor_acknowledge(
                        &fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        fixture.completions.count = 0u;
    }
    native_test_close_socket(sockets[0]);
    native_test_close_socket(sockets[1]);
    check_equal(cflow_io_native_backend_forget_socket(
                    &fixture.backend, (uintptr_t)sockets[1]),
                SALTS_OK);
    native_fixture_destroy(&fixture);
}
#endif

static void native_check_backend(cflow_io_native_backend_kind kind) {
    native_check_tcp_lifecycle(kind);
    native_check_tcp(kind);
    native_check_udp(kind);
    native_check_same_tcp_socket_bidirectional(kind);
    native_check_cancel(kind);
    native_check_tcp_accept_cancel_reuse(kind);
    native_check_tcp_accept_address_overflow(kind);
#if !defined(_WIN32)
    native_check_rejects_truncated_socket(kind);
    native_check_preserves_caller_socket_flags(kind);
#endif
}

spec("CFlow native IO backend") {
    it("validates the bounded vectored TCP operation contract") {
        unsigned char bytes[2] = {0};
        cflow_io_native_buffer_span maximum_spans[
            CFLOW_IO_NATIVE_VECTOR_MAX];
        cflow_io_native_buffer_span valid_spans[2] = {
            {&bytes[0], 1u}, {&bytes[1], 1u}};
        cflow_io_native_buffer_span overflow_spans[2] = {
            {&bytes[0], UINT32_MAX}, {&bytes[1], 1u}};

        for (size_t index = 0u; index < CFLOW_IO_NATIVE_VECTOR_MAX; ++index)
            maximum_spans[index] =
                (cflow_io_native_buffer_span){&bytes[index % 2u], 1u};

        check_true(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, valid_spans, 2u}));
        check_true(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_SEND_VECTOR, 1u, valid_spans, 2u}));
        check_true(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_SEND_VECTOR, 1u, maximum_spans,
                CFLOW_IO_NATIVE_VECTOR_MAX}));
        check_false(cflow_io_native_vector_operation_valid(NULL));
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                (cflow_io_native_vector_operation_kind)-1, 1u,
                valid_spans, 2u}));
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, UINTPTR_MAX,
                valid_spans, 2u}));
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, NULL, 2u}));
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, valid_spans, 0u}));
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, valid_spans,
                CFLOW_IO_NATIVE_VECTOR_MAX + 1u}));
        valid_spans[1].data = NULL;
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, valid_spans, 2u}));
        valid_spans[1].data = &bytes[1];
        valid_spans[1].length = 0u;
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, valid_spans, 2u}));
        check_false(cflow_io_native_vector_operation_valid(
            &(cflow_io_native_vector_operation){
                CFLOW_IO_NATIVE_TCP_SEND_VECTOR, 1u,
                overflow_spans, 2u}));
    }

    it("reports vectored TCP capability separately") {
        check_false(cflow_io_native_backend_vector_operation_supported(
            (cflow_io_native_backend_kind)0,
            CFLOW_IO_NATIVE_TCP_RECV_VECTOR));
        check_false(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_POLL,
            (cflow_io_native_vector_operation_kind)-1));
#if defined(_WIN32)
        check_true(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_IOCP, CFLOW_IO_NATIVE_TCP_RECV_VECTOR));
        check_true(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_IOCP, CFLOW_IO_NATIVE_TCP_SEND_VECTOR));
        check_false(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_POLL, CFLOW_IO_NATIVE_TCP_RECV_VECTOR));
#elif defined(__APPLE__)
        check_true(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_KQUEUE, CFLOW_IO_NATIVE_TCP_RECV_VECTOR));
        check_true(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_POLL, CFLOW_IO_NATIVE_TCP_SEND_VECTOR));
#elif defined(__linux__)
#if defined(CFLOW_TEST_NATIVE_EPOLL)
        check_true(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_EPOLL, CFLOW_IO_NATIVE_TCP_RECV_VECTOR));
#else
        check_false(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_EPOLL, CFLOW_IO_NATIVE_TCP_RECV_VECTOR));
#endif
        check_true(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_POLL, CFLOW_IO_NATIVE_TCP_SEND_VECTOR));
        check_equal(
            cflow_io_native_backend_vector_operation_supported(
                CFLOW_IO_NATIVE_IO_URING,
                CFLOW_IO_NATIVE_TCP_RECV_VECTOR),
            cflow_io_native_backend_supported(CFLOW_IO_NATIVE_IO_URING));
#endif
    }

    it("returns unsupported when a backend has no native vector submit") {
        unsigned char byte = 0u;
        cflow_io_native_buffer_span span = {&byte, 1u};
        cflow_io_native_vector_operation operation = {
            CFLOW_IO_NATIVE_TCP_RECV_VECTOR, 1u, &span, 1u};
        const cflow_io_native_impl_ops impl_ops = {0};
        cflow_io_native_impl impl = {
            &impl_ops,
#if defined(_WIN32)
            CFLOW_IO_NATIVE_IOCP
#else
            CFLOW_IO_NATIVE_POLL
#endif
        };
        cflow_io_native_backend backend = {&impl};
        cflow_io_actor actor = {0};
        cflow_io_backend_ops actor_ops =
            cflow_io_native_backend_vector_actor_ops();

        check_equal(actor_ops.submit(&backend, &actor, 1u, 1u, &operation),
                    SALTS_ENOTSUP);
    }
    it("validates the bounded native file operation contract") {
        unsigned char byte = 0u;

        check_true(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
                .handle = 1u,
                .buffer = &byte,
                .length = 1u,
                .offset = 0u,
                .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE}));
        check_true(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
                .handle = 1u,
                .buffer = &byte,
                .length = 1u,
                .offset = (uint64_t)INT64_MAX - 1u}));
        check_true(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
                .handle = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = (cflow_io_native_file_operation_kind)99,
                .handle = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = (cflow_io_native_file_operation_kind)-1,
                .handle = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
                .handle = UINTPTR_MAX,
                .buffer = &byte,
                .length = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
                .handle = 1u,
                .length = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
                .handle = 1u,
                .buffer = &byte}));
#if SIZE_MAX > UINT32_MAX
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_WRITE_AT,
                .handle = 1u,
                .buffer = &byte,
                .length = (size_t)UINT32_MAX + 1u}));
#endif
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
                .handle = 1u,
                .buffer = &byte,
                .length = 1u,
                .offset = (uint64_t)INT64_MAX}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_READ_AT,
                .handle = 1u,
                .buffer = &byte,
                .length = 1u,
                .offset = (uint64_t)INT64_MAX + 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
                .handle = 1u,
                .buffer = &byte}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
                .handle = 1u,
                .length = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
                .handle = 1u,
                .offset = 1u}));
        check_false(cflow_io_native_file_operation_valid(
            &(cflow_io_native_file_operation){
                .kind = CFLOW_IO_NATIVE_FILE_FLUSH,
                .handle = 1u,
                .flags = CFLOW_IO_NATIVE_FILE_ASYNC_CAPABLE << 1u}));
    }

    it("preserves backend enum ABI and rejects Darwin AIO socket vectors") {
        check_equal((int)CFLOW_IO_NATIVE_POLL, 5);
        check_equal((int)CFLOW_IO_NATIVE_DARWIN_AIO, 6);
        check_false(cflow_io_native_backend_vector_operation_supported(
            CFLOW_IO_NATIVE_DARWIN_AIO, CFLOW_IO_NATIVE_TCP_SEND_VECTOR));
#if !defined(__APPLE__) || !TARGET_OS_OSX
        cflow_io_native_backend backend = {0};
        const cflow_io_native_backend_config config = {
            CFLOW_IO_NATIVE_DARWIN_AIO, 1u, 1u};
        check_false(cflow_io_native_backend_supported(CFLOW_IO_NATIVE_DARWIN_AIO));
        check_equal(cflow_io_native_backend_init(&backend, &config), SALTS_ENOTSUP);
#endif
    }

#if defined(__APPLE__) && TARGET_OS_OSX
    it("rejects socket operations on the file-only Darwin AIO backend") {
        native_fixture fixture;
        native_test_operation operation = {0};
        unsigned char byte = 0u;
        check_equal(native_fixture_init(
                        &fixture, CFLOW_IO_NATIVE_DARWIN_AIO, 1u), SALTS_OK);
        operation.native = (cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_RECV,
            .socket = 0u, .buffer = &byte, .length = 1u};
        const cflow_io_submit_result submitted =
            native_submit(&fixture, 138u, &operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(native_fixture_wait(&fixture, 1u), SALTS_OK);
        check_equal(fixture.completions.values[0].kind, CFLOW_IO_COMPLETION_FAILED);
        check_equal(fixture.completions.values[0].error, SALTS_ENOTSUP);
        check_equal(cflow_io_actor_acknowledge(&fixture.actor, submitted.request_id),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(operation.released, 1);
        native_fixture_destroy(&fixture);
    }
    it("reads writes and flushes regular files with Darwin AIO") {
        native_check_file_read_write_posix(CFLOW_IO_NATIVE_DARWIN_AIO);
    }
    it("reports Darwin AIO partial reads EOF and rejects pipes") {
        native_check_file_eof_and_type_posix(CFLOW_IO_NATIVE_DARWIN_AIO);
    }
    it("settles Darwin AIO cancellation with one authoritative completion") {
        native_check_file_cancel_race_posix(CFLOW_IO_NATIVE_DARWIN_AIO);
    }
#endif

    it("reports file support per backend and operation") {
#if defined(__APPLE__) && TARGET_OS_OSX
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_DARWIN_AIO, CFLOW_IO_NATIVE_FILE_READ_AT));
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_DARWIN_AIO, CFLOW_IO_NATIVE_FILE_WRITE_AT));
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_DARWIN_AIO, CFLOW_IO_NATIVE_FILE_FLUSH));
#else
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_DARWIN_AIO, CFLOW_IO_NATIVE_FILE_READ_AT));
#endif
        check_false(cflow_io_native_backend_file_operation_supported(
            (cflow_io_native_backend_kind)0,
            CFLOW_IO_NATIVE_FILE_READ_AT));
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IOCP,
            (cflow_io_native_file_operation_kind)-1));
#if defined(_WIN32)
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IOCP, CFLOW_IO_NATIVE_FILE_READ_AT));
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IOCP, CFLOW_IO_NATIVE_FILE_WRITE_AT));
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IOCP, CFLOW_IO_NATIVE_FILE_FLUSH));
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_POLL, CFLOW_IO_NATIVE_FILE_READ_AT));
#elif defined(__linux__)
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IO_URING, CFLOW_IO_NATIVE_FILE_READ_AT));
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IO_URING, CFLOW_IO_NATIVE_FILE_WRITE_AT));
        check_true(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_IO_URING, CFLOW_IO_NATIVE_FILE_FLUSH));
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_EPOLL, CFLOW_IO_NATIVE_FILE_READ_AT));
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_POLL, CFLOW_IO_NATIVE_FILE_FLUSH));
#elif defined(__APPLE__)
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_KQUEUE, CFLOW_IO_NATIVE_FILE_WRITE_AT));
        check_false(cflow_io_native_backend_file_operation_supported(
            CFLOW_IO_NATIVE_POLL, CFLOW_IO_NATIVE_FILE_FLUSH));
#endif
    }

    it("rejects malformed TCP lifecycle operation contracts") {
        unsigned char byte = 0u;
        struct sockaddr_in address;

        memset(&address, 0, sizeof(address));
        check_true(cflow_io_native_operation_valid(
            &(cflow_io_native_operation){
                .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
                .socket = 1u,
                .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET}));
        check_true(cflow_io_native_operation_valid(
            &(cflow_io_native_operation){
                .kind = CFLOW_IO_NATIVE_TCP_CONNECT,
                .socket = 1u,
                .address = &address,
                .address_capacity = sizeof(address),
                .address_length = sizeof(address)}));
        native_check_rejected_operation((cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
            .socket = 1u,
            .buffer = &byte,
            .length = sizeof(byte),
            .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET});
        native_check_rejected_operation((cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
            .socket = 1u});
        native_check_rejected_operation((cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_ACCEPT,
            .socket = 1u,
            .address_capacity = sizeof(address),
            .result_socket = CFLOW_IO_NATIVE_INVALID_SOCKET});
        native_check_rejected_operation((cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_CONNECT,
            .socket = 1u,
            .buffer = &byte,
            .length = sizeof(byte),
            .address = &address,
            .address_capacity = sizeof(address),
            .address_length = sizeof(address)});
        native_check_rejected_operation((cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_CONNECT,
            .socket = 1u});
        native_check_rejected_operation((cflow_io_native_operation){
            .kind = CFLOW_IO_NATIVE_TCP_CONNECT,
            .socket = 1u,
            .address = &address,
            .address_capacity = sizeof(address) - 1u,
            .address_length = sizeof(address)});
    }

    it("rejects invalid bounded configuration without mutating output") {
        cflow_io_native_backend backend = {(void *)(uintptr_t)1u};
        cflow_io_native_backend_config config = {
            CFLOW_IO_NATIVE_IOCP, 0u, 1u};
        check_equal(cflow_io_native_backend_init(&backend, &config),
                    SALTS_EINVAL);
        check_null(backend.impl);

        backend.impl = (void *)(uintptr_t)1u;
        config.request_capacity = 1u;
        config.completion_batch_capacity = 0u;
        check_equal(cflow_io_native_backend_init(&backend, &config),
                    SALTS_EINVAL);
        check_null(backend.impl);
    }

    it("exposes complete Actor strategy callbacks") {
        cflow_io_backend_ops ops = cflow_io_native_backend_actor_ops();
        cflow_io_backend_ops vector_ops =
            cflow_io_native_backend_vector_actor_ops();
        check_not_null(ops.submit);
        check_not_null(ops.cancel);
        check_not_null(vector_ops.submit);
        check_not_null(vector_ops.cancel);
    }

#if defined(_WIN32)
    it("rejects explicit POSIX poll without fallback") {
        cflow_io_native_backend backend = {(void *)(uintptr_t)1};
        const cflow_io_native_backend_config config = {
            CFLOW_IO_NATIVE_POLL, 1u, 1u};

        check_false(cflow_io_native_backend_supported(CFLOW_IO_NATIVE_POLL));
        check_equal(cflow_io_native_backend_init(&backend, &config),
                    SALTS_ENOTSUP);
        check_null(backend.impl);
    }

    it("runs TCP lifecycle UDP and cancellation through IOCP") {
        check_true(cflow_io_native_backend_supported(CFLOW_IO_NATIVE_IOCP));
        native_check_backend(CFLOW_IO_NATIVE_IOCP);
    }
    it("runs vectored TCP receive and send through IOCP") {
        native_check_vector_contract(CFLOW_IO_NATIVE_IOCP);
    }
    it("reads and writes regular files at explicit offsets through IOCP") {
        native_check_file_read_write_iocp();
    }
    it("reports partial regular-file reads and EOF through IOCP") {
        native_check_file_eof_iocp();
    }
    it("rejects unsupported IOCP regular-file operation shapes") {
        native_check_file_rejections_iocp();
    }
    it("bounds and reuses retained regular-file identities through IOCP") {
        native_check_file_capacity_reuse_iocp();
    }
    it("delivers one authoritative regular-file cancel race through IOCP") {
        native_check_file_cancel_race_iocp();
    }
#elif defined(__APPLE__)
    it("rejects regular files without touching them through kqueue") {
        native_check_readiness_rejects_regular_file(
            CFLOW_IO_NATIVE_KQUEUE);
    }
    it("runs TCP UDP and cancellation through kqueue") {
        check_true(cflow_io_native_backend_supported(CFLOW_IO_NATIVE_KQUEUE));
        native_check_backend(CFLOW_IO_NATIVE_KQUEUE);
    }
    it("runs vectored TCP receive and send through kqueue") {
        native_check_vector_contract(CFLOW_IO_NATIVE_KQUEUE);
    }
    it("retains each kqueue socket identity until it is forgotten") {
        native_check_forget_socket_identity(CFLOW_IO_NATIVE_KQUEUE);
    }
    it("forgets a quiescent kqueue socket while another socket is pending") {
        native_check_forget_is_socket_scoped(CFLOW_IO_NATIVE_KQUEUE);
    }
    it("uses only the platform kqueue reactor worker") {
        native_check_readiness_has_no_adapter_worker(CFLOW_IO_NATIVE_KQUEUE);
    }
    it("cancels a queued kqueue follower without waiting for the head") {
        native_check_cancel_queued_follower(CFLOW_IO_NATIVE_KQUEUE);
    }
#elif defined(__linux__)
#if defined(CFLOW_TEST_NATIVE_EPOLL)
    it("rejects regular files without touching them through epoll") {
        native_check_readiness_rejects_regular_file(CFLOW_IO_NATIVE_EPOLL);
    }
    it("runs TCP UDP and cancellation through epoll") {
        check_true(cflow_io_native_backend_supported(CFLOW_IO_NATIVE_EPOLL));
        native_check_backend(CFLOW_IO_NATIVE_EPOLL);
    }
    it("runs vectored TCP receive and send through epoll") {
        native_check_vector_contract(CFLOW_IO_NATIVE_EPOLL);
    }
    it("retains each epoll socket identity until it is forgotten") {
        native_check_forget_socket_identity(CFLOW_IO_NATIVE_EPOLL);
    }
    it("forgets a quiescent epoll socket while another socket is pending") {
        native_check_forget_is_socket_scoped(CFLOW_IO_NATIVE_EPOLL);
    }
    it("uses only the platform epoll reactor worker") {
        native_check_readiness_has_no_adapter_worker(CFLOW_IO_NATIVE_EPOLL);
    }
    it("cancels a queued epoll follower without waiting for the head") {
        native_check_cancel_queued_follower(CFLOW_IO_NATIVE_EPOLL);
    }
    it("reuses an epoll request slot after cancellation") {
        native_check_cancelled_slot_reuse(CFLOW_IO_NATIVE_EPOLL);
    }
#endif
    it("runs TCP UDP and cancellation through io_uring when available") {
        if (cflow_io_native_backend_supported(CFLOW_IO_NATIVE_IO_URING)) {
            cflow_io_native_backend probe = {0};
            cflow_io_native_backend_config config = {
                CFLOW_IO_NATIVE_IO_URING, 1u, 1u};
            const int status = cflow_io_native_backend_init(&probe, &config);
            if (status == SALTS_OK) {
                check_equal(cflow_io_native_backend_shutdown(&probe), SALTS_OK);
                check_equal(cflow_io_native_backend_destroy(&probe), SALTS_OK);
                native_check_backend(CFLOW_IO_NATIVE_IO_URING);
                native_check_vector_contract(CFLOW_IO_NATIVE_IO_URING);
                native_check_cancelled_slot_reuse(
                    CFLOW_IO_NATIVE_IO_URING);





                native_check_file_read_write_posix(CFLOW_IO_NATIVE_IO_URING);
                native_check_file_eof_and_type_posix(CFLOW_IO_NATIVE_IO_URING);
                native_check_file_cancel_race_posix(CFLOW_IO_NATIVE_IO_URING);
            } else {
                check_true(status < 0);
                check_null(probe.impl);
            }
        }
    }
#endif

#if !defined(_WIN32)
    it("rejects regular files without touching them through poll") {
        native_check_readiness_rejects_regular_file(CFLOW_IO_NATIVE_POLL);
    }
    it("runs the shared TCP UDP and cancellation contract through poll") {
        check_true(cflow_io_native_backend_supported(CFLOW_IO_NATIVE_POLL));
        native_check_backend(CFLOW_IO_NATIVE_POLL);
    }
    it("runs vectored TCP receive and send through poll") {
        native_check_vector_contract(CFLOW_IO_NATIVE_POLL);
    }
    it("retains each poll socket identity until it is forgotten") {
        native_check_forget_socket_identity(CFLOW_IO_NATIVE_POLL);
    }
    it("forgets one poll socket while another socket is pending") {
        native_check_forget_is_socket_scoped(CFLOW_IO_NATIVE_POLL);
    }
    it("uses only the platform poll reactor worker") {
        native_check_readiness_has_no_adapter_worker(CFLOW_IO_NATIVE_POLL);
    }
    it("cancels a queued poll follower without waiting for the head") {
        native_check_cancel_queued_follower(CFLOW_IO_NATIVE_POLL);
    }
    it("reuses a poll request slot after cancellation") {
        native_check_cancelled_slot_reuse(CFLOW_IO_NATIVE_POLL);
    }
#endif
}
