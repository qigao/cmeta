/**
 * @file cmeta_process.c
 * @brief Native Windows/POSIX child process management backend.
 */

#include "cmeta_process.h"

#include "cmeta_error.h"
#include "tstr.h"
#include "cmeta_thread.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SALTS_PROCESS_POLL_INTERVAL_MS 10U
#define SALTS_PROCESS_WINDOWS_BLOCK_MAX 32767U

#ifdef _WIN32
  #include <wchar.h>
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <poll.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #ifdef __APPLE__
    #include <sys/sysctl.h>
  #endif

extern char **environ;
#endif

struct cmeta_process_s {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  cmeta_mutex_t stdin_mutex;
  cmeta_thread_t monitor_thread;
  int monitor_started;
  int monitor_joined;

  cmeta_process_state_t state;
  cmeta_process_result_t result;
  int terminate_requested;
  int native_active;
  bool shell_command;
  uint64_t timeout_ms;
  size_t max_output_bytes;
  size_t captured_bytes;

  tstr stdout_data;
  tstr stderr_data;
  size_t stdout_offset;
  size_t stderr_offset;
  atomic_int stdout_open;
  atomic_int stderr_open;

#ifdef _WIN32
  HANDLE process_handle;
  HANDLE job_handle;
  HANDLE stdin_write;
  HANDLE stdout_read;
  HANDLE stderr_read;
#else
  pid_t pid;
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
#endif
};

static int process_state_terminal(cmeta_process_state_t state) {
  return state == SALTS_PROCESS_EXITED || state == SALTS_PROCESS_SIGNALED ||
         state == SALTS_PROCESS_TIMED_OUT || state == SALTS_PROCESS_TERMINATED ||
         state == SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED || state == SALTS_PROCESS_WAIT_FAILED;
}

static int process_validate_options(const cmeta_process_options_t *options) {
  const unsigned int known_flags =
      SALTS_PROCESS_PIPE_STDIN | SALTS_PROCESS_CAPTURE_STDOUT | SALTS_PROCESS_CAPTURE_STDERR |
      SALTS_PROCESS_CLEAN_ENVIRONMENT | SALTS_PROCESS_SHELL_COMMAND;
  if (!options || !options->program || options->program[0] == '\0') return SALTS_EINVAL;
  if ((options->flags & ~known_flags) != 0U) return SALTS_EINVAL;
  if ((options->flags & SALTS_PROCESS_SHELL_COMMAND) != 0U && options->args != NULL)
    return SALTS_EINVAL;
  return SALTS_OK;
}

static int process_validate_stdio_conflicts(const cmeta_process_options_t *options,
                                            const cmeta_process_stdio_bindings_t *bindings) {
  if ((bindings->stdin_handle != SALTS_PROCESS_STDIO_INHERIT &&
       (options->flags & SALTS_PROCESS_PIPE_STDIN) != 0U) ||
      (bindings->stdout_handle != SALTS_PROCESS_STDIO_INHERIT &&
       (options->flags & SALTS_PROCESS_CAPTURE_STDOUT) != 0U) ||
      (bindings->stderr_handle != SALTS_PROCESS_STDIO_INHERIT &&
       (options->flags & SALTS_PROCESS_CAPTURE_STDERR) != 0U))
    return SALTS_EINVAL;
  return SALTS_OK;
}

static int process_init(cmeta_process_t *process, const cmeta_process_options_t *options) {
  memset(process, 0, sizeof(*process));
#ifdef _WIN32
  process->process_handle = NULL;
  process->job_handle = NULL;
  process->stdin_write = INVALID_HANDLE_VALUE;
  process->stdout_read = INVALID_HANDLE_VALUE;
  process->stderr_read = INVALID_HANDLE_VALUE;
#else
  process->pid = -1;
  process->stdin_fd = -1;
  process->stdout_fd = -1;
  process->stderr_fd = -1;
#endif
  cmeta_mutex_init(&process->mutex);
  cmeta_cond_init(&process->changed);
  cmeta_mutex_init(&process->stdin_mutex);
  if (!process->mutex || !process->changed || !process->stdin_mutex) return SALTS_ENOMEM;

  process->state = SALTS_PROCESS_STARTING;
  process->result.state = SALTS_PROCESS_STARTING;
  process->result.pid = -1;
  process->result.exit_code = -1;
  process->timeout_ms = options->timeout_ms;
  process->shell_command = (options->flags & SALTS_PROCESS_SHELL_COMMAND) != 0u;
  process->max_output_bytes = options->max_output_bytes != 0
                                  ? options->max_output_bytes
                                  : SALTS_PROCESS_DEFAULT_MAX_OUTPUT_BYTES;
  process->stdout_data = tstr_new();
  process->stderr_data = tstr_new();
  if (!process->stdout_data || !process->stderr_data) return SALTS_ENOMEM;
  return SALTS_OK;
}

static void process_finish(cmeta_process_t *process, cmeta_process_state_t state, int exit_code,
                           int term_signal, int error_code) {
  cmeta_mutex_lock(&process->mutex);
  process->state = state;
  process->result.state = state;
  process->result.exit_code = exit_code;
  process->result.term_signal = term_signal;
  process->result.error_code = error_code;
  process->stdout_open = 0;
  process->stderr_open = 0;
  cmeta_cond_broadcast(&process->changed);
  cmeta_mutex_unlock(&process->mutex);
}

static int process_append_output(cmeta_process_t *process, int is_stdout, const char *data,
                                 size_t size) {
  tstr *target;
  tstr appended;
  size_t remaining;
  size_t accepted;

  if (size == 0) return SALTS_OK;
  cmeta_mutex_lock(&process->mutex);
  remaining = process->captured_bytes < process->max_output_bytes
                  ? process->max_output_bytes - process->captured_bytes
                  : 0;
  accepted = size < remaining ? size : remaining;
  target = is_stdout ? &process->stdout_data : &process->stderr_data;
  if (accepted > 0) {
    appended = tstr_cat_len(*target, data, accepted);
    if (!appended) {
      cmeta_mutex_unlock(&process->mutex);
      return SALTS_ENOMEM;
    }
    *target = appended;
    process->captured_bytes += accepted;
  }
  cmeta_cond_broadcast(&process->changed);
  cmeta_mutex_unlock(&process->mutex);
  return accepted == size ? SALTS_OK : SALTS_ERANGE;
}

static int process_read_buffer(cmeta_process_t *process, int is_stdout, void *buffer,
                               size_t capacity, size_t *out_read) {
  tstr data;
  size_t *offset;
  size_t available;
  size_t count;
  int stream_open;

  if (!process || !out_read || (capacity > 0 && !buffer)) return SALTS_EINVAL;
  *out_read = 0;
  cmeta_mutex_lock(&process->mutex);
  data = is_stdout ? process->stdout_data : process->stderr_data;
  offset = is_stdout ? &process->stdout_offset : &process->stderr_offset;
  stream_open = is_stdout ? process->stdout_open : process->stderr_open;
  available = tstr_len(data) - *offset;
  count = available < capacity ? available : capacity;
  if (count > 0) {
    memcpy(buffer, data + *offset, count);
    *offset += count;
    *out_read = count;
  }
  cmeta_mutex_unlock(&process->mutex);
  return count == 0 && !stream_open ? SALTS_EOF : SALTS_OK;
}

static void process_join_monitor(cmeta_process_t *process) {
  int should_join = 0;
  cmeta_mutex_lock(&process->mutex);
  if (process->monitor_started && !process->monitor_joined) {
    process->monitor_joined = 1;
    should_join = 1;
  }
  cmeta_mutex_unlock(&process->mutex);
  if (should_join) cmeta_thread_join(&process->monitor_thread);
}

void cmeta_process_options_init(cmeta_process_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->flags = SALTS_PROCESS_CAPTURE_STDOUT | SALTS_PROCESS_CAPTURE_STDERR;
  options->max_output_bytes = SALTS_PROCESS_DEFAULT_MAX_OUTPUT_BYTES;
}

