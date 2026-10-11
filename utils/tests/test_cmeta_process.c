#include "tinytest.h"
#include "cmeta_error.h"
#include "cmeta_fs.h"
#include "cmeta_process.h"
#include "cmeta_thread.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #define TEST_SHELL "cmd.exe"
#else
  #include <errno.h>
  #include <unistd.h>
  #define TEST_SHELL "/bin/sh"
#endif

typedef struct test_process_pipe {
#ifdef _WIN32
  HANDLE read_handle;
  HANDLE write_handle;
#else
  int read_handle;
  int write_handle;
#endif
} test_process_pipe;

static int test_process_pipe_init(test_process_pipe *channel) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  channel->read_handle = INVALID_HANDLE_VALUE;
  channel->write_handle = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&channel->read_handle, &channel->write_handle, &security, 0)) return -1;
  if (!SetHandleInformation(channel->read_handle, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(channel->read_handle);
    CloseHandle(channel->write_handle);
    channel->read_handle = INVALID_HANDLE_VALUE;
    channel->write_handle = INVALID_HANDLE_VALUE;
    return -1;
  }
#else
  int handles[2];
  channel->read_handle = -1;
  channel->write_handle = -1;
  if (pipe(handles) != 0) return -1;
  channel->read_handle = handles[0];
  channel->write_handle = handles[1];
#endif
  return 0;
}

static uintptr_t test_process_pipe_write_handle(const test_process_pipe *pipe) {
  return (uintptr_t)pipe->write_handle;
}

static void test_process_pipe_close_write(test_process_pipe *pipe) {
#ifdef _WIN32
  if (pipe->write_handle != INVALID_HANDLE_VALUE) CloseHandle(pipe->write_handle);
  pipe->write_handle = INVALID_HANDLE_VALUE;
#else
  if (pipe->write_handle >= 0) close(pipe->write_handle);
  pipe->write_handle = -1;
#endif
}

static size_t test_process_pipe_read_all(test_process_pipe *pipe, char *buffer, size_t capacity) {
  size_t total = 0;
  while (total < capacity) {
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(pipe->read_handle, buffer + total, (DWORD)(capacity - total), &count, NULL))
      break;
    if (count == 0) break;
    total += (size_t)count;
#else
    ssize_t count = read(pipe->read_handle, buffer + total, capacity - total);
    if (count > 0) {
      total += (size_t)count;
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
#endif
  }
  return total;
}

static void test_process_pipe_destroy(test_process_pipe *pipe) {
  test_process_pipe_close_write(pipe);
#ifdef _WIN32
  if (pipe->read_handle != INVALID_HANDLE_VALUE) CloseHandle(pipe->read_handle);
  pipe->read_handle = INVALID_HANDLE_VALUE;
#else
  if (pipe->read_handle >= 0) close(pipe->read_handle);
  pipe->read_handle = -1;
#endif
}

static void init_shell_options(cmeta_process_options_t *options, const char *command) {
#ifdef _WIN32
  static const char *args[5];
  args[0] = "/d";
  args[1] = "/s";
  args[2] = "/c";
  args[3] = command;
  args[4] = NULL;
#else
  static const char *args[3];
  args[0] = "-c";
  args[1] = command;
  args[2] = NULL;
#endif
  cmeta_process_options_init(options);
  options->program = TEST_SHELL;
  options->args = args;
}

static int read_all(cmeta_process_t *process, int stdout_stream, char *buffer, size_t capacity,
                    size_t *out_size) {
  size_t total = 0;
  int rc = SALTS_OK;
  while (total < capacity - 1) {
    size_t count = 0;
    rc = stdout_stream
             ? cmeta_process_read_stdout(process, buffer + total, capacity - 1 - total, &count)
             : cmeta_process_read_stderr(process, buffer + total, capacity - 1 - total, &count);
    total += count;
    if (rc == SALTS_EOF) break;
    if (rc != SALTS_OK || count == 0) break;
  }
  buffer[total] = '\0';
  *out_size = total;
  return rc;
}

