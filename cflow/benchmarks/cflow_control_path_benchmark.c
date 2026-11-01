#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cflow/cflow.h>
#include <cflow/io_native_adapter.h>

#include <clock.h>
#include <error_codes.h>
#include <native_io.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#if defined(interface)
#undef interface
#endif
#include <windows.h>
typedef HANDLE control_pipe;
#define CONTROL_INVALID_PIPE INVALID_HANDLE_VALUE
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
typedef int control_pipe;
#define CONTROL_INVALID_PIPE (-1)
#endif

enum {
  CONTROL_WARMUP_VALUES = 64,
#if defined(_WIN32)
  /*
   * GetProcessTimes() accounts CPU in scheduler quanta on hosted Windows.
   * One longer measured chunk keeps peer prefill outside timing while making
   * Direct/Actor CPU-efficiency observable instead of rounding to zero.
   */
  CONTROL_MEASURED_VALUES = 32768,
  CONTROL_CHUNK_VALUES = 32768,
#else
  CONTROL_MEASURED_VALUES = 4096,
  CONTROL_CHUNK_VALUES = 256,
#endif
  CONTROL_REPLICATES = 7,
  CONTROL_MAX_STEPS = 64,
  CONTROL_MAX_DRIVE_ROUNDS = 100000,
  CONTROL_ACTOR_LEASE_ID = 1
};

typedef struct control_layer_summary {
  const char *layer;
  double p50_ns;
  double p95_ns;
  double p99_ns;
  double median_values_per_second;
  double median_cpu_percent;
  double median_values_per_cpu_second;
  uint64_t accepted;
  uint64_t completed;
  uint64_t rejected;
  uint64_t stale;
} control_layer_summary;

typedef struct control_actor_operation {
  native_io_operation native;
  uint64_t started_ns;
  uint64_t *latency_out;
  cflow_io_request_id request_id;
  size_t *released;
  unsigned char byte;
  int status;
  bool completed;
} control_actor_operation;

typedef struct control_publisher_operation {
  native_io_operation native;
  uint64_t started_ns;
  uint64_t *latency_out;
  size_t *released;
  size_t index;
  unsigned char byte;
} control_publisher_operation;

typedef struct control_publisher_fixture {
  control_publisher_operation *operations;
  size_t operation_count;
  size_t prepared;
  size_t encoded;
  size_t values;
  size_t errors;
  size_t dones;
  const char *error;
} control_publisher_fixture;

static native_io_backend_kind control_backend(void) {
  const char *value = getenv("CFLOW_CONTROL_BASELINE_BACKEND");

  if (value == NULL || *value == '\0') {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
  }
  if (strcmp(value, "epoll") == 0) return NATIVE_IO_BACKEND_EPOLL;
  if (strcmp(value, "io_uring") == 0) return NATIVE_IO_BACKEND_IO_URING;
  if (strcmp(value, "iocp") == 0) return NATIVE_IO_BACKEND_IOCP;
  if (strcmp(value, "kqueue") == 0) return NATIVE_IO_BACKEND_KQUEUE;
  return (native_io_backend_kind)0;
}

static const char *control_backend_name(native_io_backend_kind kind) {
  switch (kind) {
    case NATIVE_IO_BACKEND_EPOLL: return "epoll";
    case NATIVE_IO_BACKEND_IO_URING: return "io_uring";
    case NATIVE_IO_BACKEND_IOCP: return "iocp";
    case NATIVE_IO_BACKEND_KQUEUE: return "kqueue";
    default: return "unsupported";
  }
}

static uint64_t control_process_cpu_ns(void) {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  ULARGE_INTEGER kernel_ticks, user_ticks;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
    return 0u;
  kernel_ticks.LowPart = kernel.dwLowDateTime;
  kernel_ticks.HighPart = kernel.dwHighDateTime;
  user_ticks.LowPart = user.dwLowDateTime;
  user_ticks.HighPart = user.dwHighDateTime;
  return (kernel_ticks.QuadPart + user_ticks.QuadPart) * UINT64_C(100);
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0u;
  return ((uint64_t)usage.ru_utime.tv_sec +
          (uint64_t)usage.ru_stime.tv_sec) * UINT64_C(1000000000) +
         ((uint64_t)usage.ru_utime.tv_usec +
          (uint64_t)usage.ru_stime.tv_usec) * UINT64_C(1000);
#endif
}

