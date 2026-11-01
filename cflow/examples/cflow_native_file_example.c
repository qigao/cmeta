#include <cflow/io_file.h>

#include <clock.h>
#include <error_codes.h>
#include <thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#if defined(interface)
#undef interface
#endif
#include <windows.h>
#elif defined(__linux__) || (defined(__APPLE__) && TARGET_OS_OSX)
#include <errno.h>
#include <unistd.h>
#endif

enum {
    CFLOW_FILE_EXAMPLE_CAPACITY = 2,
    CFLOW_FILE_EXAMPLE_MAX_STEPS = 64,
    CFLOW_FILE_EXAMPLE_PATH_CAPACITY = 512,
    CFLOW_FILE_EXAMPLE_SKIP = 77
};

static const uint64_t CFLOW_FILE_EXAMPLE_TIMEOUT_NS =
    UINT64_C(5000000000);
static const uint64_t CFLOW_FILE_EXAMPLE_OFFSET = UINT64_C(7);

typedef struct cflow_file_example_state {
    cflow_io_completion completions[CFLOW_FILE_EXAMPLE_CAPACITY];
    cflow_io_native_file_operation_kind kinds[CFLOW_FILE_EXAMPLE_CAPACITY];
    size_t count;
    bool overflow;
} cflow_file_example_state;

static void cflow_file_example_completed(
    void *user, cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    cflow_io_native_file_operation_kind operation_kind,
    const cflow_io_completion *completion) {
    cflow_file_example_state *state =
        (cflow_file_example_state *)user;
    (void)request_id;
    (void)lease_id;
    if (state == NULL || completion == NULL)
        return;
    if (state->count >= CFLOW_FILE_EXAMPLE_CAPACITY) {
        state->overflow = true;
        return;
    }
    state->kinds[state->count] = operation_kind;
    state->completions[state->count] = *completion;
    ++state->count;
}

static cflow_io_native_backend_kind cflow_file_example_backend(void) {
#if defined(_WIN32)
    return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
    return CFLOW_IO_NATIVE_IO_URING;
#elif defined(__APPLE__) && TARGET_OS_OSX
    return CFLOW_IO_NATIVE_DARWIN_AIO;
#else
    return CFLOW_IO_NATIVE_POLL;
#endif
}

static int cflow_file_example_make_path(
    char path[CFLOW_FILE_EXAMPLE_PATH_CAPACITY]) {
#if defined(_WIN32)
    char directory[CFLOW_FILE_EXAMPLE_PATH_CAPACITY];
    const DWORD length = GetTempPathA(
        (DWORD)sizeof(directory), directory);
    if (length == 0u || length >= sizeof(directory))
        return -(int)GetLastError();
    if (_snprintf_s(
            path, CFLOW_FILE_EXAMPLE_PATH_CAPACITY, _TRUNCATE,
            "%scflow-native-file-%lu-%llu.bin", directory,
            GetCurrentProcessId(),
            (unsigned long long)cmeta_hrtime()) < 0)
        return SALTS_ERANGE;
    return SALTS_OK;
#elif defined(__linux__) || (defined(__APPLE__) && TARGET_OS_OSX)
    const int written = snprintf(
        path, CFLOW_FILE_EXAMPLE_PATH_CAPACITY,
        "/tmp/cflow-native-file-%ld-%llu.bin", (long)getpid(),
        (unsigned long long)cmeta_hrtime());
    return written > 0 && written < CFLOW_FILE_EXAMPLE_PATH_CAPACITY
        ? SALTS_OK : SALTS_ERANGE;
#else
    (void)path;
    return SALTS_ENOTSUP;
#endif
}

static int cflow_file_example_remove(const char *path) {
    if (path == NULL || path[0] == '\0')
        return SALTS_EINVAL;
#if defined(_WIN32)
    return DeleteFileA(path) ? SALTS_OK : -(int)GetLastError();
#elif defined(__linux__) || (defined(__APPLE__) && TARGET_OS_OSX)
    return unlink(path) == 0 ? SALTS_OK : -errno;
#else
    return SALTS_ENOTSUP;
#endif
}

