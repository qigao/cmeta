/**
 * @file cmeta_process.h
 * @brief Cross-platform child process management without an event-loop dependency.
 *
 * A cmeta_process_t exclusively owns the child process, its process group or
 * Windows Job Object, redirected standard-I/O handles, monitor thread, and
 * terminal result. Operations are thread-safe except destroy, which requires
 * exclusive ownership. Destroying a running handle terminates and reaps the
 * owned process tree.
 */

#ifndef SALTS_PROCESS_H
#define SALTS_PROCESS_H

#include "platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_PROCESS_DEFAULT_MAX_OUTPUT_BYTES (16U * 1024U * 1024U)

typedef struct cmeta_process_s cmeta_process_t;

typedef enum {
  SALTS_PROCESS_STARTING = 0,
  SALTS_PROCESS_RUNNING,
  SALTS_PROCESS_EXITED,
  SALTS_PROCESS_SIGNALED,
  SALTS_PROCESS_TIMED_OUT,
  SALTS_PROCESS_TERMINATED,
  SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED,
  SALTS_PROCESS_WAIT_FAILED
} cmeta_process_state_t;

typedef enum {
  SALTS_PROCESS_PIPE_STDIN = 1U << 0,
  SALTS_PROCESS_CAPTURE_STDOUT = 1U << 1,
  SALTS_PROCESS_CAPTURE_STDERR = 1U << 2,
  /** Do not inherit parent variables; env becomes the complete child environment. */
  SALTS_PROCESS_CLEAN_ENVIRONMENT = 1U << 3,
  /**
   * Interpret program as a shell command; args must be NULL. Remaining job/
   * process-group members are terminated when the root shell exits, before
   * publishing its result. Background jobs cannot outlive this command owner.
   */
  SALTS_PROCESS_SHELL_COMMAND = 1U << 4
} cmeta_process_flags_t;

typedef struct {
  /**
   * Executable path/name, or command text with SALTS_PROCESS_SHELL_COMMAND.
   * Explicit shell mode uses /bin/sh -c on POSIX and the system cmd.exe
   * /d /s /c on Windows. Command text is interpreted verbatim by that shell;
   * it is not an argv escaping API. Empty commands are rejected.
   */
  const char *program;
  /** NULL-terminated arguments excluding argv[0], or NULL for no arguments. */
  const char *const *args;
  /**
   * NULL inherits the parent environment. A non-NULL, NULL-terminated list of
   * KEY=VALUE entries overrides matching parent variables. With
   * SALTS_PROCESS_CLEAN_ENVIRONMENT, this list is the complete child
   * environment and NULL creates an empty environment.
   */
  const char *const *env;
  /** Child working directory, or NULL to inherit the current directory. */
  const char *cwd;
  /** Combination of cmeta_process_flags_t. */
  unsigned int flags;
  /** Execution deadline in milliseconds. Zero disables the deadline. */
  uint64_t timeout_ms;
  /** Shared stdout/stderr capture limit. Zero selects the default limit. */
  size_t max_output_bytes;
} cmeta_process_options_t;

typedef struct {
  cmeta_process_state_t state;
  int pid;
  int exit_code;
  int term_signal;
  int error_code;
} cmeta_process_result_t;

/** Select parent standard-handle inheritance for one spawn binding. */
#define SALTS_PROCESS_STDIO_INHERIT UINTPTR_MAX

/**
 * Child-side standard handles borrowed until cmeta_process_spawn_with_stdio()
 * returns. Each value is a native HANDLE/fd or SALTS_PROCESS_STDIO_INHERIT.
 * The spawn call never closes or consumes a supplied handle.
 */
typedef struct {
  uintptr_t stdin_handle;
  uintptr_t stdout_handle;
  uintptr_t stderr_handle;
} cmeta_process_stdio_bindings_t;

/** Fill options with defaults: stdout/stderr capture and no execution deadline. */
SALTS_C_API void cmeta_process_options_init(cmeta_process_options_t *options);

/** Return the stable string name of a process state. */
SALTS_C_API const char *cmeta_process_state_name(cmeta_process_state_t state);