static int control_u64_compare(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static int control_double_compare(const void *left, const void *right) {
  const double a = *(const double *)left;
  const double b = *(const double *)right;
  return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t control_u64_percentile(uint64_t *values, size_t count,
                                       unsigned percentile) {
  size_t rank;
  qsort(values, count, sizeof(*values), control_u64_compare);
  rank = ((size_t)percentile * count + 99u) / 100u;
  if (rank == 0u) rank = 1u;
  return values[rank - 1u];
}

static double control_double_median(double *values, size_t count) {
  qsort(values, count, sizeof(*values), control_double_compare);
  return values[count / 2u];
}

static void control_close_pipe(control_pipe value) {
#if defined(_WIN32)
  if (value != NULL && value != INVALID_HANDLE_VALUE)
    (void)CloseHandle(value);
#else
  if (value >= 0)
    (void)close(value);
#endif
}

#if defined(_WIN32)
static int control_make_pipe_pair(control_pipe pipes[2]) {
  wchar_t name[128];
  OVERLAPPED connected = {0};
  HANDLE event = NULL;
  DWORD error = ERROR_SUCCESS;
  BOOL pending = FALSE;

  pipes[0] = INVALID_HANDLE_VALUE;
  pipes[1] = INVALID_HANDLE_VALUE;
  if (_snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                   L"\\\\.\\pipe\\cflow-control-baseline-%lu-%llu",
                   GetCurrentProcessId(),
                   (unsigned long long)cmeta_hrtime()) < 0)
    return SALTS_ERANGE;
  pipes[0] = CreateNamedPipeW(
      name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1u,
      65536u, 65536u, 0u, NULL);
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

  pipes[1] = CreateFileW(
      name, GENERIC_WRITE, 0u, NULL, OPEN_EXISTING, 0u, NULL);
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
  control_close_pipe(pipes[1]);
  control_close_pipe(pipes[0]);
  if (event != NULL) (void)CloseHandle(event);
  pipes[0] = INVALID_HANDLE_VALUE;
  pipes[1] = INVALID_HANDLE_VALUE;
  return -(int)error;
}
#else
static int control_set_nonblocking(int fd) {
  int flags;
  do {
    flags = fcntl(fd, F_GETFL);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) return -errno;
  while (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    if (errno != EINTR) return -errno;
  }
  return SALTS_OK;
}

static int control_make_pipe_pair(control_pipe pipes[2]) {
  int status;

  pipes[0] = -1;
  pipes[1] = -1;
  if (pipe(pipes) != 0) return -errno;
  status = control_set_nonblocking(pipes[0]);
  if (status != SALTS_OK) {
    control_close_pipe(pipes[0]);
    control_close_pipe(pipes[1]);
    pipes[0] = -1;
    pipes[1] = -1;
  }
  return status;
}
#endif

static int control_prefill(control_pipe writer, size_t count) {
  unsigned char bytes[CONTROL_CHUNK_VALUES];
  size_t offset = 0u;

  if (count > sizeof(bytes)) return SALTS_EINVAL;
  memset(bytes, 0x5au, count);
#if defined(_WIN32)
  while (offset < count) {
    DWORD written = 0u;
    if (!WriteFile(writer, bytes + offset, (DWORD)(count - offset),
                   &written, NULL))
      return -(int)GetLastError();
    if (written == 0u) return SALTS_EIO;
    offset += (size_t)written;
  }
#else
  while (offset < count) {
    ssize_t written = write(writer, bytes + offset, count - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written < 0) return -errno;
    if (written == 0) return SALTS_EIO;
    offset += (size_t)written;
  }
#endif
  return SALTS_OK;
}

static int control_direct_read(native_io_backend *backend,
                               native_io_endpoint endpoint,
                               unsigned char *byte,
                               uint64_t *latency_out) {
  native_io_operation operation = {
      .kind = NATIVE_IO_OPERATION_PIPE_READ,
      .endpoint = endpoint,
      .buffer = byte,
      .length = 1u};
  native_io_request request = {0};
  native_io_completion completion = {0};
  size_t count = 0u;
  uint64_t started;
  int status;

  *byte = 0u;
  started = cmeta_hrtime();
  status = native_io_backend_submit(backend, &operation, &request);
  if (status != SALTS_OK) return status;
  status = native_io_backend_observe(
      backend, &completion, 1u, 1000u, &count);
  if (status != SALTS_OK) return status;
  if (count != 1u || completion.kind != NATIVE_IO_COMPLETION_OK ||
      completion.bytes != 1u || *byte != 0x5au)
    return SALTS_EPROTO;
  *latency_out = cmeta_hrtime() - started;
  return SALTS_OK;
}

static int control_direct_replicate(native_io_backend_kind kind,
                                    uint64_t *latencies,
                                    double *rate,
                                    double *cpu_percent,
                                    double *cpu_efficiency,
                                    uint64_t *accepted,
                                    uint64_t *completed,
                                    uint64_t *rejected,
                                    uint64_t *stale) {
  control_pipe pipes[2] = {CONTROL_INVALID_PIPE, CONTROL_INVALID_PIPE};
  native_io_backend backend = {0};
  native_io_endpoint endpoint = {0};
  native_io_backend_stats stats = {0};
  native_io_backend_config config = {kind, 1u, 1u, 1u};
  unsigned char byte = 0u;
  uint64_t wall_ns = 0u;
  uint64_t cpu_ns = 0u;
  size_t measured = 0u;
  int status;

  status = control_make_pipe_pair(pipes);
  if (status != SALTS_OK) goto cleanup;
  status = native_io_backend_init(&backend, &config);
  if (status != SALTS_OK) goto cleanup;
  status = native_io_backend_attach_pipe(
      &backend, (uintptr_t)pipes[0],
      NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint);
  if (status != SALTS_OK) goto cleanup;

  for (size_t offset = 0u; offset < CONTROL_WARMUP_VALUES;
       offset += CONTROL_CHUNK_VALUES) {
    const size_t chunk =
        CONTROL_WARMUP_VALUES - offset < CONTROL_CHUNK_VALUES
            ? CONTROL_WARMUP_VALUES - offset
            : CONTROL_CHUNK_VALUES;
    status = control_prefill(pipes[1], chunk);
    if (status != SALTS_OK) goto cleanup;
    for (size_t index = 0u; index < chunk; ++index) {
      uint64_t ignored = 0u;
      status = control_direct_read(&backend, endpoint, &byte, &ignored);
      if (status != SALTS_OK) goto cleanup;
    }
  }

  for (size_t offset = 0u; offset < CONTROL_MEASURED_VALUES;
       offset += CONTROL_CHUNK_VALUES) {
    const size_t chunk =
        CONTROL_MEASURED_VALUES - offset < CONTROL_CHUNK_VALUES
            ? CONTROL_MEASURED_VALUES - offset
            : CONTROL_CHUNK_VALUES;
    uint64_t wall_started;
    uint64_t cpu_started;
    status = control_prefill(pipes[1], chunk);
    if (status != SALTS_OK) goto cleanup;
    wall_started = cmeta_hrtime();
    cpu_started = control_process_cpu_ns();
    for (size_t index = 0u; index < chunk; ++index) {
      status = control_direct_read(
          &backend, endpoint, &byte, &latencies[measured]);
      if (status != SALTS_OK) goto cleanup;
      ++measured;
    }
    wall_ns += cmeta_hrtime() - wall_started;
    cpu_ns += control_process_cpu_ns() - cpu_started;
  }

  if (measured != CONTROL_MEASURED_VALUES) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  if (!native_io_backend_get_stats(&backend, &stats)) {
    status = SALTS_EIO;
    goto cleanup;
  }
  *accepted = stats.submitted;
  *completed = stats.completed;
  *rejected = stats.rejected_full;
  *stale = 0u;
  *rate = wall_ns == 0u ? 0.0
      : (double)CONTROL_MEASURED_VALUES * 1.0e9 / (double)wall_ns;
  *cpu_percent = wall_ns == 0u ? 0.0
      : (double)cpu_ns * 100.0 / (double)wall_ns;
  *cpu_efficiency = cpu_ns == 0u ? 0.0
      : (double)CONTROL_MEASURED_VALUES * 1.0e9 / (double)cpu_ns;
  status = SALTS_OK;

cleanup:
  if (backend.impl != NULL) {
    (void)native_io_backend_close(&backend);
    control_close_pipe(pipes[0]);
    pipes[0] = CONTROL_INVALID_PIPE;
    if (native_io_endpoint_valid(endpoint))
      (void)native_io_backend_release_pipe(&backend, endpoint);
    (void)native_io_backend_destroy(&backend);
  }
  control_close_pipe(pipes[0]);
  control_close_pipe(pipes[1]);
  return status;
}

static void control_actor_release(void *user) {
  control_actor_operation *operation = (control_actor_operation *)user;
  if (operation != NULL && operation->released != NULL)
    ++*operation->released;
}

static void control_actor_complete(
    void *user, cflow_io_request_id request_id,
    cflow_io_lease_id lease_id, void *operation_user,
    const cflow_io_completion *completion) {
  control_actor_operation *operation =
      (control_actor_operation *)operation_user;
  (void)user;
  (void)lease_id;

  if (operation == NULL || completion == NULL) return;
  operation->request_id = request_id;
  operation->status =
      completion->kind == CFLOW_IO_COMPLETION_OK &&
              completion->bytes == 1u &&
              operation->byte == 0x5au
          ? SALTS_OK
          : SALTS_EPROTO;
  if (operation->latency_out != NULL)
    *operation->latency_out = cmeta_hrtime() - operation->started_ns;
  operation->completed = true;
}

static int control_actor_drive_one(
    cflow_io_native_adapter *adapter, cflow_io_actor *actor,
    cflow_executor *executor, control_actor_operation *operation) {
  for (size_t round = 0u; round < CONTROL_MAX_DRIVE_ROUNDS; ++round) {
    cflow_io_run_result run =
        cflow_io_actor_run_ready(actor, CONTROL_MAX_STEPS);
    size_t observed = 0u;
    int status;

    if (run.status == CFLOW_IO_RUN_INVALID_ARGUMENT) return SALTS_EINVAL;
    if (run.status == CFLOW_IO_RUN_BUSY) return SALTS_EBUSY;
    status = cflow_io_native_adapter_observe(adapter, 1u, &observed);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT) return status;
    run = cflow_io_actor_run_ready(actor, CONTROL_MAX_STEPS);
    if (run.status == CFLOW_IO_RUN_INVALID_ARGUMENT) return SALTS_EINVAL;
    if (run.status == CFLOW_IO_RUN_BUSY) return SALTS_EBUSY;
    (void)cflow_executor_run_ready(executor);
    if (operation->completed) return operation->status;
  }
  return SALTS_ETIMEDOUT;
}

