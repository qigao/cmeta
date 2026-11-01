#include <cflow/io_file.h>

#include <clock.h>
#include <error_codes.h>
#include <thread.h>

#include "tinytest.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { FILE_TEST_COMPLETION_CAPACITY = 8 };

static const uint64_t FILE_TEST_TIMEOUT_NS = UINT64_C(5000000000);

typedef struct file_completion_probe {
  cflow_io_request_id request_ids[FILE_TEST_COMPLETION_CAPACITY];
  cflow_io_lease_id lease_ids[FILE_TEST_COMPLETION_CAPACITY];
  cflow_io_native_file_operation_kind operation_kinds[FILE_TEST_COMPLETION_CAPACITY];
  cflow_io_completion completions[FILE_TEST_COMPLETION_CAPACITY];
  size_t count;
  cflow_io_file *reentrant_file;
  int reentrant_status;
  size_t reentrant_progressed;
} file_completion_probe;

typedef int (*file_open_fn)(cflow_io_file *, const char *, const cflow_io_file_config *);
typedef bool (*file_supported_fn)(const cflow_io_file *, cflow_io_native_file_operation_kind);
typedef cflow_io_file_submit_result (*file_read_fn)(cflow_io_file *, cflow_io_lease_id, void *,
                                                    size_t, uint64_t);
typedef cflow_io_file_submit_result (*file_write_fn)(cflow_io_file *, cflow_io_lease_id,
                                                     const void *, size_t, uint64_t);
typedef cflow_io_file_submit_result (*file_flush_fn)(cflow_io_file *, cflow_io_lease_id);
typedef cflow_io_cancel_status (*file_cancel_fn)(cflow_io_file *, cflow_io_request_id);
typedef int (*file_run_fn)(cflow_io_file *, size_t, size_t *);
typedef int (*file_close_fn)(cflow_io_file *);
typedef bool (*file_quiescent_fn)(const cflow_io_file *);
typedef bool (*file_stats_fn)(const cflow_io_file *, cflow_io_file_stats *);
typedef int (*file_destroy_fn)(cflow_io_file *);
typedef int (*file_runtime_init_fn)(cflow_io_file_runtime *, const cflow_io_file_runtime_config *);
typedef int (*file_runtime_run_fn)(cflow_io_file_runtime *, size_t, size_t *);
typedef int (*file_runtime_close_fn)(cflow_io_file_runtime *);
typedef bool (*file_runtime_quiescent_fn)(const cflow_io_file_runtime *);
typedef bool (*file_runtime_stats_fn)(const cflow_io_file_runtime *, cflow_io_file_runtime_stats *);
typedef int (*file_runtime_destroy_fn)(cflow_io_file_runtime *);

static void file_completion(void *user, cflow_io_request_id request_id, cflow_io_lease_id lease_id,
                            cflow_io_native_file_operation_kind operation_kind,
                            const cflow_io_completion *completion) {
  file_completion_probe *probe = (file_completion_probe *)user;
  if (probe != NULL && completion != NULL && probe->count < FILE_TEST_COMPLETION_CAPACITY) {
    probe->request_ids[probe->count] = request_id;
    probe->lease_ids[probe->count] = lease_id;
    probe->operation_kinds[probe->count] = operation_kind;
    probe->completions[probe->count] = *completion;
    ++probe->count;
  }
  if (probe != NULL && probe->reentrant_file != NULL) {
    cflow_io_file *file = probe->reentrant_file;
    probe->reentrant_file = NULL;
    probe->reentrant_status = cflow_io_file_run_ready(file, 1u, &probe->reentrant_progressed);
  }
}