static int path_exists(const char *path) {
  return cmeta_fs_access(path, SALTS_FS_ACCESS_EXISTS) == SALTS_OK;
}

spec("cmeta_process") {
  group("configuration") {
    it("rejects argv in explicit shell mode") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      const char *args[] = {NULL};
      cmeta_process_options_init(&options);
      options.program = "echo unreachable";
      options.args = args;
      options.flags |= SALTS_PROCESS_SHELL_COMMAND;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_EINVAL);
      check_null(process);
    }

    it("preserves quotes and shell metacharacters in explicit shell mode") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      char output[128];
      size_t count = 0u;
      int wait_status;
      int read_status;
      cmeta_process_options_init(&options);
#ifdef _WIN32
      options.program = "echo \"quoted & text\" & exit /b 7";
#else
      options.program = "printf '%s' 'quoted & text'; exit 7";
#endif
      options.flags |= SALTS_PROCESS_SHELL_COMMAND;
      options.timeout_ms = 5000u;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      wait_status = cmeta_process_wait(process, &result);
      read_status = read_all(process, 1, output, sizeof(output), &count);
      cmeta_process_destroy(process);
      check_equal(wait_status, SALTS_OK);
      check_true(read_status == SALTS_OK || read_status == SALTS_EOF);
      info("shell state=%d exit=%d error=%d", result.state, result.exit_code, result.error_code);
      check_equal(result.state, SALTS_PROCESS_EXITED);
      check_equal(result.exit_code, 7);
      check_contains(output, "quoted & text");
      check_null(strchr(output, '\\'));
    }

    it("rejects an external stdout binding combined with capture") {
      cmeta_process_options_t options;
      cmeta_process_stdio_bindings_t bindings = {
          SALTS_PROCESS_STDIO_INHERIT, SALTS_PROCESS_STDIO_INHERIT, SALTS_PROCESS_STDIO_INHERIT};
      cmeta_process_t *process = NULL;
      test_process_pipe output_pipe;
      int rc;
      bool returned_process;

      init_shell_options(&options, "echo unreachable");
      check_equal(test_process_pipe_init(&output_pipe), 0);
      bindings.stdout_handle = test_process_pipe_write_handle(&output_pipe);

      rc = cmeta_process_spawn_with_stdio(&options, &bindings, &process);
      returned_process = process != NULL;
      if (process) cmeta_process_destroy(process);
      test_process_pipe_destroy(&output_pipe);
      check_equal(rc, SALTS_EINVAL);
      check_false(returned_process);
    }

    it("initializes safe defaults and rejects an empty program") {
      cmeta_process_options_t options;
      cmeta_process_t *process = (cmeta_process_t *)1;

      cmeta_process_options_init(&options);
      check_equal((int)options.flags, SALTS_PROCESS_CAPTURE_STDOUT | SALTS_PROCESS_CAPTURE_STDERR);
      check_equal(options.max_output_bytes, SALTS_PROCESS_DEFAULT_MAX_OUTPUT_BYTES);
      check_equal(cmeta_process_spawn(&options, &process), SALTS_EINVAL);
      check_null(process);
    }

    it("reports a missing executable during spawn") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;

      cmeta_process_options_init(&options);
      options.program = "cmeta_process_missing_executable_97531";
      check_less(cmeta_process_spawn(&options, &process), 0);
      check_null(process);
    }

    it("rejects malformed and duplicate environment overrides") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      const char *malformed[] = {"MISSING_EQUALS", NULL};
      const char *duplicate[] = {"SALTS_DUPLICATE=first", "SALTS_DUPLICATE=second", NULL};

      cmeta_process_options_init(&options);
      options.program = TEST_SHELL;
      options.env = malformed;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_EINVAL);
      check_null(process);
      options.env = duplicate;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_EINVAL);
      check_null(process);
    }
  }

  group("execution") {
#ifndef _WIN32
    it("preserves crossed standard descriptor bindings") {
      cmeta_process_options_t options;
      cmeta_process_stdio_bindings_t bindings = {
          SALTS_PROCESS_STDIO_INHERIT, STDERR_FILENO, STDOUT_FILENO};
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result = {0};
      test_process_pipe stdout_destination;
      test_process_pipe stderr_destination;
      char stdout_bytes[32] = {0};
      char stderr_bytes[32] = {0};
      int saved_stdout;
      int saved_stderr;
      int stdout_redirect;
      int stderr_redirect;
      int stdout_restore;
      int stderr_restore;
      int spawn_status = SALTS_EINVAL;
      int wait_status = SALTS_EINVAL;
      int exit_code = -1;
      size_t stdout_size;
      size_t stderr_size;

      init_shell_options(&options, "printf stdout-side; printf stderr-side >&2");
      options.flags &= ~(SALTS_PROCESS_CAPTURE_STDOUT | SALTS_PROCESS_CAPTURE_STDERR);
      check_equal(test_process_pipe_init(&stdout_destination), 0);
      check_equal(test_process_pipe_init(&stderr_destination), 0);
      saved_stdout = dup(STDOUT_FILENO);
      saved_stderr = dup(STDERR_FILENO);
      check_true(saved_stdout >= 0);
      check_true(saved_stderr >= 0);

      stdout_redirect = dup2(stdout_destination.write_handle, STDOUT_FILENO);
      stderr_redirect = dup2(stderr_destination.write_handle, STDERR_FILENO);
      if (stdout_redirect == STDOUT_FILENO && stderr_redirect == STDERR_FILENO)
        spawn_status = cmeta_process_spawn_with_stdio(&options, &bindings, &process);
      stdout_restore = dup2(saved_stdout, STDOUT_FILENO);
      stderr_restore = dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdout);
      close(saved_stderr);
      test_process_pipe_close_write(&stdout_destination);
      test_process_pipe_close_write(&stderr_destination);
      if (process != NULL) {
        wait_status = cmeta_process_wait(process, &result);
        if (wait_status == SALTS_OK) exit_code = result.exit_code;
        cmeta_process_destroy(process);
      }
      stdout_size = test_process_pipe_read_all(&stdout_destination, stdout_bytes,
                                               sizeof(stdout_bytes));
      stderr_size = test_process_pipe_read_all(&stderr_destination, stderr_bytes,
                                               sizeof(stderr_bytes));
      test_process_pipe_destroy(&stdout_destination);
      test_process_pipe_destroy(&stderr_destination);

      check_equal(stdout_redirect, STDOUT_FILENO);
      check_equal(stderr_redirect, STDERR_FILENO);
      check_equal(stdout_restore, STDOUT_FILENO);
      check_equal(stderr_restore, STDERR_FILENO);
      check_equal(spawn_status, SALTS_OK);
      check_equal(wait_status, SALTS_OK);
      check_equal(exit_code, 0);
      check_equal(stdout_size, strlen("stderr-side"));
      check_equal(stdout_bytes, "stderr-side", stdout_size);
      check_equal(stderr_size, strlen("stdout-side"));
      check_equal(stderr_bytes, "stdout-side", stderr_size);
    }