static int control_actor_submit_one(
    cflow_io_native_adapter *adapter, cflow_io_actor *actor,
    cflow_executor *executor, native_io_endpoint endpoint,
    uint64_t *latency_out, size_t *released) {
  control_actor_operation operation;
  cflow_io_operation token;
  cflow_io_submit_result submitted;
  int status;

  memset(&operation, 0, sizeof(operation));
  operation.native = (native_io_operation){
      .kind = NATIVE_IO_OPERATION_PIPE_READ,
      .endpoint = endpoint,
      .buffer = &operation.byte,
      .length = 1u};
  operation.latency_out = latency_out;
  operation.released = released;
  operation.status = SALTS_EIO;
  token = (cflow_io_operation){&operation, control_actor_release};
  operation.started_ns = cmeta_hrtime();
  submitted = cflow_io_actor_try_submit(
      actor, CONTROL_ACTOR_LEASE_ID, &token);
  if (submitted.status != CFLOW_IO_SUBMIT_ACCEPTED)
    return submitted.status == CFLOW_IO_SUBMIT_FULL
        ? SALTS_ENOBUFS : SALTS_EPROTO;
  status = control_actor_drive_one(adapter, actor, executor, &operation);
  if (status != SALTS_OK) return status;
  if (cflow_io_actor_acknowledge(actor, operation.request_id) !=
      CFLOW_IO_ACK_RELEASED)
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int control_actor_replicate(
    native_io_backend_kind kind, uint64_t *latencies,
    double *rate, double *cpu_percent, double *cpu_efficiency,
    uint64_t *accepted, uint64_t *completed, uint64_t *rejected,
    uint64_t *stale) {
  control_pipe pipes[2] = {CONTROL_INVALID_PIPE, CONTROL_INVALID_PIPE};
  cflow_io_native_adapter adapter = {0};
  cflow_executor executor = {0};
  cflow_io_actor actor = {0};
  native_io_endpoint endpoint = {0};
  cflow_io_actor_stats actor_stats = {0};
  cflow_io_native_adapter_stats adapter_stats = {0};
  cflow_io_native_adapter_config adapter_config = {
      {kind, 1u, 1u, 1u}};
  cflow_io_actor_config actor_config = {0};
  uint64_t wall_ns = 0u;
  uint64_t cpu_ns = 0u;
  size_t released = 0u;
  size_t measured = 0u;
  bool executor_initialized = false;
  bool actor_initialized = false;
  int status;

  status = control_make_pipe_pair(pipes);
  if (status != SALTS_OK) goto cleanup;
  status = cflow_io_native_adapter_init(&adapter, &adapter_config);
  if (status != SALTS_OK) goto cleanup;
  status = cflow_io_native_adapter_attach_pipe(
      &adapter, (uintptr_t)pipes[0],
      NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint);
  if (status != SALTS_OK) goto cleanup;
  if (!cflow_executor_manual_init_with_capacity(&executor, 2u)) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }
  executor_initialized = true;
  actor_config.request_capacity = 1u;
  actor_config.command_capacity = 4u;
  actor_config.executor = &executor;
  actor_config.backend = cflow_io_native_adapter_actor_ops();
  actor_config.backend_user = &adapter;
  actor_config.completion = control_actor_complete;
  status = cflow_io_actor_init(&actor, &actor_config);
  if (status != SALTS_OK) goto cleanup;
  actor_initialized = true;

  status = control_prefill(pipes[1], CONTROL_WARMUP_VALUES);
  if (status != SALTS_OK) goto cleanup;
  for (size_t index = 0u; index < CONTROL_WARMUP_VALUES; ++index) {
    status = control_actor_submit_one(
        &adapter, &actor, &executor, endpoint, NULL, &released);
    if (status != SALTS_OK) goto cleanup;
  }

  for (size_t offset = 0u; offset < CONTROL_MEASURED_VALUES;
       offset += CONTROL_CHUNK_VALUES) {
    const size_t chunk =
        CONTROL_MEASURED_VALUES - offset < CONTROL_CHUNK_VALUES
            ? CONTROL_MEASURED_VALUES - offset
            : CONTROL_CHUNK_VALUES;
    uint64_t wall_started;
    uint64_t cpu_started;

    status = control_prefill(pipes[1], chunk);
    if (status != SALTS_OK) goto cleanup;
    wall_started = cmeta_hrtime();
    cpu_started = control_process_cpu_ns();
    for (size_t index = 0u; index < chunk; ++index) {
      status = control_actor_submit_one(
          &adapter, &actor, &executor, endpoint,
          &latencies[measured], &released);
      if (status != SALTS_OK) goto cleanup;
      ++measured;
    }
    wall_ns += cmeta_hrtime() - wall_started;
    cpu_ns += control_process_cpu_ns() - cpu_started;
  }

  if (!cflow_io_actor_get_stats(&actor, &actor_stats) ||
      !cflow_io_native_adapter_get_stats(&adapter, &adapter_stats) ||
      measured != CONTROL_MEASURED_VALUES ||
      released != CONTROL_WARMUP_VALUES + CONTROL_MEASURED_VALUES) {
    fprintf(stderr,
            "actor accounting mismatch: measured=%zu released=%zu accepted=%" PRIu64 " acknowledged=%" PRIu64 " stale=%" PRIu64 " adapter_stale=%" PRIu64 "\n",
            measured, released, actor_stats.accepted,
            actor_stats.acknowledged, actor_stats.stale_completions,
            adapter_stats.stale_actor_completions);
    status = SALTS_EPROTO;
    goto cleanup;
  }
  *accepted = actor_stats.accepted;
  *completed = actor_stats.acknowledged;
  *rejected = actor_stats.rejected_request_full +
              actor_stats.rejected_command_full +
              actor_stats.rejected_closed +
              actor_stats.rejected_lease_in_use +
              actor_stats.executor_rejected_full +
              actor_stats.executor_rejected_closed +
              actor_stats.executor_rejected_invalid;
  *stale = actor_stats.stale_completions +
           adapter_stats.stale_actor_completions;
  *rate = wall_ns == 0u ? 0.0
      : (double)CONTROL_MEASURED_VALUES * 1.0e9 / (double)wall_ns;
  *cpu_percent = wall_ns == 0u ? 0.0
      : (double)cpu_ns * 100.0 / (double)wall_ns;
  *cpu_efficiency = cpu_ns == 0u ? 0.0
      : (double)CONTROL_MEASURED_VALUES * 1.0e9 / (double)cpu_ns;
  status = SALTS_OK;

cleanup:
  if (actor_initialized) {
    (void)cflow_io_actor_close(&actor);
    for (size_t round = 0u;
         round < CONTROL_MAX_DRIVE_ROUNDS &&
         !cflow_io_actor_is_quiescent(&actor); ++round) {
      (void)cflow_io_actor_run_ready(&actor, CONTROL_MAX_STEPS);
      (void)cflow_executor_run_ready(&executor);
    }
    (void)cflow_io_actor_destroy(&actor);
  }
  if (executor_initialized)
    cflow_executor_destroy(&executor);
  if (adapter.impl != NULL) {
    (void)cflow_io_native_adapter_close(&adapter);
    control_close_pipe(pipes[0]);
    pipes[0] = CONTROL_INVALID_PIPE;
    if (native_io_endpoint_valid(endpoint))
      (void)cflow_io_native_adapter_release_pipe(&adapter, endpoint);
    (void)cflow_io_native_adapter_destroy(&adapter);
  }
  control_close_pipe(pipes[0]);
  control_close_pipe(pipes[1]);
  return status;
}