static bool cflow_file_example_runtime_unavailable(int status) {
    if (status == SALTS_ENOTSUP)
        return true;
#if defined(__linux__)
    return status == -EPERM || status == -EACCES || status == -ENOSYS;
#else
    return false;
#endif
}

static int cflow_file_example_drive_until(
    cflow_io_file *file, cflow_file_example_state *state,
    size_t expected_completions) {
    const uint64_t started = cmeta_hrtime();
    for (;;) {
        cflow_io_file_stats stats = {0};
        size_t progressed = 0u;
        const int status = cflow_io_file_run_ready(
            file, CFLOW_FILE_EXAMPLE_MAX_STEPS, &progressed);
        if (status != SALTS_OK)
            return status;
        if (!cflow_io_file_get_stats(file, &stats))
            return SALTS_EPROTO;
        if (state->overflow)
            return SALTS_ERANGE;
        if (state->count >= expected_completions &&
            stats.operation_slots_in_use == 0u)
            return SALTS_OK;
        if (cmeta_hrtime() - started >= CFLOW_FILE_EXAMPLE_TIMEOUT_NS)
            return SALTS_ETIMEDOUT;
        if (progressed == 0u)
            cmeta_thread_yield();
    }
}

static int cflow_file_example_close_destroy(cflow_io_file *file) {
    const uint64_t started = cmeta_hrtime();
    int status = cflow_io_file_close(file);
    if (status != SALTS_OK && status != SALTS_EALREADY)
        return status;
    while (!cflow_io_file_is_quiescent(file)) {
        size_t progressed = 0u;
        status = cflow_io_file_run_ready(
            file, CFLOW_FILE_EXAMPLE_MAX_STEPS, &progressed);
        if (status != SALTS_OK)
            return status;
        if (cmeta_hrtime() - started >= CFLOW_FILE_EXAMPLE_TIMEOUT_NS)
            return SALTS_ETIMEDOUT;
        if (progressed == 0u)
            cmeta_thread_yield();
    }
    return cflow_io_file_destroy(file);
}