#endif

    it("borrows an external stdout handle without capturing or closing it") {
      cmeta_process_options_t options;
      cmeta_process_stdio_bindings_t bindings = {
          SALTS_PROCESS_STDIO_INHERIT, SALTS_PROCESS_STDIO_INHERIT, SALTS_PROCESS_STDIO_INHERIT};
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      test_process_pipe output_pipe;
      char output[64] = {0};
      char captured[8] = {0};
      size_t output_size;
      size_t captured_size = 1;
#ifdef _WIN32
      const char *expected_output = "bound-output\r\n";
#else
      const char *expected_output = "bound-output";
#endif

#ifdef _WIN32
      init_shell_options(&options, "echo bound-output");
#else
      init_shell_options(&options, "printf bound-output");
#endif
      options.flags &= ~SALTS_PROCESS_CAPTURE_STDOUT;
      check_equal(test_process_pipe_init(&output_pipe), 0);
      bindings.stdout_handle = test_process_pipe_write_handle(&output_pipe);

      check_equal(cmeta_process_spawn_with_stdio(&options, &bindings, &process), SALTS_OK);
      test_process_pipe_close_write(&output_pipe);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.exit_code, 0);
      output_size = test_process_pipe_read_all(&output_pipe, output, sizeof(output));
      check_equal(output_size, strlen(expected_output));
      check_equal(output, expected_output, output_size);
      check_equal(cmeta_process_read_stdout(process, captured, sizeof(captured), &captured_size),
                  SALTS_EOF);
      check_equal(captured_size, 0U);

      cmeta_process_destroy(process);
      test_process_pipe_destroy(&output_pipe);
    }

    it("captures stdout stderr and the exit code") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      char stdout_data[128];
      char stderr_data[128];
      size_t stdout_size = 0;
      size_t stderr_size = 0;
