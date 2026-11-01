#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cflow/cflow.h>

#include <clock.h>
#include <error_codes.h>
#include <thread.h>
#include <thread_pool.h>

#include "tinytest.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#if defined(interface)
#undef interface
#endif
#include <windows.h>
typedef HANDLE native_adapter_test_pipe;
#define NATIVE_ADAPTER_TEST_INVALID_PIPE INVALID_HANDLE_VALUE
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
typedef int native_adapter_test_pipe;
#define NATIVE_ADAPTER_TEST_INVALID_PIPE (-1)
#endif

enum {
    NATIVE_ADAPTER_TEST_CAPACITY = 2,
    NATIVE_ADAPTER_TEST_MAX_STEPS = 16,
    NATIVE_ADAPTER_TEST_POLL_TIMEOUT_MS = 1,
    NATIVE_ADAPTER_TEST_TIMEOUT_MS = 5000,
    NATIVE_ADAPTER_TEST_PIPE_BUFFER_CAPACITY = 4096,
    NATIVE_ADAPTER_TEST_THREAD_WAIT_SLICE_NS = 10000000,
    NATIVE_ADAPTER_TEST_THREAD_WAIT_LIMIT = 500,
    NATIVE_ADAPTER_TEST_PUBLISHER_QUEUE_CAPACITY = 16
};

enum {
    NATIVE_ADAPTER_TEST_THREAD_ROLE_NONE = 0,
    NATIVE_ADAPTER_TEST_THREAD_ROLE_MAIN,
    NATIVE_ADAPTER_TEST_THREAD_ROLE_PUBLISHER,
    NATIVE_ADAPTER_TEST_THREAD_ROLE_SUBSCRIBER
};

static const uint64_t NATIVE_ADAPTER_TEST_TIMEOUT_NS = UINT64_C(5000000000);

static SALTS_THREAD_LOCAL int native_adapter_test_thread_role;

typedef struct native_adapter_test_operation {
    native_io_operation native;
    size_t *release_count;
} native_adapter_test_operation;

typedef struct native_adapter_test_completions {
    cflow_io_request_id ids[NATIVE_ADAPTER_TEST_CAPACITY];
    cflow_io_completion values[NATIVE_ADAPTER_TEST_CAPACITY];
    size_t count;
} native_adapter_test_completions;

typedef struct native_adapter_test_source_operation {
    native_io_operation native;
    atomic_size_t *release_count;
    cmeta_mutex_t *release_gate;
    cmeta_cond_t *release_changed;
} native_adapter_test_source_operation;

typedef struct native_adapter_test_source_fixture {
    native_adapter_test_source_operation operations[NATIVE_ADAPTER_TEST_CAPACITY];
    size_t operation_count;
    atomic_size_t prepared;
    atomic_size_t encoded;
    atomic_size_t release_count;
    int prepare_expected_role;
    int encode_expected_role;
    atomic_int prepare_callbacks;
    atomic_int encode_callbacks;
    atomic_int prepare_role_collisions;
    atomic_int encode_role_collisions;
} native_adapter_test_source_fixture;

typedef struct native_adapter_test_sink_probe {
    int values[NATIVE_ADAPTER_TEST_CAPACITY];
    size_t value_count;
    size_t error_count;
    size_t done_count;
    const char *error;
} native_adapter_test_sink_probe;

typedef struct native_adapter_test_threaded_sink_probe {
    cmeta_mutex_t gate;
    cmeta_cond_t changed;
    int values[NATIVE_ADAPTER_TEST_CAPACITY];
    size_t value_count;
    size_t error_count;
    size_t done_count;
    const char *error;
    atomic_int subscriber_callbacks;
    atomic_int role_collisions;
} native_adapter_test_threaded_sink_probe;

typedef struct native_adapter_test_threaded_driver {
    cflow_io_native_adapter *adapter;
    cflow_io_publisher_owner *owner;
    cmeta_mutex_t gate;
    cmeta_cond_t changed;
    bool drive_pending;
    bool stop_requested;
    atomic_int wake_status;
    atomic_int drive_status;
    atomic_size_t drive_cycles;
    atomic_size_t observed;
    atomic_int publisher_callbacks;
    atomic_int role_collisions;
} native_adapter_test_threaded_driver;

typedef struct native_adapter_test_threaded_cleanup {
    cflow_io_native_adapter *adapter;
    cflow_io_publisher_owner *owner;
    native_adapter_test_pipe *pipes;
    native_io_endpoint *endpoints;
    bool endpoint_attached[NATIVE_ADAPTER_TEST_CAPACITY];
    int drain_status;
    bool owner_quiescent;
    bool stats_valid;
    cflow_io_native_adapter_stats stats;
    int owner_status;
    int adapter_close_status;
    int release_status[NATIVE_ADAPTER_TEST_CAPACITY];
    int destroy_status;
} native_adapter_test_threaded_cleanup;

typedef struct native_adapter_test_threaded_setup {
    cflow_io_native_adapter *adapter;
    const cflow_io_native_adapter_config *config;
    native_adapter_test_pipe *pipes;
    native_io_endpoint *endpoints;
    bool adapter_initialized;
    bool endpoint_attached[NATIVE_ADAPTER_TEST_CAPACITY];
    int status;
} native_adapter_test_threaded_setup;

typedef struct native_adapter_test_publisher_result {
    int status;
    bool payload_equal;
    bool subscription_done;
    size_t prepared;
    size_t encoded;
    size_t released;
    size_t values;
    int first_value;
    int second_value;
    size_t errors;
    size_t dones;
    int publisher_callbacks;
    int subscriber_callbacks;
    int publisher_role_collisions;
    int subscriber_role_collisions;
    int prepare_callbacks;
    int encode_callbacks;
    int prepare_role_collisions;
    int encode_role_collisions;
    int wake_status;
    int drive_status;
    size_t drive_cycles;
    size_t observed;
    bool owner_drained;
    size_t active_bridges;
    size_t active_native_requests;
    uint64_t cancelled_native_requests;
} native_adapter_test_publisher_result;

static native_io_backend_kind native_adapter_test_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static void native_adapter_test_close_pipe(native_adapter_test_pipe pipe_value) {
#if defined(_WIN32)
    if (pipe_value != NULL && pipe_value != INVALID_HANDLE_VALUE)
        (void)CloseHandle(pipe_value);
#else
    if (pipe_value >= 0)
        (void)close(pipe_value);
#endif
}