const char *cmeta_process_state_name(cmeta_process_state_t state) {
  switch (state) {
  case SALTS_PROCESS_STARTING:
    return "starting";
  case SALTS_PROCESS_RUNNING:
    return "running";
  case SALTS_PROCESS_EXITED:
    return "exited";
  case SALTS_PROCESS_SIGNALED:
    return "signaled";
  case SALTS_PROCESS_TIMED_OUT:
    return "timed_out";
  case SALTS_PROCESS_TERMINATED:
    return "terminated";
  case SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED:
    return "output_limit_exceeded";
  case SALTS_PROCESS_WAIT_FAILED:
    return "wait_failed";
  }
  return "unknown";
}

int cmeta_process_pid(const cmeta_process_t *process) {
  int pid;
  if (!process) return -1;
  cmeta_mutex_lock((cmeta_mutex_t *)&process->mutex);
  pid = process->result.pid;
  cmeta_mutex_unlock((cmeta_mutex_t *)&process->mutex);
  return pid;
}

cmeta_process_state_t cmeta_process_state(const cmeta_process_t *process) {
  cmeta_process_state_t state;
  if (!process) return SALTS_PROCESS_WAIT_FAILED;
  cmeta_mutex_lock((cmeta_mutex_t *)&process->mutex);
  state = process->state;
  cmeta_mutex_unlock((cmeta_mutex_t *)&process->mutex);
  return state;
}

bool cmeta_process_is_running(const cmeta_process_t *process) {
  return process && !process_state_terminal(cmeta_process_state(process));
}

int cmeta_process_poll(const cmeta_process_t *process, cmeta_process_result_t *out_result) {
  if (!process || !out_result) return SALTS_EINVAL;
  cmeta_mutex_lock((cmeta_mutex_t *)&process->mutex);
  if (!process_state_terminal(process->state)) {
    cmeta_mutex_unlock((cmeta_mutex_t *)&process->mutex);
    return SALTS_EBUSY;
  }
  *out_result = process->result;
  cmeta_mutex_unlock((cmeta_mutex_t *)&process->mutex);
  return SALTS_OK;
}

int cmeta_process_wait(cmeta_process_t *process, cmeta_process_result_t *out_result) {
  if (!process || !out_result) return SALTS_EINVAL;
  cmeta_mutex_lock(&process->mutex);
  while (!process_state_terminal(process->state)) {
    cmeta_cond_wait(&process->changed, &process->mutex);
  }
  *out_result = process->result;
  cmeta_mutex_unlock(&process->mutex);
  return SALTS_OK;
}

int cmeta_process_wait_for(cmeta_process_t *process, uint64_t timeout_ms,
                           cmeta_process_result_t *out_result) {
  uint64_t deadline;
  int rc = SALTS_OK;
  if (!process || !out_result) return SALTS_EINVAL;
  deadline = cmeta_monotonic_ms();
  deadline = timeout_ms > UINT64_MAX - deadline ? UINT64_MAX : deadline + timeout_ms;

  cmeta_mutex_lock(&process->mutex);
  while (!process_state_terminal(process->state)) {
    uint64_t now = cmeta_monotonic_ms();
    uint64_t remaining_ms;
    if (now >= deadline) {
      rc = SALTS_ETIMEDOUT;
      break;
    }
    remaining_ms = deadline - now;
    if (remaining_ms > 1000U) remaining_ms = 1000U;
    if (cmeta_cond_timedwait(&process->changed, &process->mutex, remaining_ms * 1000000ULL) != 0 &&
        !process_state_terminal(process->state) && cmeta_monotonic_ms() >= deadline) {
      rc = SALTS_ETIMEDOUT;
      break;
    }
  }
  if (rc == SALTS_OK) *out_result = process->result;
  cmeta_mutex_unlock(&process->mutex);
  return rc;
}

int cmeta_process_read_stdout(cmeta_process_t *process, void *buffer, size_t capacity,
                              size_t *out_read) {
  return process_read_buffer(process, 1, buffer, capacity, out_read);
}

int cmeta_process_read_stderr(cmeta_process_t *process, void *buffer, size_t capacity,
                              size_t *out_read) {
  return process_read_buffer(process, 0, buffer, capacity, out_read);
}

#ifdef _WIN32

static int win32_error_from_code(DWORD error) {
  return error > (DWORD)INT_MAX ? SALTS_UNKNOWN : -(int)error;
}

static int win32_error(void) { return win32_error_from_code(GetLastError()); }

static void close_win_handle(HANDLE *handle) {
  if (*handle != NULL && *handle != INVALID_HANDLE_VALUE) CloseHandle(*handle);
  *handle = INVALID_HANDLE_VALUE;
}

static int duplicate_standard_handle(DWORD standard_handle, DWORD fallback_access,
                                     HANDLE *out_handle) {
  HANDLE source = GetStdHandle(standard_handle);
  if (source != NULL && source != INVALID_HANDLE_VALUE &&
      DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), out_handle, 0, TRUE,
                      DUPLICATE_SAME_ACCESS)) {
    return SALTS_OK;
  }
  *out_handle = CreateFileW(L"NUL", fallback_access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (*out_handle == INVALID_HANDLE_VALUE) return win32_error();
  if (!SetHandleInformation(*out_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    int rc = win32_error();
    close_win_handle(out_handle);
    return rc;
  }
  return SALTS_OK;
}

static int duplicate_bound_handle(uintptr_t source_value, HANDLE *out_handle) {
  HANDLE source = (HANDLE)source_value;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), out_handle, 0, TRUE,
                       DUPLICATE_SAME_ACCESS))
    return win32_error();
  return SALTS_OK;
}

static int utf8_to_utf16(const char *input, wchar_t **out) {
  int length;
  wchar_t *wide;
  if (!input) {
    *out = NULL;
    return SALTS_OK;
  }
  length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
  if (length <= 0) return win32_error();
  wide = (wchar_t *)malloc((size_t)length * sizeof(wchar_t));
  if (!wide) return SALTS_ENOMEM;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, wide, length) <= 0) {
    int rc = win32_error();
    free(wide);
    return rc;
  }
  *out = wide;
  return SALTS_OK;
}

static int windows_arg_needs_quotes(const wchar_t *arg) {
  if (*arg == L'\0') return 1;
  for (; *arg; ++arg) {
    if (*arg == L' ' || *arg == L'\t' || *arg == L'\n' || *arg == L'\v' || *arg == L'"') return 1;
  }
  return 0;
}

static int windows_quoted_length(const wchar_t *arg, size_t *out_length) {
  size_t length = wcslen(arg);
  if (windows_arg_needs_quotes(arg)) {
    if (length > (SIZE_MAX - 3U) / 2U) return SALTS_ENOMEM;
    *out_length = length * 2U + 3U;
  } else {
    if (length == SIZE_MAX) return SALTS_ENOMEM;
    *out_length = length + 1U;
  }
  return SALTS_OK;
}

static wchar_t *windows_quote_arg(wchar_t *output, const wchar_t *arg) {
  size_t slashes = 0;
  const wchar_t *cursor;
  if (!windows_arg_needs_quotes(arg)) {
    while (*arg)
      *output++ = *arg++;
    return output;
  }
  *output++ = L'"';
  for (cursor = arg; *cursor; ++cursor) {
    if (*cursor == L'\\') {
      ++slashes;
      continue;
    }
    if (*cursor == L'"') {
      while (slashes-- > 0) {
        *output++ = L'\\';
        *output++ = L'\\';
      }
      *output++ = L'\\';
      *output++ = L'"';
      slashes = 0;
      continue;
    }
    while (slashes-- > 0)
      *output++ = L'\\';
    slashes = 0;
    *output++ = *cursor;
  }
  while (slashes-- > 0) {
    *output++ = L'\\';
    *output++ = L'\\';
  }
  *output++ = L'"';
  return output;
}

