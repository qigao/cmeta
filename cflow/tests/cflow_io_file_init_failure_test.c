#include <cflow/cflow.h>

#include "../src/io_native_internal.h"
#define TINYTEST_NO_MAIN
#include "tinytest.h"

#include <thread.h>

static size_t file_init_failure_backend_calls;
static size_t file_forget_backend_calls;

static void file_init_failure_completion(void *user, cflow_io_request_id request_id,
                                         cflow_io_lease_id lease_id,
                                         cflow_io_native_file_operation_kind operation_kind,
                                         const cflow_io_completion *completion) {
  (void)user;
  (void)request_id;
  (void)lease_id;
  (void)operation_kind;
  (void)completion;
}

static void file_init_failure_mutex_init(cmeta_mutex_t *mutex) {
  if (mutex != NULL) *mutex = NULL;
}

static bool file_init_failure_backend_supported(cflow_io_native_backend_kind kind) {
  (void)kind;
  return true;
}

static bool
file_init_failure_operation_supported(cflow_io_native_backend_kind kind,
                                      cflow_io_native_file_operation_kind operation_kind) {
  (void)kind;
  (void)operation_kind;
  return true;
}

static int file_init_failure_backend_init(cflow_io_native_backend *backend,
                                          const cflow_io_native_backend_config *config) {
  (void)backend;
  (void)config;
  ++file_init_failure_backend_calls;
  return SALTS_EIO;
}

static int file_forget_backend_busy(cflow_io_native_impl *impl, uintptr_t closed_handle) {
  (void)impl;
  (void)closed_handle;
  ++file_forget_backend_calls;
  return SALTS_EBUSY;
}

#define cmeta_mutex_init file_init_failure_mutex_init
#define cflow_io_native_backend_supported file_init_failure_backend_supported
#define cflow_io_native_backend_file_operation_supported file_init_failure_operation_supported
#define cflow_io_native_backend_init file_init_failure_backend_init
#define cflow_io_file_runtime_init cflow_io_file_runtime_init_with_init_failure
#define cflow_io_file_runtime_run_ready cflow_io_file_runtime_run_ready_with_init_failure
#define cflow_io_file_runtime_close cflow_io_file_runtime_close_with_init_failure
#define cflow_io_file_runtime_is_quiescent cflow_io_file_runtime_is_quiescent_with_init_failure
#define cflow_io_file_runtime_get_stats cflow_io_file_runtime_get_stats_with_init_failure
#define cflow_io_file_runtime_destroy cflow_io_file_runtime_destroy_with_init_failure
#define cflow_io_file_open cflow_io_file_open_with_init_failure
#define cflow_io_file_operation_supported cflow_io_file_operation_supported_with_init_failure
#define cflow_io_file_try_read_at cflow_io_file_try_read_at_with_init_failure
#define cflow_io_file_try_write_at cflow_io_file_try_write_at_with_init_failure
#define cflow_io_file_try_flush cflow_io_file_try_flush_with_init_failure
#define cflow_io_file_try_cancel cflow_io_file_try_cancel_with_init_failure
#define cflow_io_file_run_ready cflow_io_file_run_ready_with_init_failure
#define cflow_io_file_close cflow_io_file_close_with_init_failure
#define cflow_io_file_is_quiescent cflow_io_file_is_quiescent_with_init_failure
#define cflow_io_file_get_stats cflow_io_file_get_stats_with_init_failure
#define cflow_io_file_destroy cflow_io_file_destroy_with_init_failure
int cflow_io_file_runtime_close_with_init_failure(cflow_io_file_runtime *runtime);
int cflow_io_file_runtime_destroy_with_init_failure(cflow_io_file_runtime *runtime);
#include "../src/io_file.c"
#undef cflow_io_file_runtime_destroy
#undef cflow_io_file_runtime_get_stats
#undef cflow_io_file_runtime_is_quiescent
#undef cflow_io_file_runtime_close
#undef cflow_io_file_runtime_run_ready
#undef cflow_io_file_runtime_init
#undef cflow_io_file_destroy
#undef cflow_io_file_get_stats
#undef cflow_io_file_is_quiescent
#undef cflow_io_file_close
#undef cflow_io_file_run_ready
#undef cflow_io_file_try_cancel
#undef cflow_io_file_try_flush
#undef cflow_io_file_try_write_at
#undef cflow_io_file_try_read_at
#undef cflow_io_file_operation_supported
#undef cflow_io_file_open
#undef cflow_io_native_backend_init
#undef cflow_io_native_backend_file_operation_supported
#undef cflow_io_native_backend_supported
#undef cmeta_mutex_init

spec("CFlow async file facade initialization failures") {
  it("returns ENOMEM before backend initialization when its mutex is unavailable") {
    cflow_io_file file = {0};
    const cflow_io_file_config config = {.backend_kind = CFLOW_IO_NATIVE_IOCP,
                                         .request_capacity = 1u,
                                         .command_capacity = 1u,
                                         .completion_batch_capacity = 1u,
                                         .open_flags = CFLOW_IO_FILE_READ,
                                         .completion = file_init_failure_completion};

    file_init_failure_backend_calls = 0u;
    check_equal(cflow_io_file_open_with_init_failure(&file, "mutex-init-must-fail", &config),
                SALTS_ENOMEM);
    check_equal(file_init_failure_backend_calls, (size_t)0u);
    check_null(file.impl);
  }

  it("forgets an io_uring file without consulting a global busy backend") {
    static const cflow_io_native_impl_ops ops = {.forget_file = file_forget_backend_busy};
    cflow_io_native_impl impl = {.ops = &ops, .kind = CFLOW_IO_NATIVE_IO_URING};
    cflow_io_native_backend backend = {.impl = &impl};

    file_forget_backend_calls = 0u;
    check_equal(cflow_io_native_backend_forget_file(&backend, (uintptr_t)7u), SALTS_OK);
    check_equal(file_forget_backend_calls, (size_t)0u);
  }
}