#ifdef _WIN32
      init_shell_options(&options, "echo stdout-line & echo stderr-line 1>&2 & exit /b 7");
#else
      init_shell_options(&options, "printf stdout-line; printf stderr-line >&2; exit 7");
#endif

      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_not_null(process);
      check_greater(cmeta_process_pid(process), 0);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.state, SALTS_PROCESS_EXITED);
      check_equal(result.exit_code, 7);
      check_equal(read_all(process, 1, stdout_data, sizeof(stdout_data), &stdout_size), SALTS_EOF);
      check_equal(read_all(process, 0, stderr_data, sizeof(stderr_data), &stderr_size), SALTS_EOF);
      check_contains(stdout_data, "stdout-line");
      check_contains(stderr_data, "stderr-line");
      cmeta_process_destroy(process);
    }

    it("passes argv environment and working directory without a shell wrapper") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      char *directory = tt_make_temp_dir("salts-process-cwd");
      char marker[512];
      char output[1024];
      size_t output_size = 0;
#ifdef _WIN32
      char script[512];
#endif
      const char *env[] = {"SALTS_PROCESS_VALUE=env-value",
#ifdef _WIN32
                           "PATH=C:\\Windows\\System32;C:\\Windows",
#else
                           "PATH=/usr/local/bin:/usr/bin:/bin",
#endif
                           NULL};
#ifdef _WIN32
      const char *args[] = {"-NoProfile", "-File", script, "alpha beta", NULL};
      cmeta_process_options_init(&options);
      options.program = "powershell.exe";
#else
      const char *args[] = {"-c",
                            "printf '%s|%s|' \"$1\" \"$SALTS_PROCESS_VALUE\"; cat cwd-marker.txt",
                            "probe", "alpha beta", NULL};
      cmeta_process_options_init(&options);
      options.program = "/bin/sh";
#endif
      options.args = args;
      options.env = env;
      options.cwd = directory;

      check_not_null(directory);
      check_equal(cmeta_fs_path_join(marker, sizeof(marker), directory, "cwd-marker.txt"), SALTS_OK);
      check_equal(tt_write_file(marker, "cwd-ok", strlen("cwd-ok")), 0);
#ifdef _WIN32
      check_equal(cmeta_fs_path_join(script, sizeof(script), directory, "probe.ps1"), SALTS_OK);
      {
        const char *script_data =
            "param([string]$value)\n"
            "$marker = Get-Content -Raw -LiteralPath '.\\cwd-marker.txt'\n"
            "Write-Output \"$value|$env:SALTS_PROCESS_VALUE|$marker\"\n";
        cmeta_fs_buf_t script_buffer = cmeta_fs_buf_init((char *)script_data, strlen(script_data));
        check_equal(cmeta_fs_write_file(script, &script_buffer), SALTS_OK);
      }