static cflow_io_native_backend_kind file_test_backend(void) {
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

static cflow_io_file_config file_test_config(uint32_t open_flags) {
  cflow_io_file_config config = {0};
  config.backend_kind = file_test_backend();
  config.request_capacity = 2u;
  config.command_capacity = 2u;
  config.completion_batch_capacity = 2u;
  config.open_flags = open_flags;
  config.create_mode = (open_flags & CFLOW_IO_FILE_CREATE) != 0u ? 0600u : 0u;
  config.completion = file_completion;
  return config;
}

static cflow_io_file_runtime_config file_test_runtime_config(void) {
  cflow_io_file_runtime_config config = {0};
  config.backend_kind = file_test_backend();
  config.file_capacity = 2u;
  config.request_capacity = 2u;
  config.command_capacity = 2u;
  config.completion_batch_capacity = 2u;
  return config;
}

static void file_test_runtime_wake(void *user) {
  atomic_int *wake_count = (atomic_int *)user;
  atomic_fetch_add_explicit(wake_count, 1, memory_order_release);
}

static int file_test_runtime_wait(cflow_io_file_runtime *runtime, file_completion_probe *first,
                                  file_completion_probe *second) {
  const uint64_t started = cmeta_hrtime();
  for (;;) {
    cflow_io_file_runtime_stats stats = {0};
    size_t progressed = 0u;
    int status = cflow_io_file_runtime_run_ready(runtime, 64u, &progressed);
    if (status != SALTS_OK) return status;
    if (!cflow_io_file_runtime_get_stats(runtime, &stats)) return SALTS_EINVAL;
    if (first->count == 1u && second->count == 1u && stats.operation_slots_in_use == 0u)
      return SALTS_OK;
    if (cmeta_hrtime() - started >= FILE_TEST_TIMEOUT_NS) return SALTS_ETIMEDOUT;
    if (progressed == 0u) cmeta_thread_yield();
  }
}

static int file_test_wait(cflow_io_file *file, file_completion_probe *probe,
                          size_t expected_count) {
  const uint64_t started = cmeta_hrtime();
  for (;;) {
    cflow_io_file_stats stats = {0};
    size_t progressed = 0u;
    int status = cflow_io_file_run_ready(file, 64u, &progressed);
    if (status != SALTS_OK) return status;
    if (!cflow_io_file_get_stats(file, &stats)) return SALTS_EINVAL;
    if (probe->count >= expected_count && stats.operation_slots_in_use == 0u) return SALTS_OK;
    if (cmeta_hrtime() - started >= FILE_TEST_TIMEOUT_NS) return SALTS_ETIMEDOUT;
    if (progressed == 0u) cmeta_thread_yield();
  }
}

static void file_test_close_destroy(cflow_io_file *file) {
  check_equal(cflow_io_file_close(file), SALTS_OK);
  check_true(cflow_io_file_is_quiescent(file));
  check_equal(cflow_io_file_destroy(file), SALTS_OK);
}

spec("CFlow async file facade") {
  it("shares one bounded runtime across multiple files") {
    static const char first_payload[] = "first-runtime";
    static const char second_payload[] = "second-runtime";
    char *first_path = tt_make_temp_file("cflow-io-runtime-first-", ".bin");
    char *second_path = tt_make_temp_file("cflow-io-runtime-second-", ".bin");
    cflow_io_file_runtime runtime = {0};
    cflow_io_file_runtime_config runtime_config = file_test_runtime_config();
    cflow_io_file first = {0};
    cflow_io_file second = {0};
    file_completion_probe first_probe = {0};
    file_completion_probe second_probe = {0};
    cflow_io_file_config first_config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_config second_config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    cflow_io_file_runtime_stats stats = {0};
    int status;

    check_not_null(first_path);
    check_not_null(second_path);
    status = cflow_io_file_runtime_init(&runtime, &runtime_config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(first_path), 0);
      check_equal(tt_remove_file(second_path), 0);
      free(first_path);
      free(second_path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    first_config.backend_kind = 0;
    first_config.request_capacity = 0u;
    first_config.command_capacity = 0u;
    first_config.completion_batch_capacity = 0u;
    first_config.runtime = &runtime;
    first_config.completion_user = &first_probe;
    second_config.backend_kind = 0;
    second_config.request_capacity = 0u;
    second_config.command_capacity = 0u;
    second_config.completion_batch_capacity = 0u;
    second_config.runtime = &runtime;
    second_config.completion_user = &second_probe;

    check_equal(cflow_io_file_open(&first, first_path, &first_config), SALTS_OK);
    check_equal(cflow_io_file_open(&second, second_path, &second_config), SALTS_OK);
    check_true(cflow_io_file_runtime_get_stats(&runtime, &stats));
    check_equal(stats.open_files, (size_t)2u);

    submitted =
        cflow_io_file_try_write_at(&first, 101u, first_payload, sizeof(first_payload) - 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    submitted =
        cflow_io_file_try_write_at(&second, 102u, second_payload, sizeof(second_payload) - 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_equal(file_test_runtime_wait(&runtime, &first_probe, &second_probe), SALTS_OK);
    check_equal(first_probe.completions[0].kind, CFLOW_IO_COMPLETION_OK);
    check_equal(second_probe.completions[0].kind, CFLOW_IO_COMPLETION_OK);

    check_equal(cflow_io_file_close(&first), SALTS_OK);
    check_equal(cflow_io_file_destroy(&first), SALTS_OK);
    check_equal(cflow_io_file_close(&second), SALTS_OK);
    check_equal(cflow_io_file_destroy(&second), SALTS_OK);
    check_equal(cflow_io_file_runtime_close(&runtime), SALTS_OK);
    check_true(cflow_io_file_runtime_is_quiescent(&runtime));
    check_equal(cflow_io_file_runtime_destroy(&runtime), SALTS_OK);

    check_equal(tt_remove_file(first_path), 0);
    check_equal(tt_remove_file(second_path), 0);
    free(first_path);
    free(second_path);
  }

  it("rejects files beyond the shared runtime handle capacity") {
    char *first_path = tt_make_temp_file("cflow-io-runtime-cap-first-", ".bin");
    char *second_path = tt_make_temp_file("cflow-io-runtime-cap-second-", ".bin");
    cflow_io_file_runtime runtime = {0};
    cflow_io_file_runtime_config runtime_config = file_test_runtime_config();
    cflow_io_file first = {0};
    cflow_io_file second = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_READ);
    int status;

    check_not_null(first_path);
    check_not_null(second_path);
    runtime_config.file_capacity = 1u;
    status = cflow_io_file_runtime_init(&runtime, &runtime_config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(first_path), 0);
      check_equal(tt_remove_file(second_path), 0);
      free(first_path);
      free(second_path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    config.backend_kind = 0;
    config.request_capacity = 0u;
    config.command_capacity = 0u;
    config.completion_batch_capacity = 0u;
    config.runtime = &runtime;
    check_equal(cflow_io_file_open(&first, first_path, &config), SALTS_OK);
    check_equal(cflow_io_file_open(&second, second_path, &config), SALTS_ENOBUFS);
    check_null(second.impl);
    check_equal(cflow_io_file_close(&first), SALTS_OK);
    check_equal(cflow_io_file_destroy(&first), SALTS_OK);
    check_equal(cflow_io_file_runtime_close(&runtime), SALTS_OK);
    check_equal(cflow_io_file_runtime_destroy(&runtime), SALTS_OK);
    check_equal(tt_remove_file(first_path), 0);
    check_equal(tt_remove_file(second_path), 0);
    free(first_path);
    free(second_path);
  }

  it("forwards runtime readiness through the configured wake callback") {
    static const char payload[] = "wake-runtime";
    char *path = tt_make_temp_file("cflow-io-runtime-wake-", ".bin");
    cflow_io_file_runtime runtime = {0};
    cflow_io_file_runtime_config runtime_config = file_test_runtime_config();
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    atomic_int wake_count;
    int status;

    check_not_null(path);
    atomic_init(&wake_count, 0);
    runtime_config.file_capacity = 1u;
    runtime_config.request_capacity = 1u;
    runtime_config.command_capacity = 1u;
    runtime_config.completion_batch_capacity = 1u;
    runtime_config.wake = file_test_runtime_wake;
    runtime_config.wake_user = &wake_count;
    status = cflow_io_file_runtime_init(&runtime, &runtime_config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    config.backend_kind = 0;
    config.request_capacity = 0u;
    config.command_capacity = 0u;
    config.completion_batch_capacity = 0u;
    config.runtime = &runtime;
    config.completion_user = &probe;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_OK);
    submitted = cflow_io_file_try_write_at(&file, 201u, payload, sizeof(payload) - 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_greater(atomic_load_explicit(&wake_count, memory_order_acquire), 0);
    check_equal(file_test_wait(&file, &probe, 1u), SALTS_OK);
    file_test_close_destroy(&file);
    check_equal(cflow_io_file_runtime_close(&runtime), SALTS_OK);
    check_equal(cflow_io_file_runtime_destroy(&runtime), SALTS_OK);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("exposes the bounded public contract") {
    cflow_io_file file = {0};
    cflow_io_file_config config = {.backend_kind = CFLOW_IO_NATIVE_IOCP,
                                   .request_capacity = 2u,
                                   .command_capacity = 2u,
                                   .completion_batch_capacity = 2u,
                                   .open_flags = CFLOW_IO_FILE_READ | CFLOW_IO_FILE_WRITE,
                                   .create_mode = 0600u,
                                   .completion = file_completion,
                                   .completion_user = NULL};
    cflow_io_file_stats stats = {0};
    cflow_io_file_submit_result submitted = {CFLOW_IO_FILE_SUBMIT_INVALID_ARGUMENT, 0u};

    check_null(file.impl);
    check_equal(config.request_capacity, (size_t)2u);
    check_equal(stats.operation_slots_in_use, (size_t)0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_INVALID_ARGUMENT);
    check_true(_Generic(&cflow_io_file_open, file_open_fn: true, default: false));
    check_true(
        _Generic(&cflow_io_file_operation_supported, file_supported_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_try_read_at, file_read_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_try_write_at, file_write_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_try_flush, file_flush_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_try_cancel, file_cancel_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_run_ready, file_run_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_close, file_close_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_is_quiescent, file_quiescent_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_get_stats, file_stats_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_destroy, file_destroy_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_runtime_init, file_runtime_init_fn: true, default: false));
    check_true(
        _Generic(&cflow_io_file_runtime_run_ready, file_runtime_run_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_runtime_close, file_runtime_close_fn: true, default: false));
    check_true(_Generic(&cflow_io_file_runtime_is_quiescent,
                   file_runtime_quiescent_fn: true,
                   default: false));
    check_true(
        _Generic(&cflow_io_file_runtime_get_stats, file_runtime_stats_fn: true, default: false));
    check_true(
        _Generic(&cflow_io_file_runtime_destroy, file_runtime_destroy_fn: true, default: false));
  }

  it("rejects malformed open configurations without publishing state") {
    char *path = tt_make_temp_file("cflow-io-file-config-", ".bin");
    cflow_io_file file = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_READ | CFLOW_IO_FILE_WRITE);
    int status;

    check_not_null(path);
    check_equal(cflow_io_file_open(NULL, path, &config), SALTS_EINVAL);
    check_equal(cflow_io_file_open(&file, NULL, &config), SALTS_EINVAL);
    check_equal(cflow_io_file_open(&file, "", &config), SALTS_EINVAL);
    check_equal(cflow_io_file_open(&file, path, NULL), SALTS_EINVAL);

    config.open_flags = 0u;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ | (1u << 31));
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ | CFLOW_IO_FILE_CREATE);
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ | CFLOW_IO_FILE_TRUNCATE);
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ);
    config.create_mode = 0600u;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_WRITE | CFLOW_IO_FILE_CREATE);
    config.create_mode = 01000u;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ);
    config.request_capacity = 0u;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ);
    config.command_capacity = 0u;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ);
    config.completion_batch_capacity = 0u;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    config = file_test_config(CFLOW_IO_FILE_READ);
    config.completion = NULL;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);

    file.impl = &file;
    config = file_test_config(CFLOW_IO_FILE_READ);
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_EINVAL);
    check_true(file.impl == &file);
    file.impl = NULL;

    check_equal(tt_remove_file(path), 0);
    config = file_test_config(CFLOW_IO_FILE_READ);
    status = cflow_io_file_open(&file, path, &config);
    check_not_equal(status, SALTS_OK);
    check_null(file.impl);
    free(path);
  }

  it("rejects unsupported file backends before creating the path") {
    char *path = tt_make_temp_file("cflow-io-file-unsupported-", ".bin");
    cflow_io_file file = {0};
    cflow_io_file_config config =
        file_test_config(CFLOW_IO_FILE_WRITE | CFLOW_IO_FILE_CREATE | CFLOW_IO_FILE_TRUNCATE);
    size_t length = 0u;
    char *contents;

    check_not_null(path);
    check_equal(tt_remove_file(path), 0);
    config.backend_kind = CFLOW_IO_NATIVE_POLL;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_ENOTSUP);
    check_null(file.impl);
    contents = tt_read_file(path, &length);
    check_null(contents);
    free(contents);
    free(path);
  }

  it("owns a compatible handle until close and quiescent destroy") {
    char *path = tt_make_temp_file("cflow-io-file-lifecycle-", ".bin");
    cflow_io_file file = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_READ | CFLOW_IO_FILE_WRITE);
    cflow_io_file_stats stats = {0};
    int status;

    check_not_null(path);
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    check_not_null(file.impl);
    check_true(cflow_io_file_operation_supported(&file, CFLOW_IO_NATIVE_FILE_READ_AT));
    check_true(cflow_io_file_operation_supported(&file, CFLOW_IO_NATIVE_FILE_WRITE_AT));
    check_true(cflow_io_file_get_stats(&file, &stats));
    check_equal(stats.operation_slots_in_use, (size_t)0u);
    check_false(stats.close_requested);
    check_equal(cflow_io_file_destroy(&file), SALTS_EBUSY);
    check_equal(cflow_io_file_close(&file), SALTS_OK);
    check_equal(cflow_io_file_close(&file), SALTS_EALREADY);
    check_true(cflow_io_file_is_quiescent(&file));
    check_true(cflow_io_file_get_stats(&file, &stats));
    check_true(stats.close_requested);
    check_equal(cflow_io_file_destroy(&file), SALTS_OK);
    check_null(file.impl);

    check_equal(cflow_io_file_open(&file, path, &config), SALTS_OK);
    check_equal(cflow_io_file_close(&file), SALTS_OK);
    check_true(cflow_io_file_is_quiescent(&file));
    check_equal(cflow_io_file_destroy(&file), SALTS_OK);
    check_null(file.impl);

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("round-trips explicit-offset writes and reads") {
    static const char payload[] = "payload";
    char received[16] = {0};
    char *path = tt_make_temp_file("cflow-io-file-roundtrip-", ".bin");
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_READ | CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    cflow_io_file_stats stats = {0};
    int status;

    check_not_null(path);
    config.completion_user = &probe;
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);

    probe.reentrant_file = &file;
    submitted = cflow_io_file_try_write_at(&file, 11u, payload, sizeof(payload) - 1u, 7u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_not_equal(submitted.request_id, (cflow_io_request_id)0u);
    check_equal(file_test_wait(&file, &probe, 1u), SALTS_OK);
    check_equal(probe.lease_ids[0], (cflow_io_lease_id)11u);
    check_equal(probe.operation_kinds[0], CFLOW_IO_NATIVE_FILE_WRITE_AT);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_OK);
    check_equal(probe.completions[0].bytes, sizeof(payload) - 1u);
    check_equal(probe.reentrant_status, SALTS_EBUSY);
    check_equal(probe.reentrant_progressed, (size_t)0u);

    submitted = cflow_io_file_try_read_at(&file, 12u, received, sizeof(received), 7u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_equal(file_test_wait(&file, &probe, 2u), SALTS_OK);
    check_equal(probe.lease_ids[1], (cflow_io_lease_id)12u);
    check_equal(probe.operation_kinds[1], CFLOW_IO_NATIVE_FILE_READ_AT);
    check_equal(probe.completions[1].kind, CFLOW_IO_COMPLETION_OK);
    check_equal(probe.completions[1].bytes, sizeof(payload) - 1u);
    check_equal(received, payload, sizeof(payload) - 1u);
    check_true(cflow_io_file_get_stats(&file, &stats));
    check_equal(stats.actor.accepted, (uint64_t)2u);
    check_equal(stats.actor.acknowledged, (uint64_t)2u);
    check_equal(stats.operation_slots_in_use, (size_t)0u);

    file_test_close_destroy(&file);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("flushes only when the selected native backend supports it") {
    char *path = tt_make_temp_file("cflow-io-file-flush-", ".bin");
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    int status;

    check_not_null(path);
    config.completion_user = &probe;
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    submitted = cflow_io_file_try_flush(&file, 51u);
    if (cflow_io_file_operation_supported(&file, CFLOW_IO_NATIVE_FILE_FLUSH)) {
      check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
      check_equal(file_test_wait(&file, &probe, 1u), SALTS_OK);
      check_equal(probe.operation_kinds[0], CFLOW_IO_NATIVE_FILE_FLUSH);
      check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_OK);
      check_equal(probe.completions[0].bytes, (size_t)0u);
    } else {
      check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_UNSUPPORTED);
      check_equal(probe.count, (size_t)0u);
    }
    file_test_close_destroy(&file);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects invalid shapes access mismatches and unsupported flush") {
    char byte = 0;
    char *path = tt_make_temp_file("cflow-io-file-validation-", ".bin");
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_READ);
    cflow_io_file_submit_result submitted;
    int status;

    check_not_null(path);
    config.completion_user = &probe;
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    submitted = cflow_io_file_try_read_at(&file, 1u, NULL, 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_INVALID_ARGUMENT);
    submitted = cflow_io_file_try_read_at(&file, 1u, &byte, 0u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_INVALID_ARGUMENT);
    submitted = cflow_io_file_try_read_at(&file, 1u, &byte, 2u, (uint64_t)INT64_MAX);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_INVALID_ARGUMENT);
    submitted = cflow_io_file_try_write_at(&file, 1u, &byte, 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCESS_DENIED);
    submitted = cflow_io_file_try_flush(&file, 1u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCESS_DENIED);
    check_equal(probe.count, (size_t)0u);
    file_test_close_destroy(&file);

    memset(&file, 0, sizeof(file));
    config = file_test_config(CFLOW_IO_FILE_WRITE);
    config.completion_user = &probe;
    check_equal(cflow_io_file_open(&file, path, &config), SALTS_OK);
    submitted = cflow_io_file_try_read_at(&file, 2u, &byte, 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCESS_DENIED);