/* cmd /s removes the outer pair only; CRT argv quoting corrupts inner quotes. */
static int build_windows_shell_command(const char *text, wchar_t **out_command) {
  static const wchar_t suffix[] = L"\\cmd.exe\" /d /s /c \"";
  wchar_t *body = NULL;
  wchar_t *command;
  UINT directory_capacity = GetSystemDirectoryW(NULL, 0);
  UINT directory_length;
  size_t body_length;
  size_t suffix_length = sizeof(suffix) / sizeof(suffix[0]) - 1u;
  size_t capacity;
  size_t offset;
  int rc;
  if (directory_capacity == 0u) return win32_error();
  rc = utf8_to_utf16(text, &body);
  if (rc != SALTS_OK) return rc;
  body_length = wcslen(body);
  if (directory_capacity >= SALTS_PROCESS_WINDOWS_BLOCK_MAX ||
      body_length >= SALTS_PROCESS_WINDOWS_BLOCK_MAX - directory_capacity ||
      suffix_length + 3u >= SALTS_PROCESS_WINDOWS_BLOCK_MAX - directory_capacity - body_length) {
    free(body);
    return SALTS_EINVAL;
  }
  capacity = directory_capacity + suffix_length + body_length + 3u;
  command = (wchar_t *)calloc(capacity, sizeof(*command));
  if (command == NULL) {
    free(body);
    return SALTS_ENOMEM;
  }
  command[0] = L'"';
  directory_length = GetSystemDirectoryW(command + 1u, directory_capacity);
  if (directory_length == 0u || directory_length >= directory_capacity) {
    rc = directory_length == 0u ? win32_error() : SALTS_ERANGE;
    free(body);
    free(command);
    return rc;
  }
  offset = 1u + directory_length;
  memcpy(command + offset, suffix, suffix_length * sizeof(*command));
  offset += suffix_length;
  memcpy(command + offset, body, body_length * sizeof(*command));
  command[offset + body_length] = L'"';
  free(body);
  *out_command = command;
  return SALTS_OK;
}

static int build_windows_command(const cmeta_process_options_t *options, wchar_t **out_command) {
  size_t count = 1;
  size_t total = 0;
  size_t i;
  wchar_t **items;
  wchar_t *command;
  wchar_t *cursor;
  int rc;

  if ((options->flags & SALTS_PROCESS_SHELL_COMMAND) != 0U)
    return build_windows_shell_command(options->program, out_command);

  if (options->args) {
    while (options->args[count - 1]) {
      if (count == SIZE_MAX / sizeof(*items)) return SALTS_ENOMEM;
      ++count;
    }
  }
  items = (wchar_t **)calloc(count, sizeof(*items));
  if (!items) return SALTS_ENOMEM;
  rc = utf8_to_utf16(options->program, &items[0]);
  for (i = 1; rc == SALTS_OK && i < count; ++i)
    rc = utf8_to_utf16(options->args[i - 1], &items[i]);
  if (rc != SALTS_OK) goto cleanup;
  for (i = 0; i < count; ++i) {
    size_t item_length;
    rc = windows_quoted_length(items[i], &item_length);
    if (rc != SALTS_OK || item_length > SALTS_PROCESS_WINDOWS_BLOCK_MAX - total) {
      rc = rc != SALTS_OK ? rc : SALTS_EINVAL;
      goto cleanup;
    }
    total += item_length;
  }
  if (total >= SALTS_PROCESS_WINDOWS_BLOCK_MAX) {
    rc = SALTS_EINVAL;
    goto cleanup;
  }
  command = (wchar_t *)calloc(total + 1, sizeof(wchar_t));
  if (!command) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  cursor = command;
  for (i = 0; i < count; ++i) {
    if (i != 0) *cursor++ = L' ';
    cursor = windows_quote_arg(cursor, items[i]);
  }
  *cursor = L'\0';
  *out_command = command;

cleanup:
  for (i = 0; i < count; ++i)
    free(items[i]);
  free(items);
  return rc;
}

static size_t windows_env_key_length(const wchar_t *entry) {
  const wchar_t *equals = wcschr(entry + (entry[0] == L'=' ? 1 : 0), L'=');
  return equals ? (size_t)(equals - entry) : 0;
}

static int windows_env_key_equal(const wchar_t *left, const wchar_t *right) {
  size_t left_length = windows_env_key_length(left);
  size_t right_length = windows_env_key_length(right);
  return left_length != 0 && left_length == right_length &&
         _wcsnicmp(left, right, left_length) == 0;
}

static int windows_env_entry_compare(const void *left, const void *right) {
  const wchar_t *const *left_entry = (const wchar_t *const *)left;
  const wchar_t *const *right_entry = (const wchar_t *const *)right;
  return _wcsicmp(*left_entry, *right_entry);
}

static int build_windows_environment(const char *const *env, int clean_environment,
                                     wchar_t **out_environment) {
  LPWCH parent_block = NULL;
  wchar_t **overrides = NULL;
  wchar_t **entries = NULL;
  size_t override_count = 0;
  size_t parent_count = 0;
  size_t entry_count = 0;
  size_t total = 1;
  size_t i;
  wchar_t *block = NULL;
  wchar_t *cursor;
  int rc = SALTS_OK;
  if (!env && !clean_environment) {
    *out_environment = NULL;
    return SALTS_OK;
  }

  if (env) {
    while (env[override_count])
      ++override_count;
  }
  overrides = (wchar_t **)calloc(override_count, sizeof(*overrides));
  if (override_count != 0 && !overrides) return SALTS_ENOMEM;
  for (i = 0; i < override_count; ++i) {
    const char *equals = strchr(env[i], '=');
    if (!equals || equals == env[i]) {
      rc = SALTS_EINVAL;
      goto cleanup;
    }
    rc = utf8_to_utf16(env[i], &overrides[i]);
    if (rc != SALTS_OK) goto cleanup;
    for (size_t j = 0; j < i; ++j) {
      if (windows_env_key_equal(overrides[i], overrides[j])) {
        rc = SALTS_EINVAL;
        goto cleanup;
      }
    }
  }

  if (!clean_environment) {
    parent_block = GetEnvironmentStringsW();
    if (!parent_block) {
      rc = win32_error();
      goto cleanup;
    }
    for (cursor = parent_block; *cursor; cursor += wcslen(cursor) + 1)
      ++parent_count;
  }
  if (override_count > SIZE_MAX - parent_count ||
      parent_count + override_count > SIZE_MAX / sizeof(*entries)) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  entries = (wchar_t **)calloc(parent_count + override_count, sizeof(*entries));
  if (parent_count + override_count != 0 && !entries) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  if (parent_block) {
    for (cursor = parent_block; *cursor; cursor += wcslen(cursor) + 1) {
      int overridden = 0;
      for (i = 0; i < override_count; ++i) {
        if (windows_env_key_equal(cursor, overrides[i])) {
          overridden = 1;
          break;
        }
      }
      if (!overridden) entries[entry_count++] = cursor;
    }
  }
  for (i = 0; i < override_count; ++i)
    entries[entry_count++] = overrides[i];
  if (entry_count > 1) qsort(entries, entry_count, sizeof(*entries), windows_env_entry_compare);
  if (entry_count == 0) total = 2;
  for (i = 0; i < entry_count; ++i) {
    size_t length = wcslen(entries[i]) + 1U;
    if (length > SALTS_PROCESS_WINDOWS_BLOCK_MAX - total) {
      rc = SALTS_EINVAL;
      goto cleanup;
    }
    total += length;
  }
  block = (wchar_t *)calloc(total, sizeof(wchar_t));
  if (!block) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  cursor = block;
  for (i = 0; i < entry_count; ++i) {
    size_t length = wcslen(entries[i]) + 1;
    memcpy(cursor, entries[i], length * sizeof(wchar_t));
    cursor += length;
  }
  *cursor = L'\0';
  *out_environment = block;

cleanup:
  if (parent_block) FreeEnvironmentStringsW(parent_block);
  for (i = 0; i < override_count; ++i)
    free(overrides[i]);
  free(overrides);
  free(entries);
  return rc;
}

