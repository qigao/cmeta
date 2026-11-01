#include <cflow/io_pipe.h>

#include <clock.h>
#include <error_codes.h>
#include <thread.h>

#include "tinytest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

typedef struct pipe_completion_probe {
  cflow_io_request_id request_ids[4];
  cflow_io_completion completions[4];
  cflow_io_pipe_endpoint endpoints[4];
  size_t count;
} pipe_completion_probe;

#if defined(_WIN32)
typedef struct pipe_client_thread_probe {
  const char *name;
  cflow_io_pipe_endpoint endpoint;
  volatile LONG started;
  int status;
} pipe_client_thread_probe;

static void pipe_client_connect_until_available(void *user) {
  pipe_client_thread_probe *probe = (pipe_client_thread_probe *)user;
  const uint64_t started = cmeta_hrtime();
  InterlockedExchange(&probe->started, 1);
  do {
    probe->status =
        cflow_io_pipe_client_connect(probe->name, CFLOW_IO_PIPE_DUPLEX, &probe->endpoint);
    if (probe->status != SALTS_ENOENT && probe->status != SALTS_EBUSY) return;
    cmeta_thread_yield();
  } while (cmeta_hrtime() - started <= UINT64_C(2000000000));
  probe->status = SALTS_ETIMEDOUT;
}
#endif

static void pipe_accept_completion(void *user, cflow_io_request_id request_id,
                                   const cflow_io_completion *completion,
                                   cflow_io_pipe_endpoint endpoint) {
  pipe_completion_probe *probe = (pipe_completion_probe *)user;
  if (probe->count < 4u) {
    probe->request_ids[probe->count] = request_id;
    probe->completions[probe->count] = *completion;
    probe->endpoints[probe->count] = endpoint;
    ++probe->count;
  }
}

#if defined(_WIN32)
static int pipe_wait(cflow_io_pipe_server *server, pipe_completion_probe *probe, size_t expected) {
  const uint64_t started = cmeta_hrtime();
  while (probe->count < expected) {
    size_t progressed = 0u;
    int status = cflow_io_pipe_server_run_ready(server, 8u, &progressed);
    if (status != SALTS_OK) return status;
    if (cmeta_hrtime() - started > UINT64_C(5000000000)) return SALTS_ETIMEDOUT;
    if (progressed == 0u) cmeta_thread_yield();
  }
  return SALTS_OK;
}
#endif