static void control_publisher_release(void *user) {
  control_publisher_operation *operation =
      (control_publisher_operation *)user;
  if (operation != NULL && operation->released != NULL)
    ++*operation->released;
}

static cflow_io_publisher_prepare_status control_publisher_prepare(
    void *user, cflow_io_operation *operation, const char **error) {
  control_publisher_fixture *fixture =
      (control_publisher_fixture *)user;
  control_publisher_operation *entry;

  (void)error;
  if (fixture == NULL || operation == NULL)
    return CFLOW_IO_PUBLISHER_PREPARE_ERROR;
  if (fixture->prepared >= fixture->operation_count)
    return CFLOW_IO_PUBLISHER_PREPARE_DONE;
  entry = &fixture->operations[fixture->prepared++];
  entry->started_ns = cmeta_hrtime();
  operation->user = entry;
  operation->release = control_publisher_release;
  return CFLOW_IO_PUBLISHER_PREPARE_OPERATION;
}

static cflow_read_status control_publisher_encode(
    void *user, cflow_io_request_id request_id,
    cflow_io_lease_id lease_id, void *operation_user,
    const cflow_io_completion *completion, void *out_value,
    const char **error) {
  control_publisher_fixture *fixture =
      (control_publisher_fixture *)user;
  control_publisher_operation *operation =
      (control_publisher_operation *)operation_user;
  (void)request_id;
  (void)lease_id;

  if (fixture == NULL || operation == NULL || completion == NULL ||
      out_value == NULL || completion->kind != CFLOW_IO_COMPLETION_OK ||
      completion->bytes != 1u || operation->byte != 0x5au) {
    if (error != NULL) *error = "CFlow control baseline completion mismatch";
    return CFLOW_READ_ERROR;
  }
  *(int *)out_value = (int)operation->index;
  ++fixture->encoded;
  return CFLOW_READ_VALUE;
}