#if defined(_WIN32)
static int native_adapter_test_make_pipe_pair(native_adapter_test_pipe pipes[2]) {
    wchar_t name[128];
    OVERLAPPED connected = {0};
    HANDLE event = NULL;
    DWORD error = ERROR_SUCCESS;
    BOOL pending = FALSE;

    pipes[0] = INVALID_HANDLE_VALUE;
    pipes[1] = INVALID_HANDLE_VALUE;
    if (_snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                     L"\\\\.\\pipe\\cflow-native-adapter-test-%lu-%llu",
                     GetCurrentProcessId(), (unsigned long long)cmeta_hrtime()) < 0)
        return SALTS_ERANGE;
    pipes[0] = CreateNamedPipeW(
        name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1u,
        NATIVE_ADAPTER_TEST_PIPE_BUFFER_CAPACITY,
        NATIVE_ADAPTER_TEST_PIPE_BUFFER_CAPACITY, 0u, NULL);
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
    pipes[1] = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0u, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
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
    native_adapter_test_close_pipe(pipes[1]);
    native_adapter_test_close_pipe(pipes[0]);
    if (event != NULL)
        (void)CloseHandle(event);
    pipes[0] = INVALID_HANDLE_VALUE;
    pipes[1] = INVALID_HANDLE_VALUE;
    return -(int)error;
}
#else
#if !defined(__linux__)
static int native_adapter_test_set_pipe_flags(int pipe_value) {
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

static int native_adapter_test_make_pipe_pair(native_adapter_test_pipe pipes[2]) {
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
    status = native_adapter_test_set_pipe_flags(pipes[0]);
    if (status == SALTS_OK)
        status = native_adapter_test_set_pipe_flags(pipes[1]);
    if (status != SALTS_OK) {
        native_adapter_test_close_pipe(pipes[0]);
        native_adapter_test_close_pipe(pipes[1]);
        pipes[0] = -1;
        pipes[1] = -1;
    }
    return status;
#endif
}
#endif

static void native_adapter_test_record_thread_role(
    int expected_role,
    atomic_int *callbacks,
    atomic_int *collisions);

static void native_adapter_test_release(void *operation_user) {
    native_adapter_test_operation *operation =
        (native_adapter_test_operation *)operation_user;
    ++*operation->release_count;
}

static void native_adapter_test_source_release(void *operation_user) {
    native_adapter_test_source_operation *operation =
        (native_adapter_test_source_operation *)operation_user;

    if (operation->release_gate != NULL)
        cmeta_mutex_lock(operation->release_gate);
    atomic_fetch_add(operation->release_count, 1u);
    if (operation->release_changed != NULL)
        cmeta_cond_broadcast(operation->release_changed);
    if (operation->release_gate != NULL)
        cmeta_mutex_unlock(operation->release_gate);
}

static cflow_io_publisher_prepare_status native_adapter_test_source_prepare(
    void *user, cflow_io_operation *operation, const char **error) {
    native_adapter_test_source_fixture *fixture =
        (native_adapter_test_source_fixture *)user;
    const size_t prepared = atomic_load(&fixture->prepared);

    (void)error;
    if (fixture->prepare_expected_role != NATIVE_ADAPTER_TEST_THREAD_ROLE_NONE) {
        native_adapter_test_record_thread_role(
            fixture->prepare_expected_role,
            &fixture->prepare_callbacks,
            &fixture->prepare_role_collisions);
    }
    if (prepared >= fixture->operation_count)
        return CFLOW_IO_PUBLISHER_PREPARE_DONE;
    operation->user = &fixture->operations[prepared];
    operation->release = native_adapter_test_source_release;
    atomic_fetch_add(&fixture->prepared, 1u);
    return CFLOW_IO_PUBLISHER_PREPARE_OPERATION;
}

static cflow_read_status native_adapter_test_source_encode(
    void *user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion,
    void *out_value,
    const char **error) {
    static const char completion_error[] =
        "NativeIO Publisher received a non-success completion";
    native_adapter_test_source_fixture *fixture =
        (native_adapter_test_source_fixture *)user;

    (void)request_id;
    (void)lease_id;
    (void)operation_user;
    if (fixture->encode_expected_role != NATIVE_ADAPTER_TEST_THREAD_ROLE_NONE) {
        native_adapter_test_record_thread_role(
            fixture->encode_expected_role,
            &fixture->encode_callbacks,
            &fixture->encode_role_collisions);
    }
    if (completion->kind != CFLOW_IO_COMPLETION_OK) {
        *error = completion_error;
        return CFLOW_READ_ERROR;
    }
    *(int *)out_value = (int)completion->bytes;
    atomic_fetch_add(&fixture->encoded, 1u);
    return CFLOW_READ_VALUE;
}

static bool native_adapter_test_sink_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    native_adapter_test_sink_probe *probe =
        (native_adapter_test_sink_probe *)user;

    (void)type;
    if (probe->value_count < NATIVE_ADAPTER_TEST_CAPACITY)
        probe->values[probe->value_count] = *(const int *)value;
    ++probe->value_count;
    return true;
}

static void native_adapter_test_sink_error(void *user, const char *message) {
    native_adapter_test_sink_probe *probe =
        (native_adapter_test_sink_probe *)user;

    ++probe->error_count;
    probe->error = message;
}

static void native_adapter_test_sink_done(void *user) {
    native_adapter_test_sink_probe *probe =
        (native_adapter_test_sink_probe *)user;

    ++probe->done_count;
}

static void native_adapter_test_record_thread_role(
    int expected_role,
    atomic_int *callbacks,
    atomic_int *collisions) {
    if (native_adapter_test_thread_role != NATIVE_ADAPTER_TEST_THREAD_ROLE_NONE &&
        native_adapter_test_thread_role != expected_role)
        atomic_fetch_add(collisions, 1);
    native_adapter_test_thread_role = expected_role;
    atomic_fetch_add(callbacks, 1);
}

static bool native_adapter_test_threaded_sink_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    native_adapter_test_threaded_sink_probe *probe =
        (native_adapter_test_threaded_sink_probe *)user;

    (void)type;
    native_adapter_test_record_thread_role(
        NATIVE_ADAPTER_TEST_THREAD_ROLE_SUBSCRIBER,
        &probe->subscriber_callbacks, &probe->role_collisions);
    cmeta_mutex_lock(&probe->gate);
    if (probe->value_count < NATIVE_ADAPTER_TEST_CAPACITY)
        probe->values[probe->value_count] = *(const int *)value;
    ++probe->value_count;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->gate);
    return true;
}

static void native_adapter_test_threaded_sink_error(
    void *user, const char *message) {
    native_adapter_test_threaded_sink_probe *probe =
        (native_adapter_test_threaded_sink_probe *)user;

    native_adapter_test_record_thread_role(
        NATIVE_ADAPTER_TEST_THREAD_ROLE_SUBSCRIBER,
        &probe->subscriber_callbacks, &probe->role_collisions);
    cmeta_mutex_lock(&probe->gate);
    ++probe->error_count;
    probe->error = message;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->gate);
}

static void native_adapter_test_threaded_sink_done(void *user) {
    native_adapter_test_threaded_sink_probe *probe =
        (native_adapter_test_threaded_sink_probe *)user;

    native_adapter_test_record_thread_role(
        NATIVE_ADAPTER_TEST_THREAD_ROLE_SUBSCRIBER,
        &probe->subscriber_callbacks, &probe->role_collisions);
    cmeta_mutex_lock(&probe->gate);
    ++probe->done_count;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->gate);
}