static int process_platform_terminate(cmeta_process_t *process) {
  HANDLE job;
  HANDLE child;
  cmeta_mutex_lock(&process->mutex);
  if (process_state_terminal(process->state) || !process->native_active) {
    cmeta_mutex_unlock(&process->mutex);
    return SALTS_OK;
  }
  process->terminate_requested = 1;
  job = process->job_handle;
  child = process->process_handle;
  cmeta_mutex_unlock(&process->mutex);
  if (job != NULL && job != INVALID_HANDLE_VALUE) {
    return TerminateJobObject(job, 1) ? SALTS_OK : win32_error();
  }
  return TerminateProcess(child, 1) ? SALTS_OK : win32_error();
}

static int drain_windows_pipe(cmeta_process_t *process, HANDLE pipe, int is_stdout,
                              atomic_int *open) {
  char buffer[4096];
  while (*open) {
    DWORD available = 0;
    DWORD read_count = 0;
    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        *open = 0;
        return SALTS_OK;
      }
      return -(int)error;
    }
    if (available == 0) return SALTS_OK;
    if (!ReadFile(pipe, buffer, available < sizeof(buffer) ? available : sizeof(buffer),
                  &read_count, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        *open = 0;
        return SALTS_OK;
      }
      return -(int)error;
    }
    {
      int rc = process_append_output(process, is_stdout, buffer, read_count);
      if (rc != SALTS_OK) return rc;
    }
  }
  return SALTS_OK;
}

static void process_monitor(void *arg) {
  cmeta_process_t *process = (cmeta_process_t *)arg;
  uint64_t started = cmeta_monotonic_ms();
  cmeta_process_state_t terminal = SALTS_PROCESS_EXITED;
  int error_code = SALTS_OK;
  DWORD exit_code = 1;

  for (;;) {
    int out_rc = process->stdout_open
                     ? drain_windows_pipe(process, process->stdout_read, 1, &process->stdout_open)
                     : SALTS_OK;
    int err_rc = process->stderr_open
                     ? drain_windows_pipe(process, process->stderr_read, 0, &process->stderr_open)
                     : SALTS_OK;
    DWORD wait_result;
    if (out_rc == SALTS_ERANGE || err_rc == SALTS_ERANGE) {
      terminal = SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED;
      process_platform_terminate(process);
    } else if (out_rc != SALTS_OK || err_rc != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = out_rc != SALTS_OK ? out_rc : err_rc;
      process_platform_terminate(process);
    }

    wait_result = WaitForSingleObject(process->process_handle, SALTS_PROCESS_POLL_INTERVAL_MS);
    if (wait_result == WAIT_OBJECT_0) {
      if (process->shell_command && !TerminateJobObject(process->job_handle, 1u)) {
        terminal = SALTS_PROCESS_WAIT_FAILED;
        error_code = win32_error();
      }
      cmeta_mutex_lock(&process->mutex);
      process->native_active = 0;
      cmeta_mutex_unlock(&process->mutex);
      break;
    }
    if (wait_result != WAIT_TIMEOUT) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = win32_error();
      process_platform_terminate(process);
      WaitForSingleObject(process->process_handle, INFINITE);
      cmeta_mutex_lock(&process->mutex);
      process->native_active = 0;
      cmeta_mutex_unlock(&process->mutex);
      break;
    }
    if (terminal != SALTS_PROCESS_EXITED) continue;
    cmeta_mutex_lock(&process->mutex);
    if (process->terminate_requested) terminal = SALTS_PROCESS_TERMINATED;
    cmeta_mutex_unlock(&process->mutex);
    if (terminal == SALTS_PROCESS_EXITED && process->timeout_ms != 0 &&
        cmeta_monotonic_ms() - started >= process->timeout_ms) {
      terminal = SALTS_PROCESS_TIMED_OUT;
      process_platform_terminate(process);
    }
  }

  if (process->stdout_open) {
    int final_rc = drain_windows_pipe(process, process->stdout_read, 1, &process->stdout_open);
    if (terminal == SALTS_PROCESS_EXITED && final_rc == SALTS_ERANGE)
      terminal = SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED;
    else if (terminal == SALTS_PROCESS_EXITED && final_rc != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = final_rc;
    }
  }
  if (process->stderr_open) {
    int final_rc = drain_windows_pipe(process, process->stderr_read, 0, &process->stderr_open);
    if (terminal == SALTS_PROCESS_EXITED && final_rc == SALTS_ERANGE)
      terminal = SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED;
    else if (terminal == SALTS_PROCESS_EXITED && final_rc != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = final_rc;
    }
  }
  close_win_handle(&process->stdout_read);
  close_win_handle(&process->stderr_read);
  if (!GetExitCodeProcess(process->process_handle, &exit_code) &&
      terminal == SALTS_PROCESS_EXITED) {
    terminal = SALTS_PROCESS_WAIT_FAILED;
    error_code = win32_error();
  }
  if (terminal == SALTS_PROCESS_EXITED) {
    cmeta_mutex_lock(&process->mutex);
    if (process->terminate_requested) terminal = SALTS_PROCESS_TERMINATED;
    cmeta_mutex_unlock(&process->mutex);
  }
  process_finish(process, terminal, terminal == SALTS_PROCESS_EXITED ? (int)exit_code : -1, 0,
                 error_code);
}