static bool control_publisher_value(
    void *user, const cmeta_type_desc *type, const void *value) {
  control_publisher_fixture *fixture =
      (control_publisher_fixture *)user;
  size_t index;

  if (fixture == NULL || value == NULL ||
      !cmeta_type_equal(type, &cmeta_type_int) ||
      *(const int *)value < 0)
    return false;
  index = (size_t)*(const int *)value;
  if (index >= fixture->operation_count)
    return false;
  if (fixture->operations[index].latency_out != NULL)
    *fixture->operations[index].latency_out =
        cmeta_hrtime() - fixture->operations[index].started_ns;
  ++fixture->values;
  return true;
}

static void control_publisher_error(void *user, const char *message) {
  control_publisher_fixture *fixture =
      (control_publisher_fixture *)user;
  if (fixture != NULL) {
    ++fixture->errors;
    fixture->error = message;
  }
}

static void control_publisher_done(void *user) {
  control_publisher_fixture *fixture =
      (control_publisher_fixture *)user;
  if (fixture != NULL) ++fixture->dones;
}

static int control_publisher_drive_until(
    cflow_io_native_adapter *adapter,
    cflow_io_publisher_owner *owner,
    cflow_scheduler *scheduler,
    control_publisher_fixture *fixture,
    size_t target_values) {
  for (size_t round = 0u; round < CONTROL_MAX_DRIVE_ROUNDS; ++round) {
    size_t completed = 0u;
    int status;

    (void)cflow_scheduler_run_until_idle(scheduler, 0u);
    if (fixture->errors != 0u) return SALTS_EIO;
    if (fixture->values >= target_values) return SALTS_OK;
    status = cflow_io_native_adapter_drive_publisher(
        adapter, owner, 1u, CONTROL_MAX_STEPS, &completed);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT)
      return status;
    (void)cflow_scheduler_run_until_idle(scheduler, 0u);
    if (fixture->errors != 0u) return SALTS_EIO;
    if (fixture->values >= target_values) return SALTS_OK;
  }
  return SALTS_ETIMEDOUT;
}