static bool native_adapter_test_threaded_sink_wait(
    native_adapter_test_threaded_sink_probe *probe,
    size_t values,
    size_t dones) {
    size_t waits = 0u;
    bool ready;

    cmeta_mutex_lock(&probe->gate);
    while ((probe->value_count < values || probe->done_count < dones) &&
           probe->error_count == 0u &&
           waits < NATIVE_ADAPTER_TEST_THREAD_WAIT_LIMIT) {
        (void)cmeta_cond_timedwait(
            &probe->changed, &probe->gate,
            NATIVE_ADAPTER_TEST_THREAD_WAIT_SLICE_NS);
        ++waits;
    }
    ready = probe->value_count >= values && probe->done_count >= dones &&
            probe->error_count == 0u;
    cmeta_mutex_unlock(&probe->gate);
    return ready;
}

static bool native_adapter_test_threaded_release_wait(
    native_adapter_test_threaded_sink_probe *probe,
    const atomic_size_t *release_count,
    size_t expected) {
    size_t waits = 0u;
    bool ready;

    cmeta_mutex_lock(&probe->gate);
    while (atomic_load(release_count) < expected &&
           waits < NATIVE_ADAPTER_TEST_THREAD_WAIT_LIMIT) {
        (void)cmeta_cond_timedwait(
            &probe->changed, &probe->gate,
            NATIVE_ADAPTER_TEST_THREAD_WAIT_SLICE_NS);
        ++waits;
    }
    ready = atomic_load(release_count) >= expected;
    cmeta_mutex_unlock(&probe->gate);
    return ready;
}

static void native_adapter_test_keep_first_status(
    int *first_status, int status) {
    if (*first_status == SALTS_OK && status != SALTS_OK)
        *first_status = status;
}

static void native_adapter_test_threaded_setup_task(void *user) {
    native_adapter_test_threaded_setup *setup =
        (native_adapter_test_threaded_setup *)user;

    native_adapter_test_thread_role = NATIVE_ADAPTER_TEST_THREAD_ROLE_PUBLISHER;
    setup->status = cflow_io_native_adapter_init(setup->adapter, setup->config);
    if (setup->status != SALTS_OK)
        return;
    setup->adapter_initialized = true;
    for (size_t index = 0u; index < NATIVE_ADAPTER_TEST_CAPACITY; ++index) {
        setup->status = cflow_io_native_adapter_attach_pipe(
            setup->adapter, (uintptr_t)setup->pipes[index],
            NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &setup->endpoints[index]);
        if (setup->status != SALTS_OK)
            return;
        setup->endpoint_attached[index] = true;
    }
}

static void native_adapter_test_threaded_drive_task(void *user) {
    native_adapter_test_threaded_driver *driver =
        (native_adapter_test_threaded_driver *)user;

    native_adapter_test_record_thread_role(
        NATIVE_ADAPTER_TEST_THREAD_ROLE_PUBLISHER,
        &driver->publisher_callbacks, &driver->role_collisions);
    for (;;) {
        size_t observed = 0u;
        int status;

        cmeta_mutex_lock(&driver->gate);
        while (!driver->drive_pending && !driver->stop_requested)
            cmeta_cond_wait(&driver->changed, &driver->gate);
        if (driver->stop_requested) {
            cmeta_mutex_unlock(&driver->gate);
            break;
        }
        driver->drive_pending = false;
        cmeta_mutex_unlock(&driver->gate);

        status = cflow_io_native_adapter_drive_publisher(
            driver->adapter, driver->owner, UINT32_MAX,
            NATIVE_ADAPTER_TEST_MAX_STEPS, &observed);
        if (status != SALTS_OK) {
            int expected = SALTS_OK;
            (void)atomic_compare_exchange_strong(
                &driver->drive_status, &expected, status);
            break;
        }
        atomic_fetch_add(&driver->observed, observed);
        atomic_fetch_add(&driver->drive_cycles, 1u);
    }
}

static void native_adapter_test_threaded_drive(void *user) {
    native_adapter_test_threaded_driver *driver =
        (native_adapter_test_threaded_driver *)user;
    const int status = cflow_io_native_adapter_wake(driver->adapter);

    cmeta_mutex_lock(&driver->gate);
    driver->drive_pending = true;
    if (status != SALTS_OK) {
        int expected = SALTS_OK;
        (void)atomic_compare_exchange_strong(
            &driver->wake_status, &expected, status);
    }
    cmeta_cond_signal(&driver->changed);
    cmeta_mutex_unlock(&driver->gate);
}

static void native_adapter_test_threaded_stop(
    native_adapter_test_threaded_driver *driver) {
    const int status = cflow_io_native_adapter_wake(driver->adapter);

    cmeta_mutex_lock(&driver->gate);
    driver->stop_requested = true;
    cmeta_cond_broadcast(&driver->changed);
    cmeta_mutex_unlock(&driver->gate);
    if (status != SALTS_OK) {
        int expected = SALTS_OK;
        (void)atomic_compare_exchange_strong(
            &driver->wake_status, &expected, status);
    }
}

static void native_adapter_test_threaded_cleanup_task(void *user) {
    native_adapter_test_threaded_cleanup *cleanup =
        (native_adapter_test_threaded_cleanup *)user;
    const uint64_t started = cmeta_hrtime();

    native_adapter_test_thread_role = NATIVE_ADAPTER_TEST_THREAD_ROLE_PUBLISHER;
    cleanup->drain_status = SALTS_OK;
    while (cleanup->owner->impl != NULL &&
           !cflow_io_publisher_owner_is_quiescent(cleanup->owner)) {
        size_t completed = 0u;
        const int status = cflow_io_native_adapter_drive_publisher(
            cleanup->adapter, cleanup->owner,
            NATIVE_ADAPTER_TEST_POLL_TIMEOUT_MS,
            NATIVE_ADAPTER_TEST_MAX_STEPS, &completed);

        if (status != SALTS_OK && status != SALTS_ETIMEDOUT) {
            cleanup->drain_status = status;
            break;
        }
        if (cmeta_hrtime() - started >= NATIVE_ADAPTER_TEST_TIMEOUT_NS) {
            cleanup->drain_status = SALTS_ETIMEDOUT;
            break;
        }
    }
    cleanup->owner_quiescent = cleanup->owner->impl == NULL ||
        cflow_io_publisher_owner_is_quiescent(cleanup->owner);
    cleanup->stats_valid = cleanup->adapter->impl != NULL &&
        cflow_io_native_adapter_get_stats(cleanup->adapter, &cleanup->stats);
    cleanup->owner_status = cleanup->owner->impl != NULL
        ? cflow_io_publisher_owner_close(cleanup->owner)
        : SALTS_OK;
    cleanup->adapter_close_status = cleanup->adapter->impl != NULL
        ? cflow_io_native_adapter_close(cleanup->adapter)
        : SALTS_OK;
    native_adapter_test_close_pipe(cleanup->pipes[0]);
    native_adapter_test_close_pipe(cleanup->pipes[1]);
    cleanup->pipes[0] = NATIVE_ADAPTER_TEST_INVALID_PIPE;
    cleanup->pipes[1] = NATIVE_ADAPTER_TEST_INVALID_PIPE;
    for (size_t index = 0u; index < NATIVE_ADAPTER_TEST_CAPACITY; ++index) {
        cleanup->release_status[index] = cleanup->endpoint_attached[index]
            ? cflow_io_native_adapter_release_pipe(
                  cleanup->adapter, cleanup->endpoints[index])
            : SALTS_OK;
    }
    cleanup->destroy_status = cleanup->adapter->impl != NULL
        ? cflow_io_native_adapter_destroy(cleanup->adapter)
        : SALTS_OK;
}