static int process_platform_spawn(cmeta_process_t *process, const cmeta_process_options_t *options,
                                  const cmeta_process_stdio_bindings_t *bindings) {
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  STARTUPINFOEXW startup;
  PROCESS_INFORMATION info;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  HANDLE stdin_read = INVALID_HANDLE_VALUE;
  HANDLE stdout_write = INVALID_HANDLE_VALUE;
  HANDLE stderr_write = INVALID_HANDLE_VALUE;
  HANDLE inherited_handles[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
  SIZE_T attribute_size = 0;
  wchar_t *command = NULL;
  wchar_t *cwd = NULL;
  wchar_t *environment = NULL;
  DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                EXTENDED_STARTUPINFO_PRESENT;
  int rc;

  memset(&startup, 0, sizeof(startup));
  memset(&info, 0, sizeof(info));
  memset(&limits, 0, sizeof(limits));
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

  rc = build_windows_command(options, &command);
  if (rc == SALTS_OK) rc = utf8_to_utf16(options->cwd, &cwd);
  if (rc == SALTS_OK)
    rc = build_windows_environment(
        options->env, (options->flags & SALTS_PROCESS_CLEAN_ENVIRONMENT) != 0U, &environment);
  if (rc != SALTS_OK) goto cleanup;

  if (bindings && bindings->stdin_handle != SALTS_PROCESS_STDIO_INHERIT) {
    rc = duplicate_bound_handle(bindings->stdin_handle, &inherited_handles[0]);
    if (rc != SALTS_OK) goto cleanup;
  } else if ((options->flags & SALTS_PROCESS_PIPE_STDIN) != 0U) {
    if (!CreatePipe(&stdin_read, &process->stdin_write, &security, 0) ||
        !SetHandleInformation(process->stdin_write, HANDLE_FLAG_INHERIT, 0)) {
      rc = win32_error();
      goto cleanup;
    }
    inherited_handles[0] = stdin_read;
  } else {
    rc = duplicate_standard_handle(STD_INPUT_HANDLE, GENERIC_READ, &inherited_handles[0]);
    if (rc != SALTS_OK) goto cleanup;
  }
  if (bindings && bindings->stdout_handle != SALTS_PROCESS_STDIO_INHERIT) {
    rc = duplicate_bound_handle(bindings->stdout_handle, &inherited_handles[1]);
    if (rc != SALTS_OK) goto cleanup;
  } else if ((options->flags & SALTS_PROCESS_CAPTURE_STDOUT) != 0U) {
    if (!CreatePipe(&process->stdout_read, &stdout_write, &security, 0) ||
        !SetHandleInformation(process->stdout_read, HANDLE_FLAG_INHERIT, 0)) {
      rc = win32_error();
      goto cleanup;
    }
    inherited_handles[1] = stdout_write;
    process->stdout_open = 1;
  } else {
    rc = duplicate_standard_handle(STD_OUTPUT_HANDLE, GENERIC_WRITE, &inherited_handles[1]);
    if (rc != SALTS_OK) goto cleanup;
  }
  if (bindings && bindings->stderr_handle != SALTS_PROCESS_STDIO_INHERIT) {
    rc = duplicate_bound_handle(bindings->stderr_handle, &inherited_handles[2]);
    if (rc != SALTS_OK) goto cleanup;
  } else if ((options->flags & SALTS_PROCESS_CAPTURE_STDERR) != 0U) {
    if (!CreatePipe(&process->stderr_read, &stderr_write, &security, 0) ||
        !SetHandleInformation(process->stderr_read, HANDLE_FLAG_INHERIT, 0)) {
      rc = win32_error();
      goto cleanup;
    }
    inherited_handles[2] = stderr_write;
    process->stderr_open = 1;
  } else {
    rc = duplicate_standard_handle(STD_ERROR_HANDLE, GENERIC_WRITE, &inherited_handles[2]);
    if (rc != SALTS_OK) goto cleanup;
  }

  startup.StartupInfo.hStdInput = inherited_handles[0];
  startup.StartupInfo.hStdOutput = inherited_handles[1];
  startup.StartupInfo.hStdError = inherited_handles[2];
  InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
  startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
  if (!startup.lpAttributeList) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) ||
      !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherited_handles, sizeof(inherited_handles), NULL, NULL)) {
    rc = win32_error();
    goto cleanup;
  }

  if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, flags, environment, cwd,
                      &startup.StartupInfo, &info)) {
    rc = win32_error();
    goto cleanup;
  }
  process->process_handle = info.hProcess;
  process->job_handle = CreateJobObjectW(NULL, NULL);
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!process->job_handle ||
      !SetInformationJobObject(process->job_handle, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)) ||
      !AssignProcessToJobObject(process->job_handle, process->process_handle)) {
    rc = win32_error();
    TerminateProcess(process->process_handle, 1);
    WaitForSingleObject(process->process_handle, INFINITE);
    goto cleanup;
  }
  if (ResumeThread(info.hThread) == (DWORD)-1) {
    rc = win32_error();
    TerminateJobObject(process->job_handle, 1);
    WaitForSingleObject(process->process_handle, INFINITE);
    goto cleanup;
  }

  process->result.pid = (int)info.dwProcessId;
  process->native_active = 1;
  process->state = SALTS_PROCESS_RUNNING;
  process->result.state = SALTS_PROCESS_RUNNING;
  rc = SALTS_OK;

cleanup:
  if (startup.lpAttributeList) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
  }
  if (inherited_handles[0] != stdin_read) close_win_handle(&inherited_handles[0]);
  if (inherited_handles[1] != stdout_write) close_win_handle(&inherited_handles[1]);
  if (inherited_handles[2] != stderr_write) close_win_handle(&inherited_handles[2]);
  close_win_handle(&stdin_read);
  close_win_handle(&stdout_write);
  close_win_handle(&stderr_write);
  if (info.hThread) CloseHandle(info.hThread);
  free(command);
  free(cwd);
  free(environment);
  return rc;
}

int cmeta_process_write_stdin(cmeta_process_t *process, const void *data, size_t size,
                              size_t *out_written) {
  const char *cursor = (const char *)data;
  size_t total = 0;
  int rc = SALTS_OK;
  if (!process || (size > 0 && !data)) return SALTS_EINVAL;
  if (out_written) *out_written = 0;
  cmeta_mutex_lock(&process->stdin_mutex);
  if (process->stdin_write == INVALID_HANDLE_VALUE) {
    cmeta_mutex_unlock(&process->stdin_mutex);
    return SALTS_EPIPE;
  }
  while (total < size) {
    DWORD chunk = (DWORD)((size - total) > UINT32_MAX ? UINT32_MAX : (size - total));
    DWORD written = 0;
    if (!WriteFile(process->stdin_write, cursor + total, chunk, &written, NULL)) {
      DWORD error = GetLastError();
      rc = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
               ? SALTS_EPIPE
               : win32_error_from_code(error);
      break;
    }
    if (written == 0) {
      rc = SALTS_EPIPE;
      break;
    }
    total += written;
  }
  if (out_written) *out_written = total;
  cmeta_mutex_unlock(&process->stdin_mutex);
  return rc;
}

int cmeta_process_close_stdin(cmeta_process_t *process) {
  if (!process) return SALTS_EINVAL;
  cmeta_mutex_lock(&process->stdin_mutex);
  close_win_handle(&process->stdin_write);
  cmeta_mutex_unlock(&process->stdin_mutex);
  return SALTS_OK;
}

int cmeta_process_is_pid_alive(int pid, bool *out_alive) {
  HANDLE handle;
  DWORD wait_result;
  DWORD wait_error = ERROR_SUCCESS;
  if (pid <= 0 || !out_alive) return SALTS_EINVAL;
  handle = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
  if (!handle) {
    DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
      *out_alive = false;
      return SALTS_OK;
    }
    return -(int)error;
  }
  wait_result = WaitForSingleObject(handle, 0);
  if (wait_result == WAIT_FAILED) wait_error = GetLastError();
  CloseHandle(handle);
  *out_alive = wait_result == WAIT_TIMEOUT;
  return wait_result == WAIT_FAILED ? -(int)wait_error : SALTS_OK;
}

#else

static void close_fd(int *fd) {
  if (*fd >= 0) close(*fd);
  *fd = -1;
}

static int create_pipe(int fds[2]) {
  fds[0] = -1;
  fds[1] = -1;
  return pipe(fds) == 0 ? SALTS_OK : -errno;
}

static void close_pipe(int fds[2]) {
  close_fd(&fds[0]);
  close_fd(&fds[1]);
}

static const char *environment_path(const char *const *env, int clean_environment) {
  size_t i;
  if (env) {
    for (i = 0; env[i]; ++i) {
      if (strncmp(env[i], "PATH=", 5) == 0) return env[i] + 5;
    }
  }
  return clean_environment ? NULL : getenv("PATH");
}

static char **build_argv(const cmeta_process_options_t *options) {
  size_t count = 0;
  char **argv;
  if (options->args) {
    while (options->args[count]) {
      if (count > (SIZE_MAX / sizeof(*argv)) - 2U) return NULL;
      ++count;
    }
  }
  argv = (char **)calloc(count + 2, sizeof(*argv));
  if (!argv) return NULL;
  argv[0] = (char *)options->program;
  for (size_t i = 0; i < count; ++i)
    argv[i + 1] = (char *)options->args[i];
  return argv;
}

static size_t posix_env_key_length(const char *entry) {
  const char *equals = strchr(entry, '=');
  return equals ? (size_t)(equals - entry) : 0;
}

static int posix_env_key_equal(const char *left, const char *right) {
  size_t left_length = posix_env_key_length(left);
  size_t right_length = posix_env_key_length(right);
  return left_length != 0 && left_length == right_length && memcmp(left, right, left_length) == 0;
}