#endif
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.exit_code, 0);
      read_all(process, 1, output, sizeof(output), &output_size);
      check_contains(output, "alpha beta|env-value|cwd-ok");

      cmeta_process_destroy(process);
      check_equal(tt_remove_tree(directory), 0);
      free(directory);
    }

    it("can replace the inherited environment with explicit entries") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      char output[256];
      size_t output_size = 0;
      const char *env[] = {"SALTS_PROCESS_VALUE=clean-value", NULL};
#ifdef _WIN32
      init_shell_options(
          &options,
          "set SALTS_PROCESS_PARENT_SENTINEL 2>nul & echo marker=%SALTS_PROCESS_VALUE%");
#else
      init_shell_options(
          &options,
          "printf 'parent=%s|marker=%s' \"${SALTS_PROCESS_PARENT_SENTINEL-unset}\" "
          "\"$SALTS_PROCESS_VALUE\"");
#endif
      options.env = env;
      options.flags |= SALTS_PROCESS_CLEAN_ENVIRONMENT;

      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.exit_code, 0);
      read_all(process, 1, output, sizeof(output), &output_size);
#ifdef _WIN32
      check_false(strstr(output, "SALTS_PROCESS_PARENT_SENTINEL=") != NULL);
      check_contains(output, "marker=clean-value");
#else
      check_equal(output, "parent=unset|marker=clean-value");
#endif

      cmeta_process_destroy(process);
      process = NULL;
      options.env = NULL;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.exit_code, 0);
      read_all(process, 1, output, sizeof(output), &output_size);
#ifdef _WIN32
      check_false(strstr(output, "SALTS_PROCESS_PARENT_SENTINEL=") != NULL);
      check_false(strstr(output, "clean-value") != NULL);
#else
      check_equal(output, "parent=unset|marker=");
#endif
      cmeta_process_destroy(process);
    }

    it("pumps large stdin and stdout without pipe deadlock") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      const size_t payload_size = 256U * 1024U;
      char *payload = (char *)malloc(payload_size);
      char *output = (char *)malloc(payload_size + 32U);
      size_t written = 0;
      size_t output_size = 0;
#ifdef _WIN32
      cmeta_process_options_init(&options);
      {
        static const char *copy_args[] = {
            "-NoProfile", "-Command",
            "[Console]::OpenStandardInput().CopyTo([Console]::OpenStandardOutput())", NULL};
        options.program = "powershell.exe";
        options.args = copy_args;
      }
#else
      init_shell_options(&options, "cat");
#endif
      options.flags |= SALTS_PROCESS_PIPE_STDIN;
      options.max_output_bytes = payload_size + 32U;
      memset(payload, 'I', payload_size);

      check_not_null(payload);
      check_not_null(output);
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_write_stdin(process, payload, payload_size, &written), SALTS_OK);
      check_equal(written, payload_size);
      check_equal(cmeta_process_close_stdin(process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.exit_code, 0);
      read_all(process, 1, output, payload_size + 32U, &output_size);
      check_equal(output_size, payload_size);
      check_equal(output, payload, payload_size);

      cmeta_process_destroy(process);
      free(output);
      free(payload);
    }

    it("reports a broken stdin pipe after the child exits") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      size_t written = 1;

#ifdef _WIN32
      init_shell_options(&options, "exit /b 0");
#else
      init_shell_options(&options, "exit 0");
#endif
      options.flags |= SALTS_PROCESS_PIPE_STDIN;

      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.exit_code, 0);
      check_equal(cmeta_process_write_stdin(process, "x", 1, &written), SALTS_EPIPE);
      check_equal(written, 0U);

      cmeta_process_destroy(process);
    }
  }

  group("lifecycle") {
    it("times out a bounded wait without terminating the child") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
#ifdef _WIN32
      init_shell_options(&options, "ping -n 4 127.0.0.1 >nul");
#else
      init_shell_options(&options, "sleep 2");
#endif
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait_for(process, 25, &result), SALTS_ETIMEDOUT);
      check_true(cmeta_process_is_running(process));
      check_equal(cmeta_process_terminate(process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.state, SALTS_PROCESS_TERMINATED);
      cmeta_process_destroy(process);
    }

    it("enforces the configured execution deadline") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