static int control_publisher_replicate(
    native_io_backend_kind kind, uint64_t *latencies,
    double *rate, double *cpu_percent, double *cpu_efficiency,
    uint64_t *accepted, uint64_t *completed, uint64_t *rejected,
    uint64_t *stale) {
  const size_t total_operations =
      CONTROL_WARMUP_VALUES + CONTROL_MEASURED_VALUES;
  control_pipe pipes[2] = {CONTROL_INVALID_PIPE, CONTROL_INVALID_PIPE};
  cflow_io_native_adapter adapter = {0};
  cflow_io_publisher_owner owner = {0};
  cflow_publisher publisher = {0};
  cflow_graph surface = {0};
  cflow_graph normalized = {0};
  cflow_scheduler scheduler = {0};
  cflow_subscription subscription = {0};
  native_io_endpoint endpoint = {0};
  control_publisher_operation *operations = NULL;
  control_publisher_fixture fixture = {0};
  cflow_io_publisher_stats publisher_stats = {0};
  cflow_io_native_adapter_stats adapter_stats = {0};
  cflow_io_native_adapter_config adapter_config = {
      {kind, 1u, 1u, 1u}};
  cflow_io_publisher_config publisher_config = {0};
  cflow_subscriber_callbacks callbacks = {
      control_publisher_value,
      control_publisher_error,
      control_publisher_done,
      &fixture};
  cflow_subscriber subscriber =
      cflow_subscriber_from_callbacks(&callbacks);
  uint64_t wall_ns = 0u;
  uint64_t cpu_ns = 0u;
  size_t released = 0u;
  bool surface_initialized = false;
  bool normalized_initialized = false;
  bool scheduler_initialized = false;
  bool subscription_open = false;
  int status;

  operations = (control_publisher_operation *)calloc(
      total_operations, sizeof(*operations));
  if (operations == NULL) return SALTS_ENOMEM;

  status = control_make_pipe_pair(pipes);
  if (status != SALTS_OK) goto cleanup;
  status = cflow_io_native_adapter_init(&adapter, &adapter_config);
  if (status != SALTS_OK) goto cleanup;
  status = cflow_io_native_adapter_attach_pipe(
      &adapter, (uintptr_t)pipes[0],
      NATIVE_IO_PIPE_ENDPOINT_ASYNC_CAPABLE, &endpoint);
  if (status != SALTS_OK) goto cleanup;

  for (size_t index = 0u; index < total_operations; ++index) {
    operations[index].native = (native_io_operation){
        .kind = NATIVE_IO_OPERATION_PIPE_READ,
        .endpoint = endpoint,
        .buffer = &operations[index].byte,
        .length = 1u};
    operations[index].latency_out =
        index >= CONTROL_WARMUP_VALUES
            ? &latencies[index - CONTROL_WARMUP_VALUES]
            : NULL;
    operations[index].released = &released;
    operations[index].index = index;
  }
  fixture.operations = operations;
  fixture.operation_count = total_operations;

  publisher_config.name = "cflow-control-baseline";
  publisher_config.type = &cmeta_type_int;
  publisher_config.backend = cflow_io_native_adapter_actor_ops();
  publisher_config.backend_user = &adapter;
  publisher_config.prepare = control_publisher_prepare;
  publisher_config.encode = control_publisher_encode;
  publisher_config.user = &fixture;

  cflow_graph_init(&surface, &cmeta_type_int);
  surface_initialized = true;
  if (!cflow_graph_normalize(&normalized, &surface)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  normalized_initialized = true;
  if (!cflow_scheduler_manual_init_with_capacity(&scheduler, 8u)) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }
  scheduler_initialized = true;
  status = cflow_publisher_from_io_actor_windowed(
      &publisher, &owner, &publisher_config, 1u);
  if (status != SALTS_OK) goto cleanup;
  if (!cflow_subscribe(
          &subscription, &normalized, &publisher,
          &scheduler, &subscriber)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  subscription_open = true;

  status = control_prefill(pipes[1], CONTROL_WARMUP_VALUES);
  if (status != SALTS_OK) goto cleanup;
  if (!cflow_subscription_request(
          &subscription, CONTROL_WARMUP_VALUES)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  status = control_publisher_drive_until(
      &adapter, &owner, &scheduler, &fixture,
      CONTROL_WARMUP_VALUES);
  if (status != SALTS_OK) goto cleanup;

  for (size_t offset = 0u; offset < CONTROL_MEASURED_VALUES;
       offset += CONTROL_CHUNK_VALUES) {
    const size_t chunk =
        CONTROL_MEASURED_VALUES - offset < CONTROL_CHUNK_VALUES
            ? CONTROL_MEASURED_VALUES - offset
            : CONTROL_CHUNK_VALUES;
    const size_t target =
        CONTROL_WARMUP_VALUES + offset + chunk;
    uint64_t wall_started;
    uint64_t cpu_started;

    status = control_prefill(pipes[1], chunk);
    if (status != SALTS_OK) goto cleanup;
    wall_started = cmeta_hrtime();
    cpu_started = control_process_cpu_ns();
    if (!cflow_subscription_request(&subscription, chunk)) {
      status = SALTS_EPROTO;
      goto cleanup;
    }
    status = control_publisher_drive_until(
        &adapter, &owner, &scheduler, &fixture, target);
    if (status != SALTS_OK) goto cleanup;
    wall_ns += cmeta_hrtime() - wall_started;
    cpu_ns += control_process_cpu_ns() - cpu_started;
  }

  if (!cflow_io_publisher_owner_get_stats(&owner, &publisher_stats) ||
      !cflow_io_native_adapter_get_stats(&adapter, &adapter_stats) ||
      fixture.values != total_operations ||
      fixture.encoded != total_operations ||
      released != total_operations ||
      fixture.errors != 0u) {
    fprintf(stderr,
            "publisher accounting mismatch: prepared=%zu encoded=%zu values=%zu released=%zu errors=%zu dones=%zu accepted=%" PRIu64 " acknowledged=%" PRIu64 " stale=%" PRIu64 " adapter_stale=%" PRIu64 "\n",
            fixture.prepared, fixture.encoded, fixture.values, released,
            fixture.errors, fixture.dones, publisher_stats.actor.accepted,
            publisher_stats.actor.acknowledged,
            publisher_stats.actor.stale_completions,
            adapter_stats.stale_actor_completions);
    status = SALTS_EPROTO;
    goto cleanup;
  }

  *accepted = publisher_stats.actor.accepted;
  *completed = publisher_stats.actor.acknowledged;
  *rejected = publisher_stats.actor.rejected_request_full +
              publisher_stats.actor.rejected_command_full +
              publisher_stats.actor.rejected_closed +
              publisher_stats.actor.rejected_lease_in_use +
              publisher_stats.actor.executor_rejected_full +
              publisher_stats.actor.executor_rejected_closed +
              publisher_stats.actor.executor_rejected_invalid;
  *stale = publisher_stats.actor.stale_completions +
           adapter_stats.stale_actor_completions;
  *rate = wall_ns == 0u ? 0.0
      : (double)CONTROL_MEASURED_VALUES * 1.0e9 / (double)wall_ns;
  *cpu_percent = wall_ns == 0u ? 0.0
      : (double)cpu_ns * 100.0 / (double)wall_ns;
  *cpu_efficiency = cpu_ns == 0u ? 0.0
      : (double)CONTROL_MEASURED_VALUES * 1.0e9 / (double)cpu_ns;
  status = SALTS_OK;

cleanup:
  if (subscription_open) {
    cflow_subscription_close(&subscription);
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    subscription_open = false;
  } else if (cflow_publisher_valid(&publisher)) {
    cflow_publisher_destroy(&publisher);
  }
  if (owner.impl != NULL) {
    for (size_t round = 0u;
         round < CONTROL_MAX_DRIVE_ROUNDS &&
         !cflow_io_publisher_owner_is_quiescent(&owner); ++round) {
      size_t ignored = 0u;
      int drive_status = cflow_io_native_adapter_drive_publisher(
          &adapter, &owner, 0u, CONTROL_MAX_STEPS, &ignored);
      if (drive_status != SALTS_OK &&
          drive_status != SALTS_ETIMEDOUT)
        break;
      (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    }
    (void)cflow_io_publisher_owner_close(&owner);
  }
  if (scheduler_initialized) cflow_scheduler_destroy(&scheduler);
  if (normalized_initialized) cflow_graph_destroy(&normalized);
  if (surface_initialized) cflow_graph_destroy(&surface);
  if (adapter.impl != NULL) {
    (void)cflow_io_native_adapter_close(&adapter);
    control_close_pipe(pipes[0]);
    pipes[0] = CONTROL_INVALID_PIPE;
    if (native_io_endpoint_valid(endpoint))
      (void)cflow_io_native_adapter_release_pipe(&adapter, endpoint);
    (void)cflow_io_native_adapter_destroy(&adapter);
  }
  control_close_pipe(pipes[0]);
  control_close_pipe(pipes[1]);
  free(operations);
  return status;
}

typedef int (*control_replicate_fn)(
    native_io_backend_kind kind, uint64_t *latencies,
    double *rate, double *cpu_percent, double *cpu_efficiency,
    uint64_t *accepted, uint64_t *completed, uint64_t *rejected,
    uint64_t *stale);

static int control_run_layer(
    const char *name, control_replicate_fn run,
    native_io_backend_kind kind, control_layer_summary *summary) {
  const size_t latency_count =
      (size_t)CONTROL_REPLICATES * CONTROL_MEASURED_VALUES;
  uint64_t *latencies = (uint64_t *)calloc(
      latency_count, sizeof(*latencies));
  double rates[CONTROL_REPLICATES];
  double cpu_percent[CONTROL_REPLICATES];
  double cpu_efficiency[CONTROL_REPLICATES];
  uint64_t accepted = 0u;
  uint64_t completed = 0u;
  uint64_t rejected = 0u;
  uint64_t stale = 0u;
  int status = SALTS_OK;

  if (latencies == NULL || summary == NULL) {
    free(latencies);
    return SALTS_ENOMEM;
  }

  for (size_t replicate = 0u;
       replicate < CONTROL_REPLICATES; ++replicate) {
    uint64_t replicate_accepted = 0u;
    uint64_t replicate_completed = 0u;
    uint64_t replicate_rejected = 0u;
    uint64_t replicate_stale = 0u;

    status = run(
        kind,
        &latencies[replicate * CONTROL_MEASURED_VALUES],
        &rates[replicate], &cpu_percent[replicate],
        &cpu_efficiency[replicate],
        &replicate_accepted, &replicate_completed,
        &replicate_rejected, &replicate_stale);
    if (status != SALTS_OK) {
      fprintf(stderr,
              "CFlow control baseline layer=%s replicate=%zu failed: status=%d\n",
              name, replicate, status);
      break;
    }
    accepted += replicate_accepted;
    completed += replicate_completed;
    rejected += replicate_rejected;
    stale += replicate_stale;
  }

  if (status == SALTS_OK) {
    summary->layer = name;
    summary->p50_ns =
        (double)control_u64_percentile(
            latencies, latency_count, 50u);
    summary->p95_ns =
        (double)control_u64_percentile(
            latencies, latency_count, 95u);
    summary->p99_ns =
        (double)control_u64_percentile(
            latencies, latency_count, 99u);
    summary->median_values_per_second =
        control_double_median(rates, CONTROL_REPLICATES);
    summary->median_cpu_percent =
        control_double_median(cpu_percent, CONTROL_REPLICATES);
    summary->median_values_per_cpu_second =
        control_double_median(cpu_efficiency, CONTROL_REPLICATES);
    summary->accepted = accepted;
    summary->completed = completed;
    summary->rejected = rejected;
    summary->stale = stale;
  }

  free(latencies);
  return status;
}

static FILE *control_open_csv(void) {
  const char *prefix = getenv("CFLOW_CONTROL_BASELINE_OUTPUT");
  char path[1024];

  if (prefix == NULL || *prefix == '\0') return NULL;
  if (snprintf(path, sizeof(path), "%s.csv", prefix) < 0) return NULL;
  return fopen(path, "w");
}

static void control_print_csv(
    FILE *stream, const char *backend,
    const control_layer_summary *row) {
  fprintf(
      stream,
      "%s,%s,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
      "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
      backend, row->layer, CONTROL_REPLICATES,
      CONTROL_MEASURED_VALUES, row->p50_ns, row->p95_ns,
      row->p99_ns, row->median_values_per_second,
      row->median_cpu_percent, row->median_values_per_cpu_second,
      row->accepted, row->completed, row->rejected, row->stale);
}

int main(void) {
  const native_io_backend_kind kind = control_backend();
  const char *backend = control_backend_name(kind);
  control_layer_summary direct = {0};
  control_layer_summary actor = {0};
  control_layer_summary publisher = {0};
  FILE *csv;
  int status;

  if (kind == (native_io_backend_kind)0 ||
      !native_io_backend_kind_supported(kind) ||
      !native_io_backend_kind_supports_pipe(kind)) {
    fprintf(stderr, "unsupported CFlow control baseline backend: %s\n",
            backend);
    return 2;
  }

  status = control_run_layer(
      "direct", control_direct_replicate, kind, &direct);
  if (status == SALTS_OK)
    status = control_run_layer(
        "actor", control_actor_replicate, kind, &actor);
  if (status == SALTS_OK)
    status = control_run_layer(
        "publisher", control_publisher_replicate, kind, &publisher);
  if (status != SALTS_OK) {
    fprintf(stderr, "CFlow control baseline failed: status=%d\n", status);
    return 1;
  }

  printf("# CFlow Direct / IO Actor / IO Publisher control-path baseline\n\n");
  printf("Backend: %s\n\n", backend);
  printf("Each timed chunk consumes prefilled one-byte PIPE completions; "
         "raw peer writes happen outside the timed interval. Publisher "
         "window is fixed at 1. A zero observed percentile means the single "
         "completion fell below the host monotonic-clock sampling resolution; "
         "no synthetic latency is substituted.\n\n");
  printf("| layer | p50 ns | p95 ns | p99 ns | median values/s | "
         "median CPU %% | median values/CPU-s | accepted | completed | "
         "rejected | stale |\n");
  printf("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
         "---: | ---: | ---: |\n");
  const control_layer_summary *rows[] = {
      &direct, &actor, &publisher};
  for (size_t index = 0u; index < 3u; ++index) {
    const control_layer_summary *row = rows[index];
    printf("| %s | %.3f | %.3f | %.3f | %.0f | %.3f | %.0f | "
           "%" PRIu64 " | %" PRIu64 " | %" PRIu64 " | %" PRIu64 " |\n",
           row->layer, row->p50_ns, row->p95_ns, row->p99_ns,
           row->median_values_per_second, row->median_cpu_percent,
           row->median_values_per_cpu_second, row->accepted,
           row->completed, row->rejected, row->stale);
  }
  printf("\nActor/Direct throughput ratio: %.3fx\n",
         actor.median_values_per_second /
             direct.median_values_per_second);
  printf("Publisher/Actor throughput ratio: %.3fx\n",
         publisher.median_values_per_second /
             actor.median_values_per_second);
  printf("Actor-Direct p99 delta: %.3f ns\n",
         actor.p99_ns - direct.p99_ns);
  printf("Publisher-Actor p99 delta: %.3f ns\n",
         publisher.p99_ns - actor.p99_ns);

  csv = control_open_csv();
  if (csv != NULL) {
    fprintf(
        csv,
        "backend,layer,replicates,values_per_replicate,p50_ns,p95_ns,"
        "p99_ns,median_values_per_second,median_cpu_percent,"
        "median_values_per_cpu_second,accepted,completed,rejected,stale\n");
    control_print_csv(csv, backend, &direct);
    control_print_csv(csv, backend, &actor);
    control_print_csv(csv, backend, &publisher);
    fclose(csv);
  }

  return 0;
}