/**
 * Start a child process and its monitor thread.
 *
 * On success, *out_process owns all resources and must be destroyed. On
 * failure, *out_process is NULL and the return value is a Salts or
 * negative native platform error code.
 */
SALTS_C_API int cmeta_process_spawn(const cmeta_process_options_t *options,
                                  cmeta_process_t **out_process);

/**
 * Start a child with explicitly borrowed child-side standard handles.
 *
 * A supplied stdin handle conflicts with SALTS_PROCESS_PIPE_STDIN. Supplied
 * stdout/stderr handles conflict with their corresponding capture flags.
 * Contradictory bindings return SALTS_EINVAL without consuming any handle.
 * The returned process owns its lifecycle but never reads or closes a supplied
 * parent handle.
 */
SALTS_C_API int cmeta_process_spawn_with_stdio(const cmeta_process_options_t *options,
                                               const cmeta_process_stdio_bindings_t *bindings,
                                               cmeta_process_t **out_process);

/** Return the owned child PID, or -1 for an invalid handle. */
SALTS_C_API int cmeta_process_pid(const cmeta_process_t *process);

/** Return the current lifecycle state. */
SALTS_C_API cmeta_process_state_t cmeta_process_state(const cmeta_process_t *process);

/** Return true while the owned child has not reached a terminal state. */
SALTS_C_API bool cmeta_process_is_running(const cmeta_process_t *process);

/**
 * Copy the terminal result without blocking.
 *
 * Returns SALTS_OK when terminal, SALTS_EBUSY while running, or SALTS_EINVAL
 * for invalid arguments.
 */
SALTS_C_API int cmeta_process_poll(const cmeta_process_t *process,
                                 cmeta_process_result_t *out_result);

/** Wait indefinitely for the terminal result. */
SALTS_C_API int cmeta_process_wait(cmeta_process_t *process, cmeta_process_result_t *out_result);

/**
 * Wait at most timeout_ms for the terminal result.
 *
 * SALTS_ETIMEDOUT only describes this wait operation and does not terminate
 * the process. Use options.timeout_ms for an execution deadline.
 */
SALTS_C_API int cmeta_process_wait_for(cmeta_process_t *process, uint64_t timeout_ms,
                                     cmeta_process_result_t *out_result);

/** Terminate the owned process tree. This operation is idempotent. */
SALTS_C_API int cmeta_process_terminate(cmeta_process_t *process);

/**
 * Queue bytes for the child's stdin pipe.
 *
 * This call may block until the OS pipe accepts the bytes. It returns
 * SALTS_EPIPE after stdin has been closed or when no stdin pipe was requested.
 */
SALTS_C_API int cmeta_process_write_stdin(cmeta_process_t *process, const void *data, size_t size,
                                        size_t *out_written);

/** Close the parent's stdin pipe so the child observes EOF. */
SALTS_C_API int cmeta_process_close_stdin(cmeta_process_t *process);

/**
 * Consume currently captured stdout bytes.
 *
 * Returns SALTS_OK with *out_read possibly zero, or SALTS_EOF after the stream
 * is closed and all captured bytes have been consumed.
 */
SALTS_C_API int cmeta_process_read_stdout(cmeta_process_t *process, void *buffer, size_t capacity,
                                        size_t *out_read);

/** Consume currently captured stderr bytes; semantics match stdout. */
SALTS_C_API int cmeta_process_read_stderr(cmeta_process_t *process, void *buffer, size_t capacity,
                                        size_t *out_read);

/**
 * Check whether a PID currently names a live process.
 *
 * This is observation only. Managed termination uses the retained native
 * handle to avoid PID-reuse races.
 */
SALTS_C_API int cmeta_process_is_pid_alive(int pid, bool *out_alive);

/** Terminate, reap, close, and free a process handle; no concurrent calls are allowed. */
SALTS_C_API void cmeta_process_destroy(cmeta_process_t *process);

#ifdef __cplusplus
}
#endif

#endif /* SALTS_PROCESS_H */