#ifdef _WIN32
      init_shell_options(&options, "ping -n 4 127.0.0.1 >nul");
#else
      init_shell_options(&options, "sleep 2");
#endif
      options.timeout_ms = 100;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.state, SALTS_PROCESS_TIMED_OUT);
      check_equal(result.exit_code, -1);
      cmeta_process_destroy(process);
    }

    it("terminates descendants when the owner is destroyed") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      char *directory = tt_make_temp_dir("salts-process-tree");
      char marker[512];
#ifdef _WIN32
      char *script;
      script = (char *)malloc(512);
      check_not_null(script);
      check_equal(cmeta_fs_path_join(script, 512, directory, "tree.cmd"), SALTS_OK);
      snprintf(marker, sizeof(marker), "%s\\marker.txt", directory);
      check_equal(
          tt_write_file(
              script,
              "@echo off\r\nstart \"\" /b cmd.exe /d /s /c \"ping -n 3 127.0.0.1 >nul & echo "
              "bad>\\\"%~dp0marker.txt\\\"\"\r\nping -n 20 127.0.0.1 >nul\r\n",
              strlen("@echo off\r\nstart \"\" /b cmd.exe /d /s /c \"ping -n 3 127.0.0.1 >nul & "
                     "echo bad>\\\"%~dp0marker.txt\\\"\"\r\nping -n 20 127.0.0.1 >nul\r\n")),
          0);
      {
        const char *args[] = {"/d", "/s", "/c", script, NULL};
        cmeta_process_options_init(&options);
        options.program = "cmd.exe";
        options.args = args;
        check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      }
#else
      const char *script_data = "(sleep 1; echo bad > \"$1\") & sleep 5";
      const char *args[] = {"-c", script_data, "tree-test", marker, NULL};
      snprintf(marker, sizeof(marker), "%s/marker.txt", directory);
      cmeta_process_options_init(&options);
      options.program = "/bin/sh";
      options.args = args;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
#endif
      check_not_null(directory);
      cmeta_sleep_ms(100);
      cmeta_process_destroy(process);
      cmeta_sleep_ms(1500);
      check_false(path_exists(marker));

#ifdef _WIN32
      free(script);
#endif
      check_equal(tt_remove_tree(directory), 0);
      free(directory);
    }

    it("bounds combined captured output") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      char output[1024];
      size_t output_size = 0;
#ifdef _WIN32
      init_shell_options(&options, "for /L %i in (1,1,1000) do @echo 1234567890");
#else
      init_shell_options(&options,
                         "i=0; while [ $i -lt 1000 ]; do echo 1234567890; i=$((i+1)); done");
#endif
      options.max_output_bytes = 512;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.state, SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED);
      read_all(process, 1, output, sizeof(output), &output_size);
      check_less_equal(output_size, 512);
      cmeta_process_destroy(process);
    }
  }

  group("monitoring") {
    it("reports current and completed process liveness") {
      cmeta_process_options_t options;
      cmeta_process_t *process = NULL;
      cmeta_process_result_t result;
      bool alive = false;
      int child_pid;

      check_equal(cmeta_process_is_pid_alive(cmeta_getpid(), &alive), SALTS_OK);
      check_true(alive);
#ifdef _WIN32
      init_shell_options(&options, "exit /b 0");
#else
      init_shell_options(&options, "exit 0");
#endif
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      child_pid = cmeta_process_pid(process);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(cmeta_process_is_pid_alive(child_pid, &alive), SALTS_OK);
      check_false(alive);
      cmeta_process_destroy(process);
    }
  }
}
