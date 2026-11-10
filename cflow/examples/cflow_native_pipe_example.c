#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "cflow_native_example_context.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#if defined(interface)
#undef interface
#endif
#include <windows.h>
typedef HANDLE cflow_example_pipe;
#define CFLOW_EXAMPLE_INVALID_PIPE INVALID_HANDLE_VALUE
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
typedef int cflow_example_pipe;
#define CFLOW_EXAMPLE_INVALID_PIPE (-1)
#endif

enum { CFLOW_PIPE_EXAMPLE_SKIP = 77, CFLOW_PIPE_EXAMPLE_BUFFER_CAPACITY = 4096 };

typedef struct cflow_pipe_example_operation {
    native_io_operation native;
    size_t released;
} cflow_pipe_example_operation;

static native_io_backend_kind cflow_pipe_example_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void cflow_pipe_example_close(cflow_example_pipe pipe_value) {
#if defined(_WIN32)
    if (pipe_value != NULL && pipe_value != INVALID_HANDLE_VALUE)
        (void)CloseHandle(pipe_value);
#else
    if (pipe_value >= 0)
        (void)close(pipe_value);
#endif
}

#if defined(_WIN32)
static int cflow_pipe_example_pair(cflow_example_pipe pipes[2]) {
    wchar_t name[128];
    OVERLAPPED connected = {0};
    HANDLE event = NULL;
    DWORD error = ERROR_SUCCESS;
    BOOL pending = FALSE;

    pipes[0] = INVALID_HANDLE_VALUE;
    pipes[1] = INVALID_HANDLE_VALUE;
    if (_snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                     L"\\\\.\\pipe\\cflow-native-example-%lu-%llu", GetCurrentProcessId(),
                     (unsigned long long)hrtime()) < 0)
        return SALTS_ERANGE;
    pipes[0] = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1u,
                                CFLOW_PIPE_EXAMPLE_BUFFER_CAPACITY,
                                CFLOW_PIPE_EXAMPLE_BUFFER_CAPACITY, 0u, NULL);
    if (pipes[0] == INVALID_HANDLE_VALUE)
        return -(int)GetLastError();
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
        error = GetLastError();
        goto failed;
    }
    connected.hEvent = event;
    if (!ConnectNamedPipe(pipes[0], &connected)) {
        error = GetLastError();
        if (error == ERROR_IO_PENDING)
            pending = TRUE;
        else if (error != ERROR_PIPE_CONNECTED)
            goto failed;
    }
    pipes[1] = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0u, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    if (pipes[1] == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        goto failed;
    }
    if (pending) {
        DWORD transferred = 0u;
        if (!GetOverlappedResult(pipes[0], &connected, &transferred, TRUE)) {
            error = GetLastError();
            goto failed;
        }
    }
    (void)CloseHandle(event);
    return SALTS_OK;