#if defined(_WIN32)
    submitted = cflow_io_file_try_flush(&file, 2u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_UNSUPPORTED);
#endif
    file_test_close_destroy(&file);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("applies bounded backpressure and reuses acknowledged slots") {
    static const char first[] = "first";
    static const char second[] = "second";
    char *path = tt_make_temp_file("cflow-io-file-capacity-", ".bin");
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    int status;

    check_not_null(path);
    config.request_capacity = 1u;
    config.command_capacity = 1u;
    config.completion_batch_capacity = 1u;
    config.completion_user = &probe;
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    submitted = cflow_io_file_try_write_at(&file, 21u, first, sizeof(first) - 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    submitted = cflow_io_file_try_write_at(&file, 22u, second, sizeof(second) - 1u, 16u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_FULL);
    check_equal(file_test_wait(&file, &probe, 1u), SALTS_OK);
    submitted = cflow_io_file_try_write_at(&file, 22u, second, sizeof(second) - 1u, 16u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_equal(file_test_wait(&file, &probe, 2u), SALTS_OK);
    file_test_close_destroy(&file);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("cancels queued work during close before destroying the handle") {
    static const char payload[] = "cancelled";
    char *path = tt_make_temp_file("cflow-io-file-close-", ".bin");
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    int status;

    check_not_null(path);
    config.completion_user = &probe;
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    submitted = cflow_io_file_try_write_at(&file, 31u, payload, sizeof(payload) - 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_equal(cflow_io_file_close(&file), SALTS_OK);
    check_equal(cflow_io_file_destroy(&file), SALTS_EBUSY);
    check_equal(file_test_wait(&file, &probe, 1u), SALTS_OK);
    check_equal(probe.count, (size_t)1u);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_CANCELLED);
    check_true(cflow_io_file_is_quiescent(&file));
    check_equal(cflow_io_file_destroy(&file), SALTS_OK);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("delivers one terminal completion for explicit cancellation") {
    static const char payload[] = "cancel-request";
    char *path = tt_make_temp_file("cflow-io-file-cancel-", ".bin");
    cflow_io_file file = {0};
    file_completion_probe probe = {0};
    cflow_io_file_config config = file_test_config(CFLOW_IO_FILE_WRITE);
    cflow_io_file_submit_result submitted;
    int status;

    check_not_null(path);
    config.completion_user = &probe;
    status = cflow_io_file_open(&file, path, &config);
#if !defined(_WIN32) && !(defined(__APPLE__) && TARGET_OS_OSX)
    if (status != SALTS_OK) {
      info("native async file backend unavailable at runtime: %d", status);
      check_equal(tt_remove_file(path), 0);
      free(path);
      return;
    }
#endif
    check_equal(status, SALTS_OK);
    submitted = cflow_io_file_try_write_at(&file, 41u, payload, sizeof(payload) - 1u, 0u);
    check_equal(submitted.status, CFLOW_IO_FILE_SUBMIT_ACCEPTED);
    check_equal(cflow_io_file_try_cancel(&file, submitted.request_id), CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(file_test_wait(&file, &probe, 1u), SALTS_OK);
    check_equal(probe.count, (size_t)1u);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_CANCELLED);
    file_test_close_destroy(&file);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }
}