static void native_adapter_test_complete(
    void *completion_user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion) {
    native_adapter_test_completions *probe =
        (native_adapter_test_completions *)completion_user;
    (void)lease_id;
    (void)operation_user;
    if (probe->count >= NATIVE_ADAPTER_TEST_CAPACITY)
        return;
    probe->ids[probe->count] = request_id;
    probe->values[probe->count] = *completion;
    ++probe->count;
}

static int native_adapter_test_drive_until(
    cflow_io_native_adapter *adapter,
    cflow_io_actor *actor,
    cflow_executor *executor,
    native_adapter_test_completions *completions,
    size_t expected) {
    const uint64_t started = cmeta_hrtime();

    while (completions->count < expected) {
        cflow_io_run_result run_result =
            cflow_io_actor_run_ready(actor, NATIVE_ADAPTER_TEST_MAX_STEPS);
        size_t observed = 0u;
        int status;

        if (run_result.status == CFLOW_IO_RUN_INVALID_ARGUMENT)
            return SALTS_EINVAL;
        if (run_result.status == CFLOW_IO_RUN_BUSY)
            return SALTS_EBUSY;
        status = cflow_io_native_adapter_observe(
            adapter, NATIVE_ADAPTER_TEST_POLL_TIMEOUT_MS, &observed);
        if (status != SALTS_OK && status != SALTS_ETIMEDOUT)
            return status;
        run_result = cflow_io_actor_run_ready(actor, NATIVE_ADAPTER_TEST_MAX_STEPS);
        if (run_result.status == CFLOW_IO_RUN_INVALID_ARGUMENT)
            return SALTS_EINVAL;
        if (run_result.status == CFLOW_IO_RUN_BUSY)
            return SALTS_EBUSY;
        (void)cflow_executor_run_ready(executor);
        if (cmeta_hrtime() - started >= NATIVE_ADAPTER_TEST_TIMEOUT_NS)
            return SALTS_ETIMEDOUT;
    }
    return SALTS_OK;
}