static int build_posix_environment(const char *const *overrides, int clean_environment,
                                   char ***out_environment) {
  size_t parent_count = 0;
  size_t override_count = 0;
  size_t entry_count = 0;
  char **environment;
  size_t i;

  *out_environment = NULL;
  if (!overrides && !clean_environment) return SALTS_OK;
  if (!clean_environment) {
    while (environ[parent_count])
      ++parent_count;
  }
  if (overrides) {
    while (overrides[override_count]) {
      if (posix_env_key_length(overrides[override_count]) == 0) return SALTS_EINVAL;
      for (size_t j = 0; j < override_count; ++j) {
        if (posix_env_key_equal(overrides[override_count], overrides[j])) return SALTS_EINVAL;
      }
      if (override_count == SIZE_MAX) return SALTS_ENOMEM;
      ++override_count;
    }
  }
  if (override_count > SIZE_MAX - parent_count - 1U ||
      parent_count + override_count + 1U > SIZE_MAX / sizeof(*environment))
    return SALTS_ENOMEM;
  environment = (char **)calloc(parent_count + override_count + 1, sizeof(*environment));
  if (!environment) return SALTS_ENOMEM;
  if (!clean_environment) {
    for (i = 0; i < parent_count; ++i) {
      int overridden = 0;
      for (size_t j = 0; j < override_count; ++j) {
        if (posix_env_key_equal(environ[i], overrides[j])) {
          overridden = 1;
          break;
        }
      }
      if (!overridden) environment[entry_count++] = environ[i];
    }
  }
  for (i = 0; i < override_count; ++i)
    environment[entry_count++] = (char *)overrides[i];
  *out_environment = environment;
  return SALTS_OK;
}

static char **build_program_candidates(const cmeta_process_options_t *options, size_t *out_count) {
  const char *path;
  const char *cursor;
  size_t count = 1;
  size_t index = 0;
  char **candidates;
  if (strchr(options->program, '/')) {
    candidates = (char **)calloc(1, sizeof(*candidates));
    if (!candidates) return NULL;
    candidates[0] = strdup(options->program);
    if (!candidates[0]) {
      free(candidates);
      return NULL;
    }
    *out_count = 1;
    return candidates;
  }
  path = environment_path(options->env,
                          (options->flags & SALTS_PROCESS_CLEAN_ENVIRONMENT) != 0U);
  if (!path || path[0] == '\0') path = "/usr/local/bin:/usr/bin:/bin";
  for (cursor = path; *cursor; ++cursor)
    if (*cursor == ':') ++count;
  candidates = (char **)calloc(count, sizeof(*candidates));
  if (!candidates) return NULL;
  cursor = path;
  while (index < count) {
    const char *end = strchr(cursor, ':');
    size_t dir_length = end ? (size_t)(end - cursor) : strlen(cursor);
    size_t program_length = strlen(options->program);
    size_t prefix_length = dir_length ? dir_length : 1U;
    size_t length;
    if (prefix_length > SIZE_MAX - program_length - 2U) goto error;
    length = prefix_length + program_length + 2U;
    candidates[index] = (char *)malloc(length);
    if (!candidates[index]) goto error;
    if (dir_length == 0) {
      candidates[index][0] = '.';
      candidates[index][1] = '/';
      memcpy(candidates[index] + 2, options->program, program_length + 1U);
    } else {
      memcpy(candidates[index], cursor, dir_length);
      candidates[index][dir_length] = '/';
      memcpy(candidates[index] + dir_length + 1U, options->program, program_length + 1U);
    }
    ++index;
    if (!end) break;
    cursor = end + 1;
  }
  *out_count = index;
  return candidates;

error:
  while (index > 0)
    free(candidates[--index]);
  free(candidates);
  return NULL;
}

static void free_candidates(char **candidates, size_t count) {
  for (size_t i = 0; i < count; ++i)
    free(candidates[i]);
  free(candidates);
}

static ssize_t write_without_sigpipe(int fd, const void *data, size_t size) {
  sigset_t blocked;
  sigset_t previous;
  sigset_t pending;
  int had_pending = 0;
  ssize_t result;

  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
    errno = EIO;
    return -1;
  }
  if (sigpending(&pending) == 0) had_pending = sigismember(&pending, SIGPIPE);
  result = write(fd, data, size);
  if (result < 0 && errno == EPIPE && !had_pending) {
    int signal_number;
    int wait_rc;
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
      do {
        wait_rc = sigwait(&blocked, &signal_number);
      } while (wait_rc == EINTR);
    }
    errno = EPIPE;
  }
  pthread_sigmask(SIG_SETMASK, &previous, NULL);
  return result;
}

static void child_exec(char **candidates, size_t candidate_count, char *const argv[],
                       char *const envp[], int error_fd) {
  int last_error = ENOENT;
  for (size_t i = 0; i < candidate_count; ++i) {
    execve(candidates[i], argv, envp);
    if (errno != ENOENT && errno != ENOTDIR) last_error = errno;
  }
  (void)write(error_fd, &last_error, sizeof(last_error));
  _exit(127);
}

static int duplicate_child_stdio(int source) {
  int duplicated;
  do {
    duplicated = fcntl(source, F_DUPFD, STDERR_FILENO + 1);
  } while (duplicated < 0 && errno == EINTR);
  return duplicated;
}

static int process_platform_terminate(cmeta_process_t *process) {
  pid_t pid;
  int rc;
  cmeta_mutex_lock(&process->mutex);
  if (process_state_terminal(process->state) || !process->native_active) {
    cmeta_mutex_unlock(&process->mutex);
    return SALTS_OK;
  }
  process->terminate_requested = 1;
  pid = process->pid;
  if (kill(-pid, SIGKILL) == 0 || errno == ESRCH) {
    cmeta_mutex_unlock(&process->mutex);
    return SALTS_OK;
  }
  rc = kill(pid, SIGKILL) == 0 || errno == ESRCH ? SALTS_OK : -errno;
  cmeta_mutex_unlock(&process->mutex);
  return rc;
}

static int drain_posix_fd(cmeta_process_t *process, int fd, int is_stdout, atomic_int *open) {
  char buffer[4096];
  while (*open) {
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      int rc = process_append_output(process, is_stdout, buffer, (size_t)count);
      if (rc != SALTS_OK) return rc;
      continue;
    }
    if (count == 0) {
      *open = 0;
      return SALTS_OK;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SALTS_OK;
    return -errno;
  }
  return SALTS_OK;
}

static int process_cleanup_shell_group(pid_t pid) {
  int error;
  if (kill(-pid, SIGKILL) == 0) return SALTS_OK;
  error = errno;
#ifdef __APPLE__
  if (error == EPERM) {
    int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, pid};
    struct kinfo_proc remaining;
    size_t size = sizeof(remaining);
    /* Darwin skips zombies when signalling a group and reports EPERM when
     * none remain signalable. Only accept the known unreaped root (or an
     * empty group); additional members or a failed query retain the error. */
    if (sysctl(query, sizeof(query) / sizeof(query[0]), &remaining, &size, NULL, 0) == 0 &&
        (size == 0u || (size == sizeof(remaining) && remaining.kp_proc.p_pid == pid &&
                       remaining.kp_proc.p_stat == SZOMB)))
      return SALTS_OK;
  }
#endif
  return error == ESRCH ? SALTS_OK : -error;
}

static pid_t process_poll_posix(cmeta_process_t *process, int *status, int *tree_error) {
  if (process->shell_command) {
    siginfo_t info;
    int rc;
    memset(&info, 0, sizeof(info));
    do {
      rc = waitid(P_PID, (id_t)process->pid, &info, WEXITED | WNOHANG | WNOWAIT);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) return -1;
    if (info.si_pid == 0) return 0;
    /* Keep the root waitable until group cleanup, preventing PID/PGID reuse. */
    *tree_error = process_cleanup_shell_group(process->pid);
  }
  return waitpid(process->pid, status, WNOHANG);
}