failed:
    cflow_pipe_example_close(pipes[1]);
    cflow_pipe_example_close(pipes[0]);
    if (event != NULL)
        (void)CloseHandle(event);
    pipes[0] = INVALID_HANDLE_VALUE;
    pipes[1] = INVALID_HANDLE_VALUE;
    return -(int)error;
}
#else
#if !defined(__linux__)
static int cflow_pipe_example_set_flags(int pipe_value) {
    int status_flags;
    int descriptor_flags;
    do {
        status_flags = fcntl(pipe_value, F_GETFL);
    } while (status_flags < 0 && errno == EINTR);
    if (status_flags < 0)
        return -errno;
    while (fcntl(pipe_value, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    do {
        descriptor_flags = fcntl(pipe_value, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags < 0)
        return -errno;
    while (fcntl(pipe_value, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    return SALTS_OK;
}
#endif

static int cflow_pipe_example_pair(cflow_example_pipe pipes[2]) {
    pipes[0] = -1;
    pipes[1] = -1;
#if defined(__linux__)
    if (pipe2(pipes, O_NONBLOCK | O_CLOEXEC) == 0)
        return SALTS_OK;
    return -errno;
#else
    int status;
    if (pipe(pipes) != 0)
        return -errno;
    status = cflow_pipe_example_set_flags(pipes[0]);
    if (status == SALTS_OK)
        status = cflow_pipe_example_set_flags(pipes[1]);
    if (status != SALTS_OK) {
        cflow_pipe_example_close(pipes[0]);
        cflow_pipe_example_close(pipes[1]);
        pipes[0] = -1;
        pipes[1] = -1;
    }
    return status;
#endif
}
#endif

static void cflow_pipe_example_release(void *user) {
    cflow_pipe_example_operation *operation = (cflow_pipe_example_operation *)user;
    ++operation->released;
}

int main(void) {
    static const unsigned char payload = 0x50u;
    cflow_native_example_context context;
    cflow_example_pipe pipes[2] = {CFLOW_EXAMPLE_INVALID_PIPE, CFLOW_EXAMPLE_INVALID_PIPE};
    cflow_pipe_example_operation read_operation = {0};
    cflow_pipe_example_operation write_operation = {0};
    cflow_io_operation read_token;
    cflow_io_operation write_token;
    cflow_io_submit_result read_result;
    cflow_io_submit_result write_result;
    cflow_io_actor_stats actor_stats = {0};
    native_io_endpoint endpoints[2] = {0};
    bool endpoint_attached[2] = {false, false};
    unsigned char received = 0u;
    int status;
    int result = EXIT_FAILURE;
    bool success = false;
    bool adapter_drained = false;

    if (!native_io_backend_kind_supports_pipe(cflow_pipe_example_backend())) {
        fprintf(stderr, "native pipe example: selected backend has no typed pipe "
                        "support; no fallback was attempted\n");
        return CFLOW_PIPE_EXAMPLE_SKIP;
    }
    status = cflow_native_example_adapter_context_init(&context, cflow_pipe_example_backend());
    if (status == SALTS_ENOTSUP) {
        fprintf(stderr, "native pipe example: selected backend is unsupported; "
                        "no fallback was attempted\n");
        return CFLOW_PIPE_EXAMPLE_SKIP;
    }
    if (status != SALTS_OK) {
        fprintf(stderr, "native pipe example: backend init failed: %d\n", status);
        (void)cflow_native_example_destroy_context(&context);
        return result;
    }
    status = cflow_pipe_example_pair(pipes);
    if (status != SALTS_OK) {
        fprintf(stderr, "native pipe example: endpoint setup failed: %d\n", status);
        goto cleanup;
    }

    status =
        cflow_io_native_adapter_attach_pipe(&context.adapter, (uintptr_t)pipes[0],
                                            NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[0]);
    if (status == SALTS_OK)
        endpoint_attached[0] = true;
    if (status == SALTS_OK) {
        status = cflow_io_native_adapter_attach_pipe(&context.adapter, (uintptr_t)pipes[1],
                                                     NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE,
                                                     &endpoints[1]);
        if (status == SALTS_OK)
            endpoint_attached[1] = true;
    }
    if (status != SALTS_OK) {
        fprintf(stderr, "native pipe example: endpoint attach failed: %d\n", status);
        goto cleanup;
    }

    read_operation.native = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_READ,
                                                  .endpoint = endpoints[0],
                                                  .buffer = &received,
                                                  .length = sizeof(received)};
    write_operation.native = (native_io_operation){.kind = NATIVE_IO_OPERATION_PIPE_WRITE,
                                                   .endpoint = endpoints[1],
                                                   .buffer = (void *)&payload,
                                                   .length = sizeof(payload)};
    read_token = (cflow_io_operation){&read_operation, cflow_pipe_example_release};
    write_token = (cflow_io_operation){&write_operation, cflow_pipe_example_release};
    read_result = cflow_io_actor_try_submit(&context.actor, 11u, &read_token);
    write_result = cflow_io_actor_try_submit(&context.actor, 12u, &write_token);
    if (read_result.status != CFLOW_IO_SUBMIT_ACCEPTED ||
        write_result.status != CFLOW_IO_SUBMIT_ACCEPTED) {
        fprintf(stderr,
                "native pipe example: bounded submit rejected: read=%d "
                "write=%d\n",
                (int)read_result.status, (int)write_result.status);
        goto cleanup;
    }
    status = cflow_native_example_drive_until(&context, CFLOW_NATIVE_EXAMPLE_CAPACITY);
    if (status != SALTS_OK) {
        fprintf(stderr, "native pipe example: completion drain failed: %d\n", status);
        goto cleanup;
    }
    for (size_t index = 0u; index < context.log.count; ++index) {
        if (context.log.completions[index].kind != CFLOW_IO_COMPLETION_OK ||
            context.log.completions[index].bytes != sizeof(payload)) {
            fprintf(stderr,
                    "native pipe example: unexpected completion kind=%d "
                    "bytes=%zu\n",
                    (int)context.log.completions[index].kind, context.log.completions[index].bytes);
            goto cleanup;
        }
    }
    if (received != payload || read_operation.released != 1u || write_operation.released != 1u ||
        !cflow_io_actor_get_stats(&context.actor, &actor_stats) ||
        actor_stats.active_requests != 0u || actor_stats.accepted != actor_stats.acknowledged) {
        fprintf(stderr,
                "native pipe example: ownership invariant failed: "
                "received=%u releases=%zu/%zu active=%zu accepted=%llu "
                "acknowledged=%llu\n",
                (unsigned)received, read_operation.released, write_operation.released,
                actor_stats.active_requests, (unsigned long long)actor_stats.accepted,
                (unsigned long long)actor_stats.acknowledged);
        goto cleanup;
    }
    success = true;

cleanup:
    status = cflow_native_example_close_actor(&context);
    if (status != SALTS_OK) {
        fprintf(stderr, "native pipe example: Actor close failed: %d\n", status);
        success = false;
    }
    if (!context.actor_initialized) {
        cflow_io_native_adapter_stats adapter_stats = {0};
        adapter_drained = cflow_io_native_adapter_get_stats(&context.adapter, &adapter_stats) &&
                          adapter_stats.active_bridges == 0u &&
                          adapter_stats.native.active_requests == 0u;
    }
    if (!adapter_drained) {
        fprintf(stderr,
                "native pipe example: pending NativeIO identity retained after failed drain\n");
        success = false;
    } else {
        status = cflow_io_native_adapter_close(&context.adapter);
        if (status != SALTS_OK && status != SALTS_EALREADY) {
            fprintf(stderr, "native pipe example: adapter close failed: %d\n", status);
            success = false;
            adapter_drained = false;
        }
    }
    if (adapter_drained) {
        for (size_t index = 0u; index < 2u; ++index) {
            if (pipes[index] != CFLOW_EXAMPLE_INVALID_PIPE) {
                cflow_pipe_example_close(pipes[index]);
                pipes[index] = CFLOW_EXAMPLE_INVALID_PIPE;
            }
            if (endpoint_attached[index]) {
                status = cflow_io_native_adapter_release_pipe(&context.adapter, endpoints[index]);
                if (status != SALTS_OK) {
                    fprintf(stderr, "native pipe example: endpoint release failed: %d\n", status);
                    success = false;
                }
                endpoint_attached[index] = false;
            }
        }
        status = cflow_native_example_destroy_context(&context);
        if (status != SALTS_OK) {
            fprintf(stderr, "native pipe example: context destroy failed: %d\n", status);
            success = false;
        }
    }
    if (success) {
        printf("native pipe: completions=2 acknowledged=2 releases=2 "
               "active=0\n");
        result = EXIT_SUCCESS;
    }
    return result;
}