static native_adapter_test_publisher_result
native_adapter_test_run_threaded_pipe_publisher(void) {
    static const unsigned char payload[] = {0x51u, 0x52u, 0x53u, 0x54u};
    unsigned char received[sizeof(payload)] = {0};
    native_adapter_test_pipe pipes[NATIVE_ADAPTER_TEST_CAPACITY] = {
        NATIVE_ADAPTER_TEST_INVALID_PIPE,
        NATIVE_ADAPTER_TEST_INVALID_PIPE};
    native_io_endpoint endpoints[NATIVE_ADAPTER_TEST_CAPACITY] = {0};
    cflow_io_native_adapter adapter = {0};
    cflow_io_publisher_owner owner = {0};
    cflow_publisher source = {0};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    native_adapter_test_source_fixture fixture = {0};
    native_adapter_test_threaded_sink_probe sink_probe = {0};
    native_adapter_test_threaded_driver driver = {0};
    native_adapter_test_threaded_setup setup = {0};
    native_adapter_test_threaded_cleanup cleanup = {0};
    native_adapter_test_publisher_result result = {.status = SALTS_OK};
    cmeta_threadpool_t *publisher_pool = NULL;
    const cmeta_threadpool_config_t pool_config = {
        1, NATIVE_ADAPTER_TEST_PUBLISHER_QUEUE_CAPACITY};
    const cflow_io_native_adapter_config adapter_config = {
        {native_adapter_test_backend(), 2u, 2u, 2u}};
    cflow_io_publisher_config source_config = {0};
    cflow_subscriber_callbacks sink_callbacks = {
        native_adapter_test_threaded_sink_value,
        native_adapter_test_threaded_sink_error,
        native_adapter_test_threaded_sink_done,
        &sink_probe};
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&sink_callbacks);
    bool sink_gate_initialized = false;
    bool sink_changed_initialized = false;
    bool driver_gate_initialized = false;
    bool driver_changed_initialized = false;
    bool surface_initialized = false;
    bool normalized_initialized = false;
    bool scheduler_initialized = false;
    bool subscription_open = false;
    bool driver_started = false;
    int status;

    native_adapter_test_thread_role = NATIVE_ADAPTER_TEST_THREAD_ROLE_MAIN;
    atomic_init(&fixture.prepared, 0u);
    atomic_init(&fixture.encoded, 0u);
    atomic_init(&fixture.release_count, 0u);
    fixture.prepare_expected_role =
        NATIVE_ADAPTER_TEST_THREAD_ROLE_SUBSCRIBER;
    fixture.encode_expected_role = NATIVE_ADAPTER_TEST_THREAD_ROLE_PUBLISHER;
    atomic_init(&fixture.prepare_callbacks, 0);
    atomic_init(&fixture.encode_callbacks, 0);
    atomic_init(&fixture.prepare_role_collisions, 0);
    atomic_init(&fixture.encode_role_collisions, 0);
    atomic_init(&sink_probe.subscriber_callbacks, 0);
    atomic_init(&sink_probe.role_collisions, 0);
    atomic_init(&driver.wake_status, SALTS_OK);
    atomic_init(&driver.drive_status, SALTS_OK);
    atomic_init(&driver.drive_cycles, 0u);
    atomic_init(&driver.observed, 0u);
    atomic_init(&driver.publisher_callbacks, 0);
    atomic_init(&driver.role_collisions, 0);

    cmeta_mutex_init(&sink_probe.gate);
    sink_gate_initialized = sink_probe.gate != NULL;
    if (!sink_gate_initialized) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }
    cmeta_cond_init(&sink_probe.changed);
    sink_changed_initialized = sink_probe.changed != NULL;
    if (!sink_changed_initialized) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }
    cmeta_mutex_init(&driver.gate);
    driver_gate_initialized = driver.gate != NULL;
    if (!driver_gate_initialized) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }
    cmeta_cond_init(&driver.changed);
    driver_changed_initialized = driver.changed != NULL;
    if (!driver_changed_initialized) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }
    status = native_adapter_test_make_pipe_pair(pipes);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    publisher_pool = cmeta_threadpool_create_with_config(&pool_config);
    if (publisher_pool == NULL) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }

    setup.adapter = &adapter;
    setup.config = &adapter_config;
    setup.pipes = pipes;
    setup.endpoints = endpoints;
    setup.status = SALTS_EINVAL;
    status = cmeta_threadpool_submit(
        publisher_pool, native_adapter_test_threaded_setup_task, &setup);
    native_adapter_test_keep_first_status(&result.status, status);
    if (status != SALTS_OK)
        goto cleanup;
    status = cmeta_threadpool_wait_status(publisher_pool);
    native_adapter_test_keep_first_status(&result.status, status);
    native_adapter_test_keep_first_status(&result.status, setup.status);
    if (status != SALTS_OK || setup.status != SALTS_OK)
        goto cleanup;

    fixture.operation_count = NATIVE_ADAPTER_TEST_CAPACITY;
    fixture.operations[0].native = (native_io_operation){
        .kind = NATIVE_IO_OPERATION_PIPE_READ,
        .endpoint = endpoints[0],
        .buffer = received,
        .length = sizeof(received)};
    fixture.operations[0].release_count = &fixture.release_count;
    fixture.operations[0].release_gate = &sink_probe.gate;
    fixture.operations[0].release_changed = &sink_probe.changed;
    fixture.operations[1].native = (native_io_operation){
        .kind = NATIVE_IO_OPERATION_PIPE_WRITE,
        .endpoint = endpoints[1],
        .buffer = (void *)payload,
        .length = sizeof(payload)};
    fixture.operations[1].release_count = &fixture.release_count;
    fixture.operations[1].release_gate = &sink_probe.gate;
    fixture.operations[1].release_changed = &sink_probe.changed;
    source_config.name = "native-io-windowed-pipe-publisher";
    source_config.type = &cmeta_type_int;
    source_config.backend = cflow_io_native_adapter_actor_ops();
    source_config.backend_user = &adapter;
    source_config.prepare = native_adapter_test_source_prepare;
    source_config.encode = native_adapter_test_source_encode;
    source_config.user = &fixture;
    driver.adapter = &adapter;
    driver.owner = &owner;
    source_config.drive = native_adapter_test_threaded_drive;
    source_config.drive_user = &driver;

    cflow_graph_init(&surface, &cmeta_type_int);
    surface_initialized = true;
    if (!cflow_graph_normalize(&normalized, &surface)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    normalized_initialized = true;
    if (!cflow_scheduler_worker_init_with_capacity(&scheduler, 1u, 8u, 1u)) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }
    scheduler_initialized = true;
    status = cflow_publisher_from_io_actor_windowed(
        &source, &owner, &source_config, NATIVE_ADAPTER_TEST_CAPACITY);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    if (!cflow_subscribe(&run, &normalized, &source, &scheduler, &sink)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    subscription_open = true;
    status = cmeta_threadpool_submit(
        publisher_pool, native_adapter_test_threaded_drive_task, &driver);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    driver_started = true;

    if (!cflow_subscription_request(&run, NATIVE_ADAPTER_TEST_CAPACITY)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    if (!native_adapter_test_threaded_sink_wait(
            &sink_probe, NATIVE_ADAPTER_TEST_CAPACITY, 0u)) {
        result.status = SALTS_ETIMEDOUT;
        goto cleanup;
    }
    if (!cflow_scheduler_wait_idle(&scheduler)) {
        result.status = SALTS_EBUSY;
        goto cleanup;
    }
    if (!native_adapter_test_threaded_release_wait(
            &sink_probe, &fixture.release_count,
            NATIVE_ADAPTER_TEST_CAPACITY)) {
        result.status = SALTS_ETIMEDOUT;
        goto cleanup;
    }
    result.payload_equal =
        memcmp(received, payload, sizeof(payload)) == 0;

    if (!cflow_subscription_request(&run, 1u)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    if (!native_adapter_test_threaded_sink_wait(
            &sink_probe, NATIVE_ADAPTER_TEST_CAPACITY, 1u)) {
        result.status = SALTS_ETIMEDOUT;
        goto cleanup;
    }
    if (!cflow_scheduler_wait_idle(&scheduler)) {
        result.status = SALTS_EBUSY;
        goto cleanup;
    }
    result.subscription_done = cflow_subscription_is_done(&run);

cleanup:
    if (subscription_open) {
        cflow_subscription_close(&run);
        subscription_open = false;
        if (scheduler_initialized && !cflow_scheduler_wait_idle(&scheduler))
            native_adapter_test_keep_first_status(
                &result.status, SALTS_EBUSY);
    } else if (cflow_publisher_valid(&source)) {
        cflow_publisher_destroy(&source);
    }
    if (driver_started) {
        native_adapter_test_threaded_stop(&driver);
        status = cmeta_threadpool_wait_status(publisher_pool);
        native_adapter_test_keep_first_status(&result.status, status);
        driver_started = false;
    }
    if (publisher_pool != NULL) {
        cleanup.adapter = &adapter;
        cleanup.owner = &owner;
        cleanup.pipes = pipes;
        cleanup.endpoints = endpoints;
        cleanup.endpoint_attached[0] = setup.endpoint_attached[0];
        cleanup.endpoint_attached[1] = setup.endpoint_attached[1];
        cleanup.owner_status = SALTS_EINVAL;
        cleanup.adapter_close_status = SALTS_EINVAL;
        cleanup.release_status[0] = SALTS_EINVAL;
        cleanup.release_status[1] = SALTS_EINVAL;
        cleanup.destroy_status = SALTS_EINVAL;
        status = cmeta_threadpool_submit(
            publisher_pool, native_adapter_test_threaded_cleanup_task,
            &cleanup);
        native_adapter_test_keep_first_status(&result.status, status);
        if (status == SALTS_OK) {
            status = cmeta_threadpool_wait_status(publisher_pool);
            native_adapter_test_keep_first_status(&result.status, status);
            native_adapter_test_keep_first_status(
                &result.status, cleanup.drain_status);
            native_adapter_test_keep_first_status(
                &result.status, cleanup.owner_status);
            native_adapter_test_keep_first_status(
                &result.status, cleanup.adapter_close_status);
            native_adapter_test_keep_first_status(
                &result.status, cleanup.release_status[0]);
            native_adapter_test_keep_first_status(
                &result.status, cleanup.release_status[1]);
            native_adapter_test_keep_first_status(
                &result.status, cleanup.destroy_status);
        }
        cmeta_threadpool_shutdown(publisher_pool);
        cmeta_threadpool_destroy(publisher_pool);
    }
    native_adapter_test_close_pipe(pipes[0]);
    native_adapter_test_close_pipe(pipes[1]);
    if (scheduler_initialized)
        cflow_scheduler_destroy(&scheduler);
    if (normalized_initialized)
        cflow_graph_destroy(&normalized);
    if (surface_initialized)
        cflow_graph_destroy(&surface);

    result.prepared = atomic_load(&fixture.prepared);
    result.encoded = atomic_load(&fixture.encoded);
    result.released = atomic_load(&fixture.release_count);
    result.values = sink_probe.value_count;
    result.first_value = sink_probe.values[0];
    result.second_value = sink_probe.values[1];
    result.errors = sink_probe.error_count;
    result.dones = sink_probe.done_count;
    result.publisher_callbacks = atomic_load(&driver.publisher_callbacks);
    result.subscriber_callbacks = atomic_load(&sink_probe.subscriber_callbacks);
    result.publisher_role_collisions = atomic_load(&driver.role_collisions);
    result.subscriber_role_collisions = atomic_load(&sink_probe.role_collisions);
    result.prepare_callbacks = atomic_load(&fixture.prepare_callbacks);
    result.encode_callbacks = atomic_load(&fixture.encode_callbacks);
    result.prepare_role_collisions =
        atomic_load(&fixture.prepare_role_collisions);
    result.encode_role_collisions = atomic_load(&fixture.encode_role_collisions);
    result.wake_status = atomic_load(&driver.wake_status);
    result.drive_status = atomic_load(&driver.drive_status);
    result.drive_cycles = atomic_load(&driver.drive_cycles);
    result.observed = atomic_load(&driver.observed);
    result.owner_drained = cleanup.owner_quiescent;
    result.active_bridges = cleanup.stats_valid
        ? cleanup.stats.active_bridges
        : SIZE_MAX;
    result.active_native_requests = cleanup.stats_valid
        ? cleanup.stats.native.active_requests
        : SIZE_MAX;
    result.cancelled_native_requests = cleanup.stats_valid
        ? cleanup.stats.native.cancelled
        : UINT64_MAX;

    if (driver_changed_initialized)
        cmeta_cond_destroy(&driver.changed);
    if (driver_gate_initialized)
        cmeta_mutex_destroy(&driver.gate);
    if (sink_changed_initialized)
        cmeta_cond_destroy(&sink_probe.changed);
    if (sink_gate_initialized)
        cmeta_mutex_destroy(&sink_probe.gate);
    native_adapter_test_thread_role = NATIVE_ADAPTER_TEST_THREAD_ROLE_NONE;
    return result;
}

static native_adapter_test_publisher_result
native_adapter_test_run_pending_pipe_close(void) {
    unsigned char received = 0u;
    native_adapter_test_pipe pipes[NATIVE_ADAPTER_TEST_CAPACITY] = {
        NATIVE_ADAPTER_TEST_INVALID_PIPE,
        NATIVE_ADAPTER_TEST_INVALID_PIPE};
    native_io_endpoint endpoint = {0};
    cflow_io_native_adapter adapter = {0};
    cflow_io_publisher_owner owner = {0};
    cflow_publisher source = {0};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    native_adapter_test_source_fixture fixture = {0};
    native_adapter_test_sink_probe sink_probe = {0};
    native_adapter_test_publisher_result result = {.status = SALTS_OK};
    cflow_subscriber_callbacks sink_callbacks = {
        native_adapter_test_sink_value,
        native_adapter_test_sink_error,
        native_adapter_test_sink_done,
        &sink_probe};
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&sink_callbacks);
    cflow_io_native_adapter_stats adapter_stats = {0};
    const cflow_io_native_adapter_config adapter_config = {
        {native_adapter_test_backend(), 1u, 1u, 1u}};
    cflow_io_publisher_config source_config = {0};
    bool pipes_created = false;
    bool adapter_initialized = false;
    bool endpoint_attached = false;
    bool surface_initialized = false;
    bool normalized_initialized = false;
    bool scheduler_initialized = false;
    bool subscription_open = false;
    bool stats_valid = false;
    uint64_t started;
    int status;

    atomic_init(&fixture.prepared, 0u);
    atomic_init(&fixture.encoded, 0u);
    atomic_init(&fixture.release_count, 0u);
    atomic_init(&fixture.prepare_callbacks, 0);
    atomic_init(&fixture.encode_callbacks, 0);
    atomic_init(&fixture.prepare_role_collisions, 0);
    atomic_init(&fixture.encode_role_collisions, 0);
    status = native_adapter_test_make_pipe_pair(pipes);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    pipes_created = true;
    status = cflow_io_native_adapter_init(&adapter, &adapter_config);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    adapter_initialized = true;
    status = cflow_io_native_adapter_attach_pipe(
        &adapter, (uintptr_t)pipes[0],
        NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    endpoint_attached = true;

    fixture.operation_count = 1u;
    fixture.operations[0].native = (native_io_operation){
        .kind = NATIVE_IO_OPERATION_PIPE_READ,
        .endpoint = endpoint,
        .buffer = &received,
        .length = sizeof(received)};
    fixture.operations[0].release_count = &fixture.release_count;
    source_config.name = "native-io-cancelled-pipe-publisher";
    source_config.type = &cmeta_type_int;
    source_config.backend = cflow_io_native_adapter_actor_ops();
    source_config.backend_user = &adapter;
    source_config.prepare = native_adapter_test_source_prepare;
    source_config.encode = native_adapter_test_source_encode;
    source_config.user = &fixture;

    cflow_graph_init(&surface, &cmeta_type_int);
    surface_initialized = true;
    if (!cflow_graph_normalize(&normalized, &surface)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    normalized_initialized = true;
    if (!cflow_scheduler_test_init(&scheduler)) {
        result.status = SALTS_ENOMEM;
        goto cleanup;
    }
    scheduler_initialized = true;
    status = cflow_publisher_from_io_actor_windowed(
        &source, &owner, &source_config, 1u);
    if (status != SALTS_OK) {
        result.status = status;
        goto cleanup;
    }
    if (!cflow_subscribe(&run, &normalized, &source, &scheduler, &sink)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    subscription_open = true;
    if (!cflow_subscription_request(&run, 1u)) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    {
        size_t progressed = 0u;
        const int owner_status = cflow_io_publisher_owner_run_ready(
            &owner, 32u, &progressed);

        if (owner_status != SALTS_OK || progressed == 0u) {
            result.status = owner_status != SALTS_OK
                ? owner_status
                : SALTS_EPROTO;
            goto cleanup;
        }
    }
    if (!cflow_io_native_adapter_get_stats(&adapter, &adapter_stats) ||
        adapter_stats.active_bridges != 1u ||
        adapter_stats.native.active_requests != 1u) {
        result.status = SALTS_EPROTO;
        goto cleanup;
    }

    cflow_subscription_close(&run);
    subscription_open = false;

cleanup:
    if (subscription_open) {
        cflow_subscription_close(&run);
        subscription_open = false;
    } else if (cflow_publisher_valid(&source)) {
        cflow_publisher_destroy(&source);
    }
    if (owner.impl != NULL && adapter_initialized) {
        started = cmeta_hrtime();
        while (!cflow_io_publisher_owner_is_quiescent(&owner)) {
            size_t completed = 0u;
            const int drive_status = cflow_io_native_adapter_drive_publisher(
                &adapter, &owner, NATIVE_ADAPTER_TEST_POLL_TIMEOUT_MS,
                NATIVE_ADAPTER_TEST_MAX_STEPS, &completed);

            result.observed += completed;
            if (drive_status != SALTS_OK && drive_status != SALTS_ETIMEDOUT) {
                native_adapter_test_keep_first_status(
                    &result.status, drive_status);
                break;
            }
            if (cmeta_hrtime() - started >= NATIVE_ADAPTER_TEST_TIMEOUT_NS) {
                native_adapter_test_keep_first_status(
                    &result.status, SALTS_ETIMEDOUT);
                break;
            }
        }
        result.owner_drained = cflow_io_publisher_owner_is_quiescent(&owner);
        stats_valid = cflow_io_native_adapter_get_stats(
            &adapter, &adapter_stats);
        status = cflow_io_publisher_owner_close(&owner);
        native_adapter_test_keep_first_status(&result.status, status);
    }
    if (adapter_initialized) {
        status = cflow_io_native_adapter_close(&adapter);
        native_adapter_test_keep_first_status(&result.status, status);
    }
    if (pipes_created) {
        native_adapter_test_close_pipe(pipes[0]);
        native_adapter_test_close_pipe(pipes[1]);
        pipes[0] = NATIVE_ADAPTER_TEST_INVALID_PIPE;
        pipes[1] = NATIVE_ADAPTER_TEST_INVALID_PIPE;
    }
    if (endpoint_attached) {
        status = cflow_io_native_adapter_release_pipe(&adapter, endpoint);
        native_adapter_test_keep_first_status(&result.status, status);
    }
    if (adapter_initialized) {
        status = cflow_io_native_adapter_destroy(&adapter);
        native_adapter_test_keep_first_status(&result.status, status);
    }
    if (scheduler_initialized)
        cflow_scheduler_destroy(&scheduler);
    if (normalized_initialized)
        cflow_graph_destroy(&normalized);
    if (surface_initialized)
        cflow_graph_destroy(&surface);

    result.prepared = atomic_load(&fixture.prepared);
    result.encoded = atomic_load(&fixture.encoded);
    result.released = atomic_load(&fixture.release_count);
    result.values = sink_probe.value_count;
    result.errors = sink_probe.error_count;
    result.dones = sink_probe.done_count;
    result.active_bridges = stats_valid
        ? adapter_stats.active_bridges
        : SIZE_MAX;
    result.active_native_requests = stats_valid
        ? adapter_stats.native.active_requests
        : SIZE_MAX;
    result.cancelled_native_requests = stats_valid
        ? adapter_stats.native.cancelled
        : UINT64_MAX;
    return result;
}

spec("CFlow NativeIO adapter pipe lifecycle") {
    it("rejects malformed capacity without publishing adapter state") {
        cflow_io_native_adapter adapter = {0};
        const cflow_io_native_adapter_config config = {
            {native_adapter_test_backend(), 1u, 1u, 0u}};

        check_equal(cflow_io_native_adapter_init(&adapter, &config), SALTS_EINVAL);
        check_null(adapter.impl);
    }

    it("round trips a pipe payload and releases operations after acknowledgement") {
        static const unsigned char payload = 0x41u;
        unsigned char received = 0u;
        native_adapter_test_pipe pipes[2];
        native_io_endpoint endpoints[2] = {0};
        cflow_io_native_adapter adapter = {0};
        cflow_executor executor = {0};
        cflow_io_actor actor = {0};
        native_adapter_test_completions completions = {0};
        size_t release_count = 0u;
        const cflow_io_native_adapter_config adapter_config = {
            {native_adapter_test_backend(), 2u, 2u, 2u}};
        cflow_io_actor_config actor_config = {0};
        native_adapter_test_operation operations[2] = {
            {{.kind = NATIVE_IO_OPERATION_PIPE_READ,
              .buffer = &received,
              .length = sizeof(received)},
             &release_count},
            {{.kind = NATIVE_IO_OPERATION_PIPE_WRITE,
              .buffer = (void *)&payload,
              .length = sizeof(payload)},
             &release_count}};
        cflow_io_operation actor_operations[2] = {
            {&operations[0], native_adapter_test_release},
            {&operations[1], native_adapter_test_release}};
        cflow_io_submit_result submitted[2];

        check_equal(cflow_io_native_adapter_init(&adapter, &adapter_config), SALTS_OK);
        check_equal(native_adapter_test_make_pipe_pair(pipes), SALTS_OK);
        check_equal(cflow_io_native_adapter_attach_pipe(
                        &adapter, (uintptr_t)pipes[0],
                        NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[0]),
                    SALTS_OK);
        check_equal(cflow_io_native_adapter_attach_pipe(
                        &adapter, (uintptr_t)pipes[1],
                        NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoints[1]),
                    SALTS_OK);
        operations[0].native.endpoint = endpoints[0];
        operations[1].native.endpoint = endpoints[1];
        check_true(cflow_executor_manual_init_with_capacity(&executor, 2u));
        actor_config.request_capacity = 2u;
        actor_config.command_capacity = 2u;
        actor_config.executor = &executor;
        actor_config.backend = cflow_io_native_adapter_actor_ops();
        actor_config.backend_user = &adapter;
        actor_config.completion = native_adapter_test_complete;
        actor_config.completion_user = &completions;
        check_equal(cflow_io_actor_init(&actor, &actor_config), SALTS_OK);

        submitted[0] = cflow_io_actor_try_submit(&actor, 101u, &actor_operations[0]);
        submitted[1] = cflow_io_actor_try_submit(&actor, 102u, &actor_operations[1]);
        check_equal(submitted[0].status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(submitted[1].status, CFLOW_IO_SUBMIT_ACCEPTED);
        check_equal(native_adapter_test_drive_until(
                        &adapter, &actor, &executor, &completions, 2u),
                    SALTS_OK);
        check_equal(received, payload);
        check_equal(completions.count, 2u);
        check_equal(release_count, 0u);
        for (size_t index = 0u; index < completions.count; ++index) {
            check_equal(completions.values[index].kind, CFLOW_IO_COMPLETION_OK);
            check_equal(cflow_io_actor_acknowledge(&actor, completions.ids[index]),
                        CFLOW_IO_ACK_RELEASED);
        }
        check_equal(release_count, 2u);

        check_equal(cflow_io_actor_close(&actor), SALTS_OK);
        check_true(cflow_io_actor_is_quiescent(&actor));
        check_equal(cflow_io_actor_destroy(&actor), SALTS_OK);
        check_equal(cflow_io_native_adapter_close(&adapter), SALTS_OK);
        native_adapter_test_close_pipe(pipes[0]);
        native_adapter_test_close_pipe(pipes[1]);
        check_equal(cflow_io_native_adapter_release_pipe(&adapter, endpoints[0]), SALTS_OK);
        check_equal(cflow_io_native_adapter_release_pipe(&adapter, endpoints[1]), SALTS_OK);
        check_equal(cflow_io_native_adapter_destroy(&adapter), SALTS_OK);
        check_true(cflow_executor_shutdown(&executor));
        cflow_executor_destroy(&executor);
    }

    it("keeps a cancelled pipe identity until close drain and acknowledgement") {
        unsigned char received = 0u;
        native_adapter_test_pipe pipes[2];
        native_io_endpoint endpoint = {0};
        cflow_io_native_adapter adapter = {0};
        cflow_io_native_adapter_stats stats = {0};
        cflow_executor executor = {0};
        cflow_io_actor actor = {0};
        native_adapter_test_completions completions = {0};
        size_t release_count = 0u;
        const cflow_io_native_adapter_config adapter_config = {
            {native_adapter_test_backend(), 1u, 1u, 1u}};
        cflow_io_actor_config actor_config = {0};
        native_adapter_test_operation operation = {
            {.kind = NATIVE_IO_OPERATION_PIPE_READ,
             .buffer = &received,
             .length = sizeof(received)},
            &release_count};
        cflow_io_operation actor_operation = {&operation, native_adapter_test_release};
        cflow_io_submit_result submitted;

        check_equal(cflow_io_native_adapter_init(&adapter, &adapter_config), SALTS_OK);
        check_equal(native_adapter_test_make_pipe_pair(pipes), SALTS_OK);
        check_equal(cflow_io_native_adapter_attach_pipe(
                        &adapter, (uintptr_t)pipes[0],
                        NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint),
                    SALTS_OK);
        operation.native.endpoint = endpoint;
        check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
        actor_config.request_capacity = 1u;
        actor_config.command_capacity = 2u;
        actor_config.executor = &executor;
        actor_config.backend = cflow_io_native_adapter_actor_ops();
        actor_config.backend_user = &adapter;
        actor_config.completion = native_adapter_test_complete;
        actor_config.completion_user = &completions;
        check_equal(cflow_io_actor_init(&actor, &actor_config), SALTS_OK);

        submitted = cflow_io_actor_try_submit(&actor, 201u, &actor_operation);
        check_equal(submitted.status, CFLOW_IO_SUBMIT_ACCEPTED);
        (void)cflow_io_actor_run_ready(&actor, NATIVE_ADAPTER_TEST_MAX_STEPS);
        check_true(cflow_io_native_adapter_get_stats(&adapter, &stats));
        check_equal(stats.active_bridges, 1u);
        check_equal(stats.native.active_requests, 1u);
        check_equal(cflow_io_native_adapter_close(&adapter), SALTS_OK);
        check_equal(cflow_io_native_adapter_destroy(&adapter), SALTS_EBUSY);

        check_equal(cflow_io_actor_try_cancel(&actor, submitted.request_id),
                    CFLOW_IO_CANCEL_ACCEPTED);
        check_equal(native_adapter_test_drive_until(
                        &adapter, &actor, &executor, &completions, 1u),
                    SALTS_OK);
        check_equal(completions.count, 1u);
        check_equal(completions.values[0].kind, CFLOW_IO_COMPLETION_CANCELLED);
        check_equal(release_count, 0u);
        check_equal(cflow_io_actor_acknowledge(&actor, completions.ids[0]),
                    CFLOW_IO_ACK_RELEASED);
        check_equal(release_count, 1u);
        check_true(cflow_io_native_adapter_get_stats(&adapter, &stats));
        check_equal(stats.active_bridges, 0u);
        check_equal(stats.native.active_requests, 0u);

        check_equal(cflow_io_actor_close(&actor), SALTS_OK);
        check_true(cflow_io_actor_is_quiescent(&actor));
        check_equal(cflow_io_actor_destroy(&actor), SALTS_OK);
        native_adapter_test_close_pipe(pipes[0]);
        native_adapter_test_close_pipe(pipes[1]);
        check_equal(cflow_io_native_adapter_release_pipe(&adapter, endpoint), SALTS_OK);
        check_equal(cflow_io_native_adapter_destroy(&adapter), SALTS_OK);
        check_true(cflow_executor_shutdown(&executor));
        cflow_executor_destroy(&executor);
    }

    it("runs a Pipe Publisher and Subscriber on separate bounded workers") {
        const native_adapter_test_publisher_result result =
            native_adapter_test_run_threaded_pipe_publisher();

        check_equal(result.status, SALTS_OK);
        check_true(result.payload_equal);
        check_true(result.subscription_done);
        check_equal(result.prepared, (size_t)NATIVE_ADAPTER_TEST_CAPACITY);
        check_equal(result.encoded, (size_t)NATIVE_ADAPTER_TEST_CAPACITY);
        check_equal(result.released, (size_t)NATIVE_ADAPTER_TEST_CAPACITY);
        check_equal(result.values, (size_t)NATIVE_ADAPTER_TEST_CAPACITY);
        check_equal(result.first_value, 4);
        check_equal(result.second_value, 4);
        check_equal(result.errors, 0u);
        check_equal(result.dones, 1u);
        check_true(result.publisher_callbacks > 0);
        check_true(result.subscriber_callbacks > 0);
        check_equal(result.publisher_role_collisions, 0);
        check_equal(result.subscriber_role_collisions, 0);
        check_equal(result.prepare_callbacks, 3);
        check_equal(result.encode_callbacks, 2);
        check_equal(result.prepare_role_collisions, 0);
        check_equal(result.encode_role_collisions, 0);
        check_equal(result.wake_status, SALTS_OK);
        check_equal(result.drive_status, SALTS_OK);
        check_true(result.drive_cycles > 0u);
        check_equal(result.observed,
                    (size_t)NATIVE_ADAPTER_TEST_CAPACITY);
        check_true(result.owner_drained);
        check_equal(result.active_bridges, 0u);
        check_equal(result.active_native_requests, 0u);
        check_equal(result.cancelled_native_requests, (uint64_t)0u);
    }

    it("drains a pending Pipe Publisher terminal after Subscription close") {
        const native_adapter_test_publisher_result result =
            native_adapter_test_run_pending_pipe_close();

        check_equal(result.status, SALTS_OK);
        check_equal(result.prepared, 1u);
        check_equal(result.encoded, 0u);
        check_equal(result.released, 1u);
        check_equal(result.values, 0u);
        check_equal(result.errors, 0u);
        check_equal(result.dones, 0u);
        check_equal(result.observed, 1u);
        check_true(result.owner_drained);
        check_equal(result.active_bridges, 0u);
        check_equal(result.active_native_requests, 0u);
        check_equal(result.cancelled_native_requests, (uint64_t)1u);
    }
}