static void process_monitor(void *arg) {
  cmeta_process_t *process = (cmeta_process_t *)arg;
  uint64_t started = cmeta_monotonic_ms();
  cmeta_process_state_t terminal = SALTS_PROCESS_EXITED;
  int error_code = SALTS_OK;
  int status = 0;

  for (;;) {
    int out_rc = process->stdout_open
                     ? drain_posix_fd(process, process->stdout_fd, 1, &process->stdout_open)
                     : SALTS_OK;
    int err_rc = process->stderr_open
                     ? drain_posix_fd(process, process->stderr_fd, 0, &process->stderr_open)
                     : SALTS_OK;
    pid_t waited;
    int tree_error = SALTS_OK;
    if (out_rc == SALTS_ERANGE || err_rc == SALTS_ERANGE) {
      terminal = SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED;
      process_platform_terminate(process);
    } else if (out_rc != SALTS_OK || err_rc != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = out_rc != SALTS_OK ? out_rc : err_rc;
      process_platform_terminate(process);
    }

    cmeta_mutex_lock(&process->mutex);
    do {
      waited = process_poll_posix(process, &status, &tree_error);
    } while (waited < 0 && errno == EINTR);
    if (waited == process->pid || waited < 0) process->native_active = 0;
    cmeta_mutex_unlock(&process->mutex);
    if (tree_error != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = tree_error;
    }
    if (waited == process->pid) break;
    if (waited < 0) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = -errno;
      process_platform_terminate(process);
      do {
        waited = waitpid(process->pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
      break;
    }
    if (terminal == SALTS_PROCESS_EXITED) {
      cmeta_mutex_lock(&process->mutex);
      if (process->terminate_requested) terminal = SALTS_PROCESS_TERMINATED;
      cmeta_mutex_unlock(&process->mutex);
      if (terminal == SALTS_PROCESS_EXITED && process->timeout_ms != 0 &&
          cmeta_monotonic_ms() - started >= process->timeout_ms) {
        terminal = SALTS_PROCESS_TIMED_OUT;
        process_platform_terminate(process);
      }
    }
    cmeta_sleep_ms(SALTS_PROCESS_POLL_INTERVAL_MS);
  }

  if (process->stdout_open) {
    int final_rc = drain_posix_fd(process, process->stdout_fd, 1, &process->stdout_open);
    if (terminal == SALTS_PROCESS_EXITED && final_rc == SALTS_ERANGE)
      terminal = SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED;
    else if (terminal == SALTS_PROCESS_EXITED && final_rc != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = final_rc;
    }
  }
  if (process->stderr_open) {
    int final_rc = drain_posix_fd(process, process->stderr_fd, 0, &process->stderr_open);
    if (terminal == SALTS_PROCESS_EXITED && final_rc == SALTS_ERANGE)
      terminal = SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED;
    else if (terminal == SALTS_PROCESS_EXITED && final_rc != SALTS_OK) {
      terminal = SALTS_PROCESS_WAIT_FAILED;
      error_code = final_rc;
    }
  }
  close_fd(&process->stdout_fd);
  close_fd(&process->stderr_fd);
  if (terminal == SALTS_PROCESS_EXITED) {
    cmeta_mutex_lock(&process->mutex);
    if (process->terminate_requested) terminal = SALTS_PROCESS_TERMINATED;
    cmeta_mutex_unlock(&process->mutex);
  }
  if (terminal == SALTS_PROCESS_EXITED && WIFSIGNALED(status)) terminal = SALTS_PROCESS_SIGNALED;
  process_finish(process, terminal,
                 terminal == SALTS_PROCESS_EXITED && WIFEXITED(status) ? WEXITSTATUS(status)
                 : terminal == SALTS_PROCESS_SIGNALED                  ? 128 + WTERMSIG(status)
                                                                       : -1,
                 WIFSIGNALED(status) ? WTERMSIG(status) : 0, error_code);
}

static int process_platform_spawn(cmeta_process_t *process, const cmeta_process_options_t *options,
                                  const cmeta_process_stdio_bindings_t *bindings) {
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int exec_pipe[2] = {-1, -1};
  char **argv = NULL;
  char **environment = NULL;
  char **candidates = NULL;
  size_t candidate_count = 0;
  pid_t pid;
  int child_error = 0;
  ssize_t error_size;
  int rc = SALTS_OK;
  int bound_stdin = -1;
  int bound_stdout = -1;
  int bound_stderr = -1;

  if (bindings != NULL) {
    if ((bindings->stdin_handle != SALTS_PROCESS_STDIO_INHERIT &&
         bindings->stdin_handle > INT_MAX) ||
        (bindings->stdout_handle != SALTS_PROCESS_STDIO_INHERIT &&
         bindings->stdout_handle > INT_MAX) ||
        (bindings->stderr_handle != SALTS_PROCESS_STDIO_INHERIT &&
         bindings->stderr_handle > INT_MAX))
      return SALTS_EINVAL;
    if (bindings->stdin_handle != SALTS_PROCESS_STDIO_INHERIT)
      bound_stdin = (int)bindings->stdin_handle;
    if (bindings->stdout_handle != SALTS_PROCESS_STDIO_INHERIT)
      bound_stdout = (int)bindings->stdout_handle;
    if (bindings->stderr_handle != SALTS_PROCESS_STDIO_INHERIT)
      bound_stderr = (int)bindings->stderr_handle;
  }

  if ((options->flags & SALTS_PROCESS_PIPE_STDIN) != 0U &&
      (rc = create_pipe(stdin_pipe)) != SALTS_OK)
    goto cleanup;
  if ((options->flags & SALTS_PROCESS_CAPTURE_STDOUT) != 0U &&
      (rc = create_pipe(stdout_pipe)) != SALTS_OK)
    goto cleanup;
  if ((options->flags & SALTS_PROCESS_CAPTURE_STDERR) != 0U &&
      (rc = create_pipe(stderr_pipe)) != SALTS_OK)
    goto cleanup;
  if ((rc = create_pipe(exec_pipe)) != SALTS_OK) goto cleanup;
  if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
    rc = -errno;
    goto cleanup;
  }
  argv = build_argv(options);
  rc = build_posix_environment(
      options->env, (options->flags & SALTS_PROCESS_CLEAN_ENVIRONMENT) != 0U, &environment);
  if (rc != SALTS_OK) goto cleanup;
  candidates = build_program_candidates(options, &candidate_count);
  if (!argv || !candidates) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }

  pid = fork();
  if (pid < 0) {
    rc = -errno;
    goto cleanup;
  }
  if (pid == 0) {
    int child_stdin = bound_stdin >= 0 ? duplicate_child_stdio(bound_stdin) : -1;
    int child_stdout = bound_stdout >= 0 ? duplicate_child_stdio(bound_stdout) : -1;
    int child_stderr = bound_stderr >= 0 ? duplicate_child_stdio(bound_stderr) : -1;
    close_fd(&exec_pipe[0]);
    if ((bound_stdin >= 0 && child_stdin < 0) ||
        (bound_stdout >= 0 && child_stdout < 0) ||
        (bound_stderr >= 0 && child_stderr < 0) || setpgid(0, 0) != 0 ||
        ((bound_stdin >= 0 || (options->flags & SALTS_PROCESS_PIPE_STDIN) != 0U) &&
         dup2(bound_stdin >= 0 ? child_stdin : stdin_pipe[0], STDIN_FILENO) < 0) ||
        ((bound_stdout >= 0 || (options->flags & SALTS_PROCESS_CAPTURE_STDOUT) != 0U) &&
         dup2(bound_stdout >= 0 ? child_stdout : stdout_pipe[1], STDOUT_FILENO) < 0) ||
        ((bound_stderr >= 0 || (options->flags & SALTS_PROCESS_CAPTURE_STDERR) != 0U) &&
         dup2(bound_stderr >= 0 ? child_stderr : stderr_pipe[1], STDERR_FILENO) < 0) ||
        (options->cwd && chdir(options->cwd) != 0)) {
      child_error = errno;
      (void)write(exec_pipe[1], &child_error, sizeof(child_error));
      _exit(127);
    }
    if (child_stdin >= 0) close(child_stdin);
    if (child_stdout >= 0) close(child_stdout);
    if (child_stderr >= 0) close(child_stderr);
    close_pipe(stdin_pipe);
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    if (bound_stdin > STDERR_FILENO) close(bound_stdin);
    if (bound_stdout > STDERR_FILENO && bound_stdout != bound_stdin) close(bound_stdout);
    if (bound_stderr > STDERR_FILENO && bound_stderr != bound_stdin && bound_stderr != bound_stdout)
      close(bound_stderr);
    child_exec(candidates, candidate_count, argv, environment ? environment : environ,
               exec_pipe[1]);
  }

  process->pid = pid;
  setpgid(pid, pid);
  close_fd(&exec_pipe[1]);
  close_fd(&stdin_pipe[0]);
  close_fd(&stdout_pipe[1]);
  close_fd(&stderr_pipe[1]);
  do {
    error_size = read(exec_pipe[0], &child_error, sizeof(child_error));
  } while (error_size < 0 && errno == EINTR);
  close_fd(&exec_pipe[0]);
  if (error_size > 0) {
    int status;
    do {
      rc = waitpid(pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    rc = -child_error;
    goto cleanup;
  }
  if (error_size < 0) {
    rc = -errno;
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
    goto cleanup;
  }

  process->stdin_fd = stdin_pipe[1];
  stdin_pipe[1] = -1;
  process->stdout_fd = stdout_pipe[0];
  stdout_pipe[0] = -1;
  process->stderr_fd = stderr_pipe[0];
  stderr_pipe[0] = -1;
  if (process->stdout_fd >= 0) {
    fcntl(process->stdout_fd, F_SETFL, fcntl(process->stdout_fd, F_GETFL, 0) | O_NONBLOCK);
    process->stdout_open = 1;
  }
  if (process->stderr_fd >= 0) {
    fcntl(process->stderr_fd, F_SETFL, fcntl(process->stderr_fd, F_GETFL, 0) | O_NONBLOCK);
    process->stderr_open = 1;
  }
  process->result.pid = (int)pid;
  process->native_active = 1;
  process->state = SALTS_PROCESS_RUNNING;
  process->result.state = SALTS_PROCESS_RUNNING;
  rc = SALTS_OK;

cleanup:
  close_pipe(stdin_pipe);
  close_pipe(stdout_pipe);
  close_pipe(stderr_pipe);
  close_pipe(exec_pipe);
  free(argv);
  free(environment);
  free_candidates(candidates, candidate_count);
  return rc;
}

int cmeta_process_write_stdin(cmeta_process_t *process, const void *data, size_t size,
                              size_t *out_written) {
  const char *cursor = (const char *)data;
  size_t total = 0;
  int rc = SALTS_OK;
  if (!process || (size > 0 && !data)) return SALTS_EINVAL;
  if (out_written) *out_written = 0;
  cmeta_mutex_lock(&process->stdin_mutex);
  if (process->stdin_fd < 0) {
    cmeta_mutex_unlock(&process->stdin_mutex);
    return SALTS_EPIPE;
  }
  while (total < size) {
    ssize_t written = write_without_sigpipe(process->stdin_fd, cursor + total, size - total);
    if (written > 0) {
      total += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    rc = written < 0 && errno != EPIPE ? -errno : SALTS_EPIPE;
    break;
  }
  if (out_written) *out_written = total;
  cmeta_mutex_unlock(&process->stdin_mutex);
  return rc;
}

int cmeta_process_close_stdin(cmeta_process_t *process) {
  if (!process) return SALTS_EINVAL;
  cmeta_mutex_lock(&process->stdin_mutex);
  close_fd(&process->stdin_fd);
  cmeta_mutex_unlock(&process->stdin_mutex);
  return SALTS_OK;
}

int cmeta_process_is_pid_alive(int pid, bool *out_alive) {
  if (pid <= 0 || !out_alive) return SALTS_EINVAL;
  if (kill((pid_t)pid, 0) == 0 || errno == EPERM) {
    *out_alive = true;
    return SALTS_OK;
  }
  if (errno == ESRCH) {
    *out_alive = false;
    return SALTS_OK;
  }
  return -errno;
}

#endif

static int process_spawn(const cmeta_process_options_t *options,
                         const cmeta_process_stdio_bindings_t *bindings,
                         cmeta_process_t **out_process) {
  cmeta_process_t *process;
#ifndef _WIN32
  cmeta_process_options_t shell_options;
  const char *shell_args[] = {"-c", NULL, NULL};
#endif
  int rc;
  if (!out_process) return SALTS_EINVAL;
  *out_process = NULL;
  rc = process_validate_options(options);
  if (rc != SALTS_OK) return rc;
  if (bindings) {
    rc = process_validate_stdio_conflicts(options, bindings);
    if (rc != SALTS_OK) return rc;
  }
#ifndef _WIN32
  if ((options->flags & SALTS_PROCESS_SHELL_COMMAND) != 0U) {
    shell_options = *options;
    shell_args[1] = options->program;
    shell_options.program = "/bin/sh";
    shell_options.args = shell_args;
    options = &shell_options;
  }
#endif
  process = (cmeta_process_t *)calloc(1, sizeof(*process));
  if (!process) return SALTS_ENOMEM;
  rc = process_init(process, options);
  if (rc != SALTS_OK) goto error;
  rc = process_platform_spawn(process, options, bindings);
  if (rc != SALTS_OK) goto error;
  rc = cmeta_thread_create(&process->monitor_thread, process_monitor, process);
  if (rc != SALTS_OK) {
    process_platform_terminate(process);
#ifdef _WIN32
    WaitForSingleObject(process->process_handle, INFINITE);
#else
    waitpid(process->pid, NULL, 0);
#endif
    goto error;
  }
  process->monitor_started = 1;
  *out_process = process;
  return SALTS_OK;

error:
  cmeta_process_close_stdin(process);
#ifdef _WIN32
  close_win_handle(&process->stdout_read);
  close_win_handle(&process->stderr_read);
  close_win_handle(&process->job_handle);
  close_win_handle(&process->process_handle);
#else
  close_fd(&process->stdout_fd);
  close_fd(&process->stderr_fd);
#endif
  tstr_free(process->stdout_data);
  tstr_free(process->stderr_data);
  cmeta_mutex_destroy(&process->stdin_mutex);
  cmeta_cond_destroy(&process->changed);
  cmeta_mutex_destroy(&process->mutex);
  free(process);
  return rc;
}

int cmeta_process_spawn(const cmeta_process_options_t *options, cmeta_process_t **out_process) {
  return process_spawn(options, NULL, out_process);
}

int cmeta_process_spawn_with_stdio(const cmeta_process_options_t *options,
                                   const cmeta_process_stdio_bindings_t *bindings,
                                   cmeta_process_t **out_process) {
  if (!bindings) {
    if (out_process) *out_process = NULL;
    return SALTS_EINVAL;
  }
  return process_spawn(options, bindings, out_process);
}

int cmeta_process_terminate(cmeta_process_t *process) {
  if (!process) return SALTS_EINVAL;
  return process_platform_terminate(process);
}

void cmeta_process_destroy(cmeta_process_t *process) {
  if (!process) return;
  cmeta_process_terminate(process);
  cmeta_process_close_stdin(process);
  process_join_monitor(process);
#ifdef _WIN32
  close_win_handle(&process->job_handle);
  close_win_handle(&process->process_handle);
#else
  close_fd(&process->stdout_fd);
  close_fd(&process->stderr_fd);
#endif
  tstr_free(process->stdout_data);
  tstr_free(process->stderr_data);
  cmeta_mutex_destroy(&process->stdin_mutex);
  cmeta_cond_destroy(&process->changed);
  cmeta_mutex_destroy(&process->mutex);
  free(process);
}