int main(void) {
    static const unsigned char payload[] = "native-file";
    char path[CFLOW_FILE_EXAMPLE_PATH_CAPACITY] = {0};
    unsigned char received[sizeof(payload)] = {0};
    cflow_io_file file = {0};
    cflow_file_example_state state = {0};
    cflow_io_file_config config = {0};
    cflow_io_file_submit_result submitted;
    cflow_io_file_stats stats = {0};
    int status;
    int result = EXIT_FAILURE;

#if !defined(_WIN32) && !defined(__linux__) && !(defined(__APPLE__) && TARGET_OS_OSX)
    fprintf(stderr,
            "native file example: this host has no declared asynchronous "
            "regular-file backend; no fallback was attempted\n");
    return CFLOW_FILE_EXAMPLE_SKIP;
#endif
    if (!cflow_io_native_backend_file_operation_supported(
            cflow_file_example_backend(), CFLOW_IO_NATIVE_FILE_READ_AT) ||
        !cflow_io_native_backend_file_operation_supported(
            cflow_file_example_backend(), CFLOW_IO_NATIVE_FILE_WRITE_AT)) {
        fprintf(stderr,
                "native file example: selected backend does not support "
                "READ_AT/WRITE_AT; no fallback was attempted\n");
        return CFLOW_FILE_EXAMPLE_SKIP;
    }
    status = cflow_file_example_make_path(path);
    if (status != SALTS_OK) {
        fprintf(stderr, "native file example: temp path failed: %d\n",
                status);
        return result;
    }
    config.backend_kind = cflow_file_example_backend();
    config.request_capacity = CFLOW_FILE_EXAMPLE_CAPACITY;
    config.command_capacity = CFLOW_FILE_EXAMPLE_CAPACITY;
    config.completion_batch_capacity = CFLOW_FILE_EXAMPLE_CAPACITY;
    config.open_flags = CFLOW_IO_FILE_READ | CFLOW_IO_FILE_WRITE |
                        CFLOW_IO_FILE_CREATE | CFLOW_IO_FILE_TRUNCATE;
    config.create_mode = 0600u;
    config.completion = cflow_file_example_completed;
    config.completion_user = &state;
    status = cflow_io_file_open(&file, path, &config);
    if (status != SALTS_OK) {
        if (cflow_file_example_runtime_unavailable(status)) {
            fprintf(stderr,
                    "native file example: selected backend is unavailable "
                    "at runtime (%d); no fallback was attempted\n",
                    status);
            status = cflow_file_example_remove(path);
            if (status != SALTS_OK && status != SALTS_ENOENT) {
                fprintf(stderr,
                        "native file example: temp cleanup failed: %d\n",
                        status);
                return result;
            }
            return CFLOW_FILE_EXAMPLE_SKIP;
        }
        fprintf(stderr, "native file example: open failed: %d\n", status);
        status = cflow_file_example_remove(path);
        if (status != SALTS_OK && status != SALTS_ENOENT)
            fprintf(stderr,
                    "native file example: temp cleanup failed: %d\n",
                    status);
        return result;
    }
    submitted = cflow_io_file_try_write_at(
        &file, 21u, payload, sizeof(payload) - 1u,
        CFLOW_FILE_EXAMPLE_OFFSET);
    if (submitted.status != CFLOW_IO_FILE_SUBMIT_ACCEPTED) {
        fprintf(stderr,
                "native file example: bounded write rejected: %d\n",
                (int)submitted.status);
        goto cleanup;
    }
    status = cflow_file_example_drive_until(&file, &state, 1u);
    if (status != SALTS_OK ||
        state.kinds[0] != CFLOW_IO_NATIVE_FILE_WRITE_AT ||
        state.completions[0].kind != CFLOW_IO_COMPLETION_OK ||
        state.completions[0].bytes != sizeof(payload) - 1u) {
        fprintf(stderr,
                "native file example: write completion failed: status=%d "
                "kind=%d bytes=%zu\n",
                status, (int)state.completions[0].kind,
                state.completions[0].bytes);
        goto cleanup;
    }
    submitted = cflow_io_file_try_read_at(
        &file, 22u, received, sizeof(payload) - 1u,
        CFLOW_FILE_EXAMPLE_OFFSET);
    if (submitted.status != CFLOW_IO_FILE_SUBMIT_ACCEPTED) {
        fprintf(stderr,
                "native file example: bounded read rejected: %d\n",
                (int)submitted.status);
        goto cleanup;
    }
    status = cflow_file_example_drive_until(&file, &state, 2u);
    if (status != SALTS_OK ||
        state.kinds[1] != CFLOW_IO_NATIVE_FILE_READ_AT ||
        state.completions[1].kind != CFLOW_IO_COMPLETION_OK ||
        state.completions[1].bytes != sizeof(payload) - 1u ||
        memcmp(received, payload, sizeof(payload) - 1u) != 0 ||
        !cflow_io_file_get_stats(&file, &stats) ||
        stats.operation_slots_in_use != 0u ||
        stats.actor.active_requests != 0u ||
        stats.actor.accepted != 2u || stats.actor.acknowledged != 2u) {
        fprintf(stderr,
                "native file example: read/ownership invariant failed: "
                "status=%d completion=%d bytes=%zu slots=%zu active=%zu "
                "accepted=%llu acknowledged=%llu\n",
                status, (int)state.completions[1].kind,
                state.completions[1].bytes, stats.operation_slots_in_use,
                stats.actor.active_requests,
                (unsigned long long)stats.actor.accepted,
                (unsigned long long)stats.actor.acknowledged);
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    status = cflow_file_example_close_destroy(&file);
    if (status != SALTS_OK) {
        fprintf(stderr, "native file example: close/drain/destroy failed: %d\n",
                status);
        result = EXIT_FAILURE;
    }
    status = cflow_file_example_remove(path);
    if (status != SALTS_OK) {
        fprintf(stderr, "native file example: temp cleanup failed: %d\n",
                status);
        result = EXIT_FAILURE;
    }
    if (result == EXIT_SUCCESS)
        printf("native file: completions=2 acknowledged=2 slots=0 active=0\n");
    return result;
}