spec("CFlow pipe rendezvous") {
  it("reports rendezvous capabilities without implicit backend fallback") {
#if defined(_WIN32)
    check_true(cflow_io_pipe_capability_supported(CFLOW_IO_PIPE_WINDOWS_SERVER_ACCEPT));
    check_true(cflow_io_pipe_capability_supported(CFLOW_IO_PIPE_WINDOWS_CLIENT_CONNECT));
    check_false(cflow_io_pipe_capability_supported(CFLOW_IO_PIPE_POSIX_FIFO_OPEN));
#else
    check_false(cflow_io_pipe_capability_supported(CFLOW_IO_PIPE_WINDOWS_SERVER_ACCEPT));
    check_false(cflow_io_pipe_capability_supported(CFLOW_IO_PIPE_WINDOWS_CLIENT_CONNECT));
    check_true(cflow_io_pipe_capability_supported(CFLOW_IO_PIPE_POSIX_FIFO_OPEN));
#endif
    check_false(cflow_io_pipe_capability_supported((cflow_io_pipe_capability)-1));
  }
#if defined(_WIN32)
  it("rejects an unterminated bounded pipe name without reading past it") {
    char name[256];
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    pipe_completion_probe probe = {0};

    memset(name, 'x', sizeof(name));
    config.name = name;
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 1u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;

    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_EINVAL);
    check_null(server.impl);
  }

  it("transfers one overlapped endpoint after a named pipe client connects") {
    char name[160];
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    cflow_io_pipe_submit_result submitted;
    cflow_io_pipe_endpoint client;
    pipe_completion_probe probe = {0};

    snprintf(name, sizeof(name), "\\\\.\\pipe\\cflow-rendezvous-%lu-%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)cmeta_hrtime());
    cflow_io_pipe_endpoint_init(&client);
    cflow_io_pipe_endpoint_init(&probe.endpoints[0]);
    config.name = name;
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 1u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;

    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_OK);
    submitted = cflow_io_pipe_server_try_accept(&server);
    check_equal(submitted.status, CFLOW_IO_PIPE_SUBMIT_ACCEPTED);
    check_not_equal(submitted.request_id, (cflow_io_request_id)0u);
    check_equal(cflow_io_pipe_client_connect(name, CFLOW_IO_PIPE_DUPLEX, &client), SALTS_OK);
    check_true(cflow_io_pipe_endpoint_is_valid(&client));
    check_equal(pipe_wait(&server, &probe, 1u), SALTS_OK);
    check_equal(probe.request_ids[0], submitted.request_id);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_OK);
    check_true(cflow_io_pipe_endpoint_is_valid(&probe.endpoints[0]));

    check_equal(cflow_io_pipe_endpoint_close(&client), SALTS_OK);
    check_equal(cflow_io_pipe_endpoint_close(&probe.endpoints[0]), SALTS_OK);
    check_equal(cflow_io_pipe_server_close(&server), SALTS_OK);
    check_true(cflow_io_pipe_server_is_quiescent(&server));
    check_equal(cflow_io_pipe_server_destroy(&server), SALTS_OK);
  }

  it("bounds pending instances and reclaims cancelled slots after completion") {
    char name[160];
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    cflow_io_pipe_submit_result first;
    cflow_io_pipe_submit_result second;
    cflow_io_pipe_submit_result third;
    cflow_io_pipe_server_stats stats = {0};
    pipe_completion_probe probe = {0};

    snprintf(name, sizeof(name), "\\\\.\\pipe\\cflow-capacity-%lu-%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)cmeta_hrtime());
    config.name = name;
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 2u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;

    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_OK);
    first = cflow_io_pipe_server_try_accept(&server);
    second = cflow_io_pipe_server_try_accept(&server);
    third = cflow_io_pipe_server_try_accept(&server);
    check_equal(first.status, CFLOW_IO_PIPE_SUBMIT_ACCEPTED);
    check_equal(second.status, CFLOW_IO_PIPE_SUBMIT_ACCEPTED);
    check_equal(third.status, CFLOW_IO_PIPE_SUBMIT_FULL);
    check_true(cflow_io_pipe_server_get_stats(&server, &stats));
    check_equal(stats.request_capacity, (size_t)2u);
    check_equal(stats.active_requests, (size_t)2u);
    check_equal(stats.submitted, (uint64_t)2u);
    check_equal(stats.rejected_full, (uint64_t)1u);
    check_true(stats.admission_open);
    check_equal(cflow_io_pipe_server_try_cancel(&server, first.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(cflow_io_pipe_server_try_cancel(&server, second.request_id),
                CFLOW_IO_CANCEL_ACCEPTED);
    check_equal(pipe_wait(&server, &probe, 2u), SALTS_OK);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(probe.completions[1].kind, CFLOW_IO_COMPLETION_CANCELLED);
    check_equal(cflow_io_pipe_server_close(&server), SALTS_OK);
    check_true(cflow_io_pipe_server_get_stats(&server, &stats));
    check_equal(stats.active_requests, (size_t)0u);
    check_equal(stats.cancelled, (uint64_t)2u);
    check_false(stats.admission_open);
    check_true(cflow_io_pipe_server_is_quiescent(&server));
    check_equal(cflow_io_pipe_server_destroy(&server), SALTS_OK);
  }

  it("maps missing and fully occupied named pipe instances without waiting") {
    char name[160];
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    cflow_io_pipe_endpoint first_client;
    cflow_io_pipe_endpoint second_client;
    cflow_io_pipe_submit_result submitted;
    pipe_completion_probe probe = {0};

    snprintf(name, sizeof(name), "\\\\.\\pipe\\cflow-busy-%lu-%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)cmeta_hrtime());
    cflow_io_pipe_endpoint_init(&first_client);
    cflow_io_pipe_endpoint_init(&second_client);
    check_equal(cflow_io_pipe_client_connect(name, CFLOW_IO_PIPE_DUPLEX, &first_client),
                SALTS_ENOENT);
    config.name = name;
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 1u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;
    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_OK);
    submitted = cflow_io_pipe_server_try_accept(&server);
    check_equal(submitted.status, CFLOW_IO_PIPE_SUBMIT_ACCEPTED);
    check_equal(cflow_io_pipe_client_connect(name, CFLOW_IO_PIPE_DUPLEX, &first_client), SALTS_OK);
    check_equal(cflow_io_pipe_client_connect(name, CFLOW_IO_PIPE_DUPLEX, &second_client),
                SALTS_EBUSY);
    check_equal(pipe_wait(&server, &probe, 1u), SALTS_OK);
    check_equal(cflow_io_pipe_endpoint_close(&first_client), SALTS_OK);
    check_equal(cflow_io_pipe_endpoint_close(&probe.endpoints[0]), SALTS_OK);
    check_equal(cflow_io_pipe_server_close(&server), SALTS_OK);
    check_equal(cflow_io_pipe_server_destroy(&server), SALTS_OK);
  }

  it("turns close of a pending accept into one authoritative cancellation") {
    char name[160];
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    cflow_io_pipe_submit_result submitted;
    pipe_completion_probe probe = {0};
    DWORD handles_before = 0u;
    DWORD handles_after = 0u;

    check_true(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
    snprintf(name, sizeof(name), "\\\\.\\pipe\\cflow-close-%lu-%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)cmeta_hrtime());
    config.name = name;
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 1u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;
    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_OK);
    submitted = cflow_io_pipe_server_try_accept(&server);
    check_equal(submitted.status, CFLOW_IO_PIPE_SUBMIT_ACCEPTED);
    check_equal(cflow_io_pipe_server_close(&server), SALTS_OK);
    check_equal(cflow_io_pipe_server_try_accept(&server).status, CFLOW_IO_PIPE_SUBMIT_CLOSED);
    check_equal(pipe_wait(&server, &probe, 1u), SALTS_OK);
    check_equal(probe.request_ids[0], submitted.request_id);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_CANCELLED);
    check_true(cflow_io_pipe_server_is_quiescent(&server));
    check_equal(cflow_io_pipe_server_destroy(&server), SALTS_OK);
    check_true(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    check_equal(handles_after, handles_before);
  }

  it("settles a client racing CreateNamedPipe and ConnectNamedPipe exactly once") {
    char name[160];
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    pipe_client_thread_probe client = {0};
    pipe_completion_probe probe = {0};
    cflow_io_pipe_submit_result submitted;
    cmeta_thread_t thread = NULL;

    snprintf(name, sizeof(name), "\\\\.\\pipe\\cflow-race-%lu-%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)cmeta_hrtime());
    cflow_io_pipe_endpoint_init(&client.endpoint);
    client.name = name;
    client.status = SALTS_EBUSY;
    config.name = name;
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 1u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;
    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_OK);
    check_equal(cmeta_thread_create(&thread, pipe_client_connect_until_available, &client),
                SALTS_OK);
    while (InterlockedCompareExchange(&client.started, 1, 1) == 0)
      cmeta_thread_yield();
    submitted = cflow_io_pipe_server_try_accept(&server);
    check_equal(submitted.status, CFLOW_IO_PIPE_SUBMIT_ACCEPTED);
    check_equal(cmeta_thread_join(&thread), SALTS_OK);
    check_equal(client.status, SALTS_OK);
    check_equal(pipe_wait(&server, &probe, 1u), SALTS_OK);
    check_equal(probe.count, (size_t)1u);
    check_equal(probe.request_ids[0], submitted.request_id);
    check_equal(probe.completions[0].kind, CFLOW_IO_COMPLETION_OK);
    check_equal(cflow_io_pipe_endpoint_close(&client.endpoint), SALTS_OK);
    check_equal(cflow_io_pipe_endpoint_close(&probe.endpoints[0]), SALTS_OK);
    check_equal(cflow_io_pipe_server_close(&server), SALTS_OK);
    check_equal(cflow_io_pipe_server_destroy(&server), SALTS_OK);
  }
#else
  it("reports Windows named pipe accept as unsupported") {
    cflow_io_pipe_server server = {0};
    cflow_io_pipe_server_config config = {0};
    pipe_completion_probe probe = {0};
    cflow_io_pipe_submit_result submitted;
    size_t progressed = 1u;

    config.name = "not-a-windows-pipe";
    config.direction = CFLOW_IO_PIPE_DUPLEX;
    config.request_capacity = 1u;
    config.input_buffer_size = 4096u;
    config.output_buffer_size = 4096u;
    config.completion = pipe_accept_completion;
    config.completion_user = &probe;
    check_equal(cflow_io_pipe_server_init(&server, &config), SALTS_ENOTSUP);
    submitted = cflow_io_pipe_server_try_accept(&server);
    check_equal(submitted.status, CFLOW_IO_PIPE_SUBMIT_UNSUPPORTED);
    check_equal(submitted.request_id, (cflow_io_request_id)0u);
    check_equal(submitted.error, SALTS_ENOTSUP);
    check_equal(cflow_io_pipe_server_run_ready(&server, 1u, &progressed), SALTS_ENOTSUP);
    check_equal(progressed, (size_t)0u);
    check_equal(cflow_io_pipe_server_close(&server), SALTS_ENOTSUP);
  }
#endif

#if !defined(_WIN32)
  it("opens FIFO endpoints without hiding pathname rendezvous") {
    char *directory = tt_make_temp_dir("cflow-fifo-");
    char path[512];
    cflow_io_pipe_endpoint reader;
    cflow_io_pipe_endpoint writer;
    static const char payload[] = "fifo-data";
    char received[sizeof(payload)] = {0};

    check_not_null(directory);
    snprintf(path, sizeof(path), "%s/channel", directory);
    check_equal(mkfifo(path, 0600), 0);
    cflow_io_pipe_endpoint_init(&reader);
    cflow_io_pipe_endpoint_init(&writer);
    check_equal(cflow_io_fifo_open(path, CFLOW_IO_PIPE_WRITE, &writer), SALTS_EPIPE);
    check_false(cflow_io_pipe_endpoint_is_valid(&writer));
    check_equal(cflow_io_fifo_open(path, CFLOW_IO_PIPE_READ, &reader), SALTS_OK);
    check_equal(cflow_io_fifo_open(path, CFLOW_IO_PIPE_WRITE, &writer), SALTS_OK);
    check_equal(write((int)writer.handle, payload, sizeof(payload) - 1u),
                (ssize_t)(sizeof(payload) - 1u));
    check_equal(read((int)reader.handle, received, sizeof(received)),
                (ssize_t)(sizeof(payload) - 1u));
    check_equal(received, payload, sizeof(payload) - 1u);
    check_equal(cflow_io_pipe_endpoint_close(&writer), SALTS_OK);
    check_equal(cflow_io_pipe_endpoint_close(&reader), SALTS_OK);
    check_equal(tt_remove_tree(directory), 0);
    free(directory);
  }

  it("rejects a regular file independently of stale errno") {
    char *path = tt_make_temp_file("cflow-not-fifo-", ".bin");
    cflow_io_pipe_endpoint endpoint;

    check_not_null(path);
    cflow_io_pipe_endpoint_init(&endpoint);
    errno = EACCES;
    check_equal(cflow_io_fifo_open(path, CFLOW_IO_PIPE_READ, &endpoint), SALTS_ENOTSUP);
    check_false(cflow_io_pipe_endpoint_is_valid(&endpoint));
    check_equal(tt_remove_file(path), 0);
    free(path);
  }
#endif
}
