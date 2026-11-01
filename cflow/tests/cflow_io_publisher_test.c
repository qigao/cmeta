#include <cflow/cflow.h>

#include "../src/io_publisher_internal.h"
#include "tinytest.h"

#include <thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IO_SOURCE_WAIT_SLICE_NS = 10 * 1000 * 1000,
    IO_SOURCE_WAIT_LIMIT = 500,
    IO_SOURCE_NESTED_WAIT_LIMIT = 50,
    IO_SOURCE_CANCEL_OBSERVATION_NS = 20 * 1000 * 1000
};

typedef struct io_source_backend_request {
    cflow_io_actor *actor;
    cflow_io_request_id request_id;
    cflow_io_lease_id lease_id;
    bool active;
} io_source_backend_request;

typedef struct io_source_fixture {
    cflow_io_publisher_prepare_status prepare_status;
    const char *prepare_error;
    cflow_read_status encode_status;
    const char *encode_error;
    int encoded_value;
    size_t prepare_calls;
    size_t encode_calls;
    size_t backend_submit_calls;
    size_t backend_cancel_calls;
    size_t drive_calls;
    size_t release_calls;
    size_t drive_run_calls;
    size_t drive_run_progressed;
    size_t drive_run_busy;
    size_t drive_run_busy_progressed;
    int drive_run_error;
    int encoded_values[4];
    size_t encoded_value_count;
    size_t prepare_operation_limit;
    bool prepare_error_at_limit;
    uint64_t acknowledged_before_prepare[4];
    cflow_io_publisher_owner *prepare_stats_owner;
    cflow_io_publisher_owner *stats_owner;
    cflow_io_publisher_owner *drive_owner;
    cflow_io_actor *backend_actor;
    cflow_io_request_id backend_request_id;
    io_source_backend_request
        backend_requests[CFLOW_IO_PUBLISHER_MAX_WINDOW];
    size_t backend_request_count;
    size_t backend_active;
    size_t backend_active_max;
    cflow_publisher *cancel_source_during_prepare;
    cflow_io_publisher_stats drive_stats;
    bool prepare_valid_operation;
    bool prepare_empty_operation;
    bool complete_during_submit;
    bool drive_owner_inline;
    bool close_owner_after_successful_drive;
    bool drive_close_owner_preserved;
    bool capture_stats_on_drive;
    bool drive_stats_captured;
    size_t drive_close_calls;
    int drive_close_status;
} io_source_fixture;

typedef struct io_source_inline_scheduler {
    cflow_task_id next_id;
    size_t posts;
    bool closed;
} io_source_inline_scheduler;

typedef struct io_source_sink_probe {
    size_t values;
    int received[8];
    size_t errors;
    size_t done;
    const char *error;
} io_source_sink_probe;

typedef struct io_source_run_fixture {
    cflow_graph surface;
    cflow_graph normalized;
    cflow_scheduler scheduler;
    io_source_inline_scheduler inline_scheduler;
    cflow_publisher source;
    cflow_io_publisher_owner owner;
    cflow_subscription run;
    io_source_sink_probe sink_probe;
    cflow_subscriber_callbacks sink_callbacks;
    cflow_subscriber sink;
} io_source_run_fixture;

typedef struct io_source_blocking_wake_probe {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    bool entered;
    bool released;
    bool close_returned;
} io_source_blocking_wake_probe;

typedef struct io_source_close_context {
    cflow_publisher *source;
    io_source_blocking_wake_probe *probe;
} io_source_close_context;

typedef struct io_source_drive_context {
    cflow_io_publisher_owner *owner;
    size_t max_steps;
    size_t progressed;
    int status;
} io_source_drive_context;

typedef struct io_source_tail_barrier {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    bool entered;
    bool released;
} io_source_tail_barrier;

typedef struct io_source_reentrant_cancel_probe {
    cflow_publisher *source;
    bool returned;
} io_source_reentrant_cancel_probe;

typedef struct io_source_nested_wake_probe {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    cflow_publisher *source_a;
    cflow_io_publisher_owner *owner_a;
    cflow_waitable waitable_a;
    cflow_waitable waitable_b;
    size_t wake_a_calls;
    size_t wake_b_calls;
    size_t unexpected_a_calls;
    int owner_a_close_status;
    bool arm_a_accepted;
    bool arm_b_accepted;
    bool rearm_a_accepted;
    bool destroy_a_returned;
    bool outer_a_returned;
    bool worker_completed;
} io_source_nested_wake_probe;

static cflow_schedule_result io_source_inline_try_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    io_source_inline_scheduler *scheduler =
        (io_source_inline_scheduler *)self;
    cflow_task_id task_id;

    if (scheduler == NULL || fn == NULL || delay_ticks != 0u)
        return (cflow_schedule_result){
            CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    if (scheduler->closed)
        return (cflow_schedule_result){CFLOW_ADMISSION_CLOSED, 0u};
    if (scheduler->next_id == UINT64_MAX)
        return (cflow_schedule_result){
            CFLOW_ADMISSION_ALLOCATION_FAILED, 0u};
    task_id = ++scheduler->next_id;
    ++scheduler->posts;
    fn(user);
    return (cflow_schedule_result){CFLOW_ADMISSION_ACCEPTED, task_id};
}

static cflow_task_id io_source_inline_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    return io_source_inline_try_post_after(
        self, delay_ticks, fn, user).task_id;
}

static bool io_source_inline_cancel(void *self, cflow_task_id task_id) {
    (void)self;
    (void)task_id;
    return false;
}

static bool io_source_inline_run_one(void *self) {
    (void)self;
    return false;
}

static size_t io_source_inline_run_ready(void *self) {
    (void)self;
    return 0u;
}

static size_t io_source_inline_advance(void *self, uint64_t ticks) {
    (void)self;
    (void)ticks;
    return 0u;
}

static size_t io_source_inline_run_until_idle(
    void *self, size_t max_steps) {
    (void)self;
    (void)max_steps;
    return 0u;
}

static bool io_source_inline_wait_idle(void *self) {
    return self != NULL;
}

static uint64_t io_source_inline_now(void *self) {
    (void)self;
    return 0u;
}

static size_t io_source_inline_pending(void *self) {
    (void)self;
    return 0u;
}

static bool io_source_inline_shutdown(void *self) {
    io_source_inline_scheduler *scheduler =
        (io_source_inline_scheduler *)self;

    if (scheduler == NULL)
        return false;
    scheduler->closed = true;
    return true;
}

static bool io_source_inline_get_stats(
    void *self, cflow_scheduler_stats *out) {
    if (self == NULL || out == NULL)
        return false;
    *out = (cflow_scheduler_stats){0};
    return true;
}

static void io_source_inline_destroy(void *self) {
    if (self != NULL)
        ((io_source_inline_scheduler *)self)->closed = true;
}

CMETA_IMPLEMENTS(cflow_scheduler, io_source_inline_scheduler_iface, 0,
    .try_post_after = io_source_inline_try_post_after,
    .post_after = io_source_inline_post_after,
    .cancel = io_source_inline_cancel,
    .run_one = io_source_inline_run_one,
    .run_ready = io_source_inline_run_ready,
    .advance = io_source_inline_advance,
    .run_until_idle = io_source_inline_run_until_idle,
    .wait_idle = io_source_inline_wait_idle,
    .now = io_source_inline_now,
    .pending = io_source_inline_pending,
    .shutdown = io_source_inline_shutdown,
    .get_stats = io_source_inline_get_stats,
    .destroy = io_source_inline_destroy
);

static void io_source_blocking_wake(void *user) {
    io_source_blocking_wake_probe *probe =
        (io_source_blocking_wake_probe *)user;

    if (probe == NULL)
        return;
    cmeta_mutex_lock(&probe->lock);
    probe->entered = true;
    cmeta_cond_broadcast(&probe->changed);
    while (!probe->released)
        cmeta_cond_wait(&probe->changed, &probe->lock);
    cmeta_mutex_unlock(&probe->lock);
}

static void io_source_close_thread(void *user) {
    io_source_close_context *context =
        (io_source_close_context *)user;

    cflow_publisher_destroy(context->source);
    cmeta_mutex_lock(&context->probe->lock);
    context->probe->close_returned = true;
    cmeta_cond_broadcast(&context->probe->changed);
    cmeta_mutex_unlock(&context->probe->lock);
}

static void io_source_drive_thread(void *user) {
    io_source_drive_context *context =
        (io_source_drive_context *)user;

    context->status = cflow_io_publisher_owner_run_ready(
        context->owner, context->max_steps, &context->progressed);
}

static void io_source_tail_barrier_task(void *user) {
    io_source_tail_barrier *barrier =
        (io_source_tail_barrier *)user;

    cmeta_mutex_lock(&barrier->lock);
    barrier->entered = true;
    cmeta_cond_broadcast(&barrier->changed);
    while (!barrier->released)
        cmeta_cond_wait(&barrier->changed, &barrier->lock);
    cmeta_mutex_unlock(&barrier->lock);
}

static bool io_source_tail_barrier_wait(
    io_source_tail_barrier *barrier) {
    size_t waits = 0u;
    bool entered;

    cmeta_mutex_lock(&barrier->lock);
    while (!barrier->entered && waits++ < IO_SOURCE_WAIT_LIMIT)
        (void)cmeta_cond_timedwait(
            &barrier->changed, &barrier->lock,
            IO_SOURCE_WAIT_SLICE_NS);
    entered = barrier->entered;
    cmeta_mutex_unlock(&barrier->lock);
    return entered;
}

static void io_source_tail_barrier_release(
    io_source_tail_barrier *barrier) {
    cmeta_mutex_lock(&barrier->lock);
    barrier->released = true;
    cmeta_cond_broadcast(&barrier->changed);
    cmeta_mutex_unlock(&barrier->lock);
}

static void io_source_reentrant_cancel(void *user) {
    io_source_reentrant_cancel_probe *probe =
        (io_source_reentrant_cancel_probe *)user;

    cflow_publisher_cancel(probe->source);
    probe->returned = true;
}

static void io_source_unexpected_wake(void *user) {
    io_source_nested_wake_probe *probe =
        (io_source_nested_wake_probe *)user;

    ++probe->unexpected_a_calls;
}

static void io_source_nested_wake_b(void *user) {
    io_source_nested_wake_probe *probe =
        (io_source_nested_wake_probe *)user;

    ++probe->wake_b_calls;
    cflow_publisher_destroy(probe->source_a);
    probe->destroy_a_returned = true;
    probe->rearm_a_accepted = cflow_waitable_arm(
        &probe->waitable_a,
        (cflow_waker){io_source_unexpected_wake, probe});
    probe->owner_a_close_status =
        cflow_io_publisher_owner_close(probe->owner_a);
}

static void io_source_nested_wake_a(void *user) {
    io_source_nested_wake_probe *probe =
        (io_source_nested_wake_probe *)user;

    ++probe->wake_a_calls;
    probe->arm_b_accepted = cflow_waitable_arm(
        &probe->waitable_b,
        (cflow_waker){io_source_nested_wake_b, probe});
    probe->outer_a_returned = true;
}

static void io_source_nested_arm_thread(void *user) {
    io_source_nested_wake_probe *probe =
        (io_source_nested_wake_probe *)user;

    probe->arm_a_accepted = cflow_waitable_arm(
        &probe->waitable_a,
        (cflow_waker){io_source_nested_wake_a, probe});
    cmeta_mutex_lock(&probe->lock);
    probe->worker_completed = true;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->lock);
}

static void io_source_operation_release(void *user) {
    io_source_fixture *fixture = (io_source_fixture *)user;
    if (fixture != NULL)
        ++fixture->release_calls;
}

static cflow_io_publisher_prepare_status io_source_prepare(
    void *user, cflow_io_operation *operation, const char **error) {
    io_source_fixture *fixture = (io_source_fixture *)user;
    const size_t call_index = fixture->prepare_calls;
    cflow_io_publisher_stats stats = {0};

    if (call_index < sizeof(fixture->acknowledged_before_prepare) /
                         sizeof(fixture->acknowledged_before_prepare[0]) &&
        fixture->prepare_stats_owner != NULL &&
        cflow_io_publisher_owner_get_stats(
            fixture->prepare_stats_owner, &stats))
        fixture->acknowledged_before_prepare[call_index] =
            stats.actor.acknowledged;
    ++fixture->prepare_calls;
    if (fixture->prepare_status == CFLOW_IO_PUBLISHER_PREPARE_OPERATION &&
        fixture->prepare_operation_limit != 0u &&
        call_index >= fixture->prepare_operation_limit) {
        if (fixture->prepare_error_at_limit) {
            *error = fixture->prepare_error;
            return CFLOW_IO_PUBLISHER_PREPARE_ERROR;
        }
        return CFLOW_IO_PUBLISHER_PREPARE_DONE;
    }
    if (fixture->prepare_status == CFLOW_IO_PUBLISHER_PREPARE_OPERATION) {
        if (!fixture->prepare_empty_operation) {
            operation->user = fixture;
            operation->release = fixture->prepare_valid_operation
                ? io_source_operation_release : NULL;
        }
        if (fixture->cancel_source_during_prepare != NULL)
            cflow_publisher_cancel(fixture->cancel_source_during_prepare);
    } else if (fixture->prepare_status == CFLOW_IO_PUBLISHER_PREPARE_ERROR) {
        *error = fixture->prepare_error;
    }
    return fixture->prepare_status;
}

static cflow_read_status io_source_encode(
    void *user,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user,
    const cflow_io_completion *completion,
    void *out_value,
    const char **error) {
    io_source_fixture *fixture = (io_source_fixture *)user;
    const size_t call_index = fixture->encode_calls;

    (void)request_id;
    (void)lease_id;
    (void)operation_user;
    (void)completion;
    ++fixture->encode_calls;
    if (fixture->encode_status == CFLOW_READ_VALUE ||
        fixture->encode_status == CFLOW_READ_VALUE_AND_DONE) {
        *(int *)out_value = call_index < fixture->encoded_value_count
            ? fixture->encoded_values[call_index]
            : fixture->encoded_value;
    }
    if (fixture->encode_status == CFLOW_READ_ERROR)
        *error = fixture->encode_error;
    return fixture->encode_status;
}

static cflow_io_complete_status io_source_complete_backend_at(
    io_source_fixture *fixture, size_t request_index,
    const cflow_io_completion *completion) {
    cflow_io_complete_status status;
    io_source_backend_request *request;

    if (fixture == NULL ||
        request_index >= fixture->backend_request_count)
        return CFLOW_IO_COMPLETE_INVALID_ARGUMENT;
    request = &fixture->backend_requests[request_index];
    status = cflow_io_actor_complete(
        request->actor, request->request_id,
        completion);
    if (status == CFLOW_IO_COMPLETE_ACCEPTED && request->active) {
        request->active = false;
        --fixture->backend_active;
    }
    return status;
}

static cflow_io_complete_status io_source_complete_backend(
    io_source_fixture *fixture,
    const cflow_io_completion *completion) {
    if (fixture == NULL || fixture->backend_request_count == 0u)
        return CFLOW_IO_COMPLETE_INVALID_ARGUMENT;
    return io_source_complete_backend_at(
        fixture, fixture->backend_request_count - 1u, completion);
}

static int io_source_backend_submit(
    void *user,
    cflow_io_actor *actor,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user) {
    io_source_fixture *fixture = (io_source_fixture *)user;

    (void)actor;
    (void)request_id;
    (void)lease_id;
    (void)operation_user;
    ++fixture->backend_submit_calls;
    fixture->backend_actor = actor;
    fixture->backend_request_id = request_id;
    if (fixture->backend_request_count < CFLOW_IO_PUBLISHER_MAX_WINDOW) {
        io_source_backend_request *request =
            &fixture->backend_requests[fixture->backend_request_count++];
        request->actor = actor;
        request->request_id = request_id;
        request->lease_id = lease_id;
        request->active = true;
    }
    ++fixture->backend_active;
    if (fixture->backend_active > fixture->backend_active_max)
        fixture->backend_active_max = fixture->backend_active;
    if (fixture->complete_during_submit) {
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        (void)io_source_complete_backend(fixture, &completion);
    }
    return SALTS_OK;
}

static int io_source_backend_cancel(
    void *user, cflow_io_request_id request_id) {
    io_source_fixture *fixture = (io_source_fixture *)user;

    (void)request_id;
    ++fixture->backend_cancel_calls;
    return SALTS_OK;
}

static void io_source_drive(void *user) {
    io_source_fixture *fixture = (io_source_fixture *)user;
    ++fixture->drive_calls;
    if (fixture->drive_owner_inline && fixture->drive_owner != NULL) {
        size_t progressed = 0u;
        const int status = cflow_io_publisher_owner_run_ready(
            fixture->drive_owner, 32u, &progressed);

        ++fixture->drive_run_calls;
        fixture->drive_run_progressed += progressed;
        if (status == SALTS_EBUSY) {
            ++fixture->drive_run_busy;
            fixture->drive_run_busy_progressed += progressed;
        } else if (status != SALTS_OK) {
            fixture->drive_run_error = status;
        }
        if (status == SALTS_OK &&
            fixture->close_owner_after_successful_drive) {
            ++fixture->drive_close_calls;
            fixture->drive_close_status =
                cflow_io_publisher_owner_close(fixture->drive_owner);
            fixture->drive_close_owner_preserved =
                fixture->drive_owner->impl != NULL;
        }
    }
    if (fixture->capture_stats_on_drive &&
        fixture->stats_owner != NULL) {
        fixture->drive_stats_captured =
            cflow_io_publisher_owner_get_stats(
                fixture->stats_owner, &fixture->drive_stats);
    }
}

static cflow_io_publisher_config io_source_config(
    io_source_fixture *fixture) {
    cflow_io_publisher_config config = {0};

    config.name = "test-io-source";
    config.type = &cmeta_type_int;
    config.backend.submit = io_source_backend_submit;
    config.backend.cancel = io_source_backend_cancel;
    config.backend_user = fixture;
    config.prepare = io_source_prepare;
    config.encode = io_source_encode;
    config.user = fixture;
    config.drive = io_source_drive;
    config.drive_user = fixture;
    return config;
}

static bool io_source_sink_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    io_source_sink_probe *probe = (io_source_sink_probe *)user;

    (void)type;
    if (probe->values < sizeof(probe->received) / sizeof(probe->received[0]))
        probe->received[probe->values] = *(const int *)value;
    ++probe->values;
    return true;
}

static void io_source_sink_error(void *user, const char *message) {
    io_source_sink_probe *probe = (io_source_sink_probe *)user;

    ++probe->errors;
    probe->error = message;
}

static void io_source_sink_done(void *user) {
    io_source_sink_probe *probe = (io_source_sink_probe *)user;
    ++probe->done;
}

static bool io_source_run_fixture_init_mode(
    io_source_run_fixture *run_fixture, io_source_fixture *fixture,
    bool inline_scheduler, size_t window_capacity) {
    cflow_io_publisher_config config = io_source_config(fixture);
    bool surface_initialized = false;
    bool normalized_initialized = false;
    bool scheduler_initialized = false;

    memset(run_fixture, 0, sizeof(*run_fixture));
    run_fixture->normalized.root = CMETA_INVALID_ID;
    cflow_graph_init(&run_fixture->surface, &cmeta_type_int);
    surface_initialized = true;
    if (!cflow_graph_normalize(
            &run_fixture->normalized, &run_fixture->surface))
        goto cleanup;
    normalized_initialized = true;
    if (inline_scheduler) {
        run_fixture->scheduler =
            io_source_inline_scheduler_iface_as_cflow_scheduler(
                &run_fixture->inline_scheduler);
    } else if (!cflow_scheduler_test_init(&run_fixture->scheduler)) {
        goto cleanup;
    }
    scheduler_initialized = true;
    if (window_capacity == 0u) {
        if (cflow_publisher_from_io_actor(
                &run_fixture->source, &run_fixture->owner,
                &config) != SALTS_OK)
            goto cleanup;
    } else if (cflow_publisher_from_io_actor_windowed(
                   &run_fixture->source, &run_fixture->owner,
                   &config, window_capacity) != SALTS_OK) {
        goto cleanup;
    }

    run_fixture->sink_callbacks = (cflow_subscriber_callbacks){
        io_source_sink_value,
        io_source_sink_error,
        io_source_sink_done,
        &run_fixture->sink_probe
    };
    run_fixture->sink = cflow_subscriber_from_callbacks(
        &run_fixture->sink_callbacks);
    return true;

cleanup:
    if (cflow_publisher_valid(&run_fixture->source)) {
        cflow_publisher_destroy(&run_fixture->source);
        (void)cflow_io_publisher_owner_close(&run_fixture->owner);
    }
    if (scheduler_initialized)
        cflow_scheduler_destroy(&run_fixture->scheduler);
    if (normalized_initialized)
        cflow_graph_destroy(&run_fixture->normalized);
    if (surface_initialized)
        cflow_graph_destroy(&run_fixture->surface);
    return false;
}

static bool io_source_run_fixture_init_with_scheduler(
    io_source_run_fixture *run_fixture, io_source_fixture *fixture,
    bool inline_scheduler) {
    return io_source_run_fixture_init_mode(
        run_fixture, fixture, inline_scheduler, 0u);
}

static bool io_source_run_fixture_init(
    io_source_run_fixture *run_fixture, io_source_fixture *fixture) {
    return io_source_run_fixture_init_with_scheduler(
        run_fixture, fixture, false);
}

static bool io_source_run_fixture_init_windowed(
    io_source_run_fixture *run_fixture, io_source_fixture *fixture,
    size_t window_capacity) {
    return io_source_run_fixture_init_mode(
        run_fixture, fixture, false, window_capacity);
}

static void io_source_run_fixture_close(
    io_source_run_fixture *run_fixture) {
    cflow_subscription_close(&run_fixture->run);
    check_equal(cflow_io_publisher_owner_close(
                    &run_fixture->owner), SALTS_OK);
    cflow_scheduler_destroy(&run_fixture->scheduler);
    cflow_graph_destroy(&run_fixture->normalized);
    cflow_graph_destroy(&run_fixture->surface);
}

spec("CFlow reactive IO source") {
    it("rejects an empty configuration without mutating outputs") {
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_config config = {0};
        cflow_io_publisher_stats stats = {0};

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(owner.impl);
        check_false(stats.publisher_live);
    }

    it("rejects a window capacity outside the public bound") {
        io_source_fixture fixture = {0};
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};

        check_equal(cflow_publisher_from_io_actor_windowed(
                        &source, &owner, &config, 0u), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(owner.impl);

        check_equal(cflow_publisher_from_io_actor_windowed(
                        &source, &owner, &config,
                        CFLOW_IO_PUBLISHER_MAX_WINDOW + 1u),
                    SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(owner.impl);
    }

    it("rejects the serial batch phase when a drive callback may reenter") {
        io_source_fixture fixture = {0};
        io_source_run_fixture run_fixture;
        size_t progressed = 99u;

        check_true(io_source_run_fixture_init(
            &run_fixture, &fixture));
        check_equal(
            cflow_io_publisher_owner_run_serial_batch_phase_internal(
                &run_fixture.owner, 8u, &progressed),
            SALTS_ENOTSUP);
        check_equal(progressed, (size_t)0u);

        cflow_publisher_destroy(&run_fixture.source);
        check_equal(cflow_io_publisher_owner_close(
                        &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
    }

    it("keeps capacity one identical through the windowed constructor") {
        io_source_fixture sequential = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE_AND_DONE,
            .encoded_value = 37,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        io_source_fixture windowed = sequential;
        io_source_run_fixture sequential_run;
        io_source_run_fixture windowed_run;
        cflow_io_publisher_stats sequential_stats = {0};
        cflow_io_publisher_stats windowed_stats = {0};
        cflow_io_publisher_window_stats sequential_window = {0};
        cflow_io_publisher_window_stats windowed_window = {0};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init(
            &sequential_run, &sequential));
        check_true(cflow_subscribe(
            &sequential_run.run, &sequential_run.normalized,
            &sequential_run.source, &sequential_run.scheduler,
            &sequential_run.sink));
        check_true(cflow_subscription_request(&sequential_run.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &sequential_run.scheduler, 0u);
        check_equal(cflow_io_publisher_owner_run_ready(
            &sequential_run.owner, 32u, &progressed), SALTS_OK);
        (void)cflow_scheduler_run_until_idle(
            &sequential_run.scheduler, 0u);
        check_true(cflow_io_publisher_owner_get_stats(
            &sequential_run.owner, &sequential_stats));
        check_true(cflow_io_publisher_owner_get_window_stats(
            &sequential_run.owner, &sequential_window));

        progressed = 0u;
        check_true(io_source_run_fixture_init_windowed(
            &windowed_run, &windowed, 1u));
        check_true(cflow_subscribe(
            &windowed_run.run, &windowed_run.normalized,
            &windowed_run.source, &windowed_run.scheduler,
            &windowed_run.sink));
        check_true(cflow_subscription_request(&windowed_run.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &windowed_run.scheduler, 0u);
        check_equal(cflow_io_publisher_owner_run_ready(
            &windowed_run.owner, 32u, &progressed), SALTS_OK);
        (void)cflow_scheduler_run_until_idle(
            &windowed_run.scheduler, 0u);
        check_true(cflow_io_publisher_owner_get_stats(
            &windowed_run.owner, &windowed_stats));
        check_true(cflow_io_publisher_owner_get_window_stats(
            &windowed_run.owner, &windowed_window));

        check_equal(windowed.prepare_calls, sequential.prepare_calls);
        check_equal(windowed.backend_submit_calls,
                    sequential.backend_submit_calls);
        check_equal(windowed.encode_calls, sequential.encode_calls);
        check_equal(windowed.release_calls, sequential.release_calls);
        check_equal(windowed_run.sink_probe.values,
                    sequential_run.sink_probe.values);
        check_equal(windowed_run.sink_probe.received[0],
                    sequential_run.sink_probe.received[0]);
        check_equal(windowed_run.sink_probe.done,
                    sequential_run.sink_probe.done);
        check_equal(windowed_stats.actor.request_capacity,
                    sequential_stats.actor.request_capacity);
        check_equal(windowed_stats.actor.command_capacity,
                    sequential_stats.actor.command_capacity);
        check_equal(windowed_stats.actor.acknowledged,
                    sequential_stats.actor.acknowledged);
        check_equal(sequential_window.peak_occupied, (size_t)1u);
        check_equal(windowed_window.peak_occupied, (size_t)1u);

        io_source_run_fixture_close(&windowed_run);
        io_source_run_fixture_close(&sequential_run);
    }

    it("preserves an occupied Source on rejected construction") {
        io_source_fixture fixture = {0};
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_publisher original_source;
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_owner second_owner = {0};

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        original_source = source;
        check_equal(cflow_publisher_from_io_actor(
                        &source, &second_owner, &config), SALTS_EINVAL);
        check_true(cflow_publisher_valid(&source));
        check_equal(&source, &original_source, sizeof(source));
        check_null(second_owner.impl);

        if (cflow_publisher_valid(&source))
            cflow_publisher_destroy(&source);
        else
            cflow_publisher_destroy(&original_source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("preserves an occupied owner on rejected construction") {
        io_source_fixture fixture = {0};
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_publisher second_source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_owner original_owner;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        original_owner = owner;
        check_equal(cflow_publisher_from_io_actor(
                        &second_source, &owner, &config), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&second_source));
        check_equal(owner.impl, original_owner.impl);

        cflow_publisher_destroy(&source);
        if (owner.impl != NULL)
            check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        else
            check_equal(cflow_io_publisher_owner_close(
                            &original_owner), SALTS_OK);
    }

    it("requires only the callbacks needed by the adapter protocol") {
        io_source_fixture fixture = {0};
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};

        config.backend.submit = NULL;
        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(owner.impl);

        config = io_source_config(&fixture);
        config.prepare = NULL;
        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(owner.impl);

        config = io_source_config(&fixture);
        config.encode = NULL;
        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_EINVAL);
        check_false(cflow_publisher_valid(&source));
        check_null(owner.impl);

        config = io_source_config(&fixture);
        config.drive = NULL;
        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        check_true(cflow_publisher_valid(&source));
        check_not_null(owner.impl);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        source = (cflow_publisher){0};

        config = io_source_config(&fixture);
        config.backend.cancel = NULL;
        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("does not prepare or submit without downstream demand") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_equal(fixture.prepare_calls, (size_t)0u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(cflow_scheduler_run_until_idle(
                        &run_fixture.scheduler, 0u), (size_t)0u);
        check_equal(fixture.prepare_calls, (size_t)0u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.request_capacity, (size_t)1u);
        check_equal(stats.actor.command_capacity, (size_t)1u);
        check_true(stats.publisher_live);
        check_false(stats.request_active);
        check_false(stats.result_ready);
        check_false(stats.close_requested);
        check_equal(cflow_io_publisher_owner_close(
                        &run_fixture.owner), SALTS_EBUSY);
        check_not_null(run_fixture.owner.impl);

        io_source_run_fixture_close(&run_fixture);
        check_equal(fixture.release_calls, (size_t)0u);
    }

    it("fills a bounded window from one downstream demand snapshot") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};
        cflow_io_publisher_window_stats window = {0};
        const cflow_io_completion cancelled = {
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init_windowed(
            &run_fixture, &fixture, 4u));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 6u));

        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(fixture.prepare_calls, (size_t)4u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.request_capacity, (size_t)4u);
        check_equal(stats.actor.command_capacity, (size_t)4u);
        check_equal(stats.actor.active_requests, (size_t)4u);
        check_true(cflow_io_publisher_owner_get_window_stats(
            &run_fixture.owner, &window));
        check_equal(window.capacity, (size_t)4u);
        check_equal(window.occupied, (size_t)4u);
        check_equal(window.demand_reserved, (size_t)4u);
        check_equal(window.results_ready, (size_t)0u);
        check_equal(window.peak_occupied, (size_t)4u);

        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_true(progressed >= (size_t)4u);
        check_equal(fixture.backend_submit_calls, (size_t)4u);
        check_equal(fixture.backend_active, (size_t)4u);
        check_equal(fixture.backend_active_max, (size_t)4u);

        cflow_subscription_close(&run_fixture.run);
        for (size_t index = 0u;
             index < fixture.backend_request_count; ++index) {
            if (fixture.backend_requests[index].active)
                check_equal(io_source_complete_backend_at(
                    &fixture, index, &cancelled),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        }
        while (!cflow_io_publisher_owner_is_quiescent(
                   &run_fixture.owner)) {
            progressed = 0u;
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
        }
        check_equal(cflow_io_publisher_owner_close(
            &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
        check_equal(fixture.release_calls, (size_t)4u);
    }

    it("emits windowed values in authoritative completion delivery order") {
        const size_t completion_order[] = {2u, 0u, 3u, 1u};
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_values = {30, 10, 40, 20},
            .encoded_value_count = 4u,
            .encoded_value = 7,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};
        cflow_io_publisher_window_stats window = {0};
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init_windowed(
            &run_fixture, &fixture, 4u));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 4u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_equal(fixture.backend_submit_calls, (size_t)4u);

        for (size_t index = 0u;
             index < sizeof(completion_order) /
                         sizeof(completion_order[0]); ++index) {
            progressed = 0u;
            check_equal(io_source_complete_backend_at(
                &fixture, completion_order[index], &completion),
                CFLOW_IO_COMPLETE_ACCEPTED);
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
            check_equal(fixture.encode_calls, index + 1u);
        }

        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(run_fixture.sink_probe.values, (size_t)4u);
        check_equal(run_fixture.sink_probe.received[0], 30);
        check_equal(run_fixture.sink_probe.received[1], 10);
        check_equal(run_fixture.sink_probe.received[2], 40);
        check_equal(run_fixture.sink_probe.received[3], 20);
        check_equal(fixture.release_calls, (size_t)4u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.acknowledged, (uint64_t)4u);
        check_equal(stats.actor.stale_completions, (uint64_t)0u);
        check_equal(stats.actor.rejected_request_full, (uint64_t)0u);
        check_equal(stats.actor.rejected_command_full, (uint64_t)0u);
        check_true(cflow_io_publisher_owner_get_window_stats(
            &run_fixture.owner, &window));
        check_equal(window.occupied, (size_t)0u);
        check_equal(window.demand_reserved, (size_t)0u);
        check_equal(window.results_ready, (size_t)0u);

        check_true(cflow_subscription_request(&run_fixture.run, 4u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);
        progressed = 0u;
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_equal(fixture.backend_request_count, (size_t)8u);
        for (size_t index = 4u; index < 8u; ++index) {
            check_equal(fixture.backend_requests[index].lease_id,
                        fixture.backend_requests[index - 4u].lease_id);
            check_equal(io_source_complete_backend_at(
                &fixture, index, &completion),
                CFLOW_IO_COMPLETE_ACCEPTED);
            progressed = 0u;
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
        }
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(run_fixture.sink_probe.values, (size_t)8u);
        check_equal(fixture.release_calls, (size_t)8u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.acknowledged, (uint64_t)8u);
        check_true(cflow_io_publisher_owner_get_window_stats(
            &run_fixture.owner, &window));
        check_equal(window.occupied, (size_t)0u);
        check_equal(window.demand_reserved, (size_t)0u);
        check_equal(window.results_ready, (size_t)0u);

        io_source_run_fixture_close(&run_fixture);
    }

    it("drains accepted window entries before prepare done terminates") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_values = {11, 22, 33},
            .encoded_value_count = 3u,
            .prepare_operation_limit = 3u,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init_windowed(
            &run_fixture, &fixture, 4u));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 4u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);
        check_equal(fixture.prepare_calls, (size_t)4u);
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_equal(fixture.backend_submit_calls, (size_t)3u);

        for (size_t index = 0u; index < 3u; ++index) {
            check_equal(io_source_complete_backend_at(
                &fixture, index, &completion),
                CFLOW_IO_COMPLETE_ACCEPTED);
            progressed = 0u;
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
        }
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(run_fixture.sink_probe.values, (size_t)3u);
        check_equal(run_fixture.sink_probe.received[0], 11);
        check_equal(run_fixture.sink_probe.received[1], 22);
        check_equal(run_fixture.sink_probe.received[2], 33);
        check_equal(run_fixture.sink_probe.done, (size_t)1u);
        check_equal(run_fixture.sink_probe.errors, (size_t)0u);
        check_true(cflow_subscription_is_done(&run_fixture.run));
        check_equal(fixture.release_calls, (size_t)3u);

        io_source_run_fixture_close(&run_fixture);
    }

    it("discards accepted window entries after prepare error") {
        static const char prepare_error[] = "window prepare failure";
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .prepare_error = prepare_error,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 99,
            .prepare_operation_limit = 2u,
            .prepare_error_at_limit = true,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_window_stats window = {0};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init_windowed(
            &run_fixture, &fixture, 4u));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 4u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(fixture.prepare_calls, (size_t)3u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(run_fixture.sink_probe.errors, (size_t)1u);
        check_true(run_fixture.sink_probe.error == prepare_error);

        while (!cflow_io_publisher_owner_is_quiescent(
                   &run_fixture.owner)) {
            progressed = 0u;
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
        }

        check_equal(fixture.encode_calls, (size_t)0u);
        check_equal(fixture.release_calls, (size_t)2u);
        check_true(cflow_io_publisher_owner_get_window_stats(
            &run_fixture.owner, &window));
        check_equal(window.occupied, (size_t)0u);
        check_equal(window.demand_reserved, (size_t)0u);
        check_equal(window.results_ready, (size_t)0u);

        io_source_run_fixture_close(&run_fixture);
    }

    it("cuts off later window completions after value and done") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE_AND_DONE,
            .encoded_value = 77,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        const cflow_io_completion completed = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        const cflow_io_completion cancelled = {
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init_windowed(
            &run_fixture, &fixture, 4u));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 4u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_equal(fixture.backend_submit_calls, (size_t)4u);

        check_equal(io_source_complete_backend_at(
            &fixture, 2u, &completed), CFLOW_IO_COMPLETE_ACCEPTED);
        progressed = 0u;
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(run_fixture.sink_probe.values, (size_t)1u);
        check_equal(run_fixture.sink_probe.received[0], 77);
        check_equal(run_fixture.sink_probe.done, (size_t)1u);
        check_equal(run_fixture.sink_probe.errors, (size_t)0u);
        check_true(cflow_subscription_is_done(&run_fixture.run));
        check_equal(fixture.encode_calls, (size_t)1u);

        cflow_subscription_close(&run_fixture.run);
        for (size_t index = 0u;
             index < fixture.backend_request_count; ++index) {
            if (fixture.backend_requests[index].active)
                check_equal(io_source_complete_backend_at(
                    &fixture, index, &cancelled),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        }
        while (!cflow_io_publisher_owner_is_quiescent(
                   &run_fixture.owner)) {
            progressed = 0u;
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
        }

        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)4u);
        check_equal(cflow_io_publisher_owner_close(
            &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
    }

    it("discards a ready window result when the run is cancelled") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 55,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_window_stats window = {0};
        const cflow_io_completion completed = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        const cflow_io_completion cancelled = {
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init_windowed(
            &run_fixture, &fixture, 4u));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 4u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);

        check_equal(io_source_complete_backend_at(
            &fixture, 0u, &completed), CFLOW_IO_COMPLETE_ACCEPTED);
        progressed = 0u;
        check_equal(cflow_io_publisher_owner_run_ready(
            &run_fixture.owner, 64u, &progressed), SALTS_OK);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_true(cflow_io_publisher_owner_get_window_stats(
            &run_fixture.owner, &window));
        check_equal(window.results_ready, (size_t)1u);

        cflow_subscription_close(&run_fixture.run);
        for (size_t index = 0u;
             index < fixture.backend_request_count; ++index) {
            if (fixture.backend_requests[index].active)
                check_equal(io_source_complete_backend_at(
                    &fixture, index, &cancelled),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        }
        while (!cflow_io_publisher_owner_is_quiescent(
                   &run_fixture.owner)) {
            progressed = 0u;
            check_equal(cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 64u, &progressed), SALTS_OK);
            check_true(progressed > (size_t)0u);
        }

        check_equal(run_fixture.sink_probe.values, (size_t)0u);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)4u);
        check_true(cflow_io_publisher_owner_get_window_stats(
            &run_fixture.owner, &window));
        check_equal(window.occupied, (size_t)0u);
        check_equal(window.demand_reserved, (size_t)0u);
        check_equal(window.results_ready, (size_t)0u);

        check_equal(cflow_io_publisher_owner_close(
            &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
    }

    it("delivers one authoritative completion as a typed value") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE_AND_DONE,
            .encoded_value = 37,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));

        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        check_equal(cflow_scheduler_run_until_idle(
                        &run_fixture.scheduler, 0u), (size_t)1u);
        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_true(fixture.drive_calls >= (size_t)1u);

        check_equal(cflow_io_publisher_owner_run_ready(
                        &run_fixture.owner, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        check_equal(cflow_scheduler_run_until_idle(
                        &run_fixture.scheduler, 0u), (size_t)1u);
        check_equal(run_fixture.sink_probe.values, (size_t)1u);
        check_equal(run_fixture.sink_probe.received[0], 37);
        check_equal(run_fixture.sink_probe.done, (size_t)1u);
        check_true(cflow_subscription_is_done(&run_fixture.run));
        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)1u);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.acknowledged, (uint64_t)1u);
        check_false(stats.request_active);

        io_source_run_fixture_close(&run_fixture);
        check_equal(fixture.release_calls, (size_t)1u);
    }

    it("wakes when synchronous completion precedes WAIT arm") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE_AND_DONE,
            .encoded_value = 37,
            .prepare_valid_operation = true,
            .complete_during_submit = true,
            .drive_owner_inline = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        fixture.drive_owner = &run_fixture.owner;
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));

        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(run_fixture.sink_probe.values, (size_t)1u);
        check_equal(run_fixture.sink_probe.received[0], 37);
        check_equal(run_fixture.sink_probe.done, (size_t)1u);
        check_true(cflow_subscription_is_done(&run_fixture.run));
        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)1u);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(fixture.drive_run_calls >= (size_t)1u);
        check_true(fixture.drive_run_progressed > (size_t)0u);
        check_equal(fixture.drive_run_busy, (size_t)0u);
        check_equal(fixture.drive_run_error, SALTS_OK);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.acknowledged, (uint64_t)1u);
        check_false(stats.request_active);

        io_source_run_fixture_close(&run_fixture);
        check_equal(fixture.release_calls, (size_t)1u);
    }

    it("preserves completion across the adapter driver tail window") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE_AND_DONE,
            .encoded_value = 61,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        io_source_tail_barrier barrier = {0};
        io_source_drive_context drive_context = {
            &run_fixture.owner, 1u, 0u, SALTS_EINVAL
        };
        const cflow_io_completion completed = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        cflow_executor *executor;
        cmeta_thread_t driver = NULL;
        size_t progressed = 0u;
        bool entered;

        cmeta_mutex_init(&barrier.lock);
        cmeta_cond_init(&barrier.changed);
        check_not_null(barrier.lock);
        check_not_null(barrier.changed);
        check_true(io_source_run_fixture_init_with_scheduler(
            &run_fixture, &fixture, true));
        fixture.drive_owner = &run_fixture.owner;
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 1u));

        check_equal(cflow_io_publisher_owner_run_ready(
                        &run_fixture.owner, 2u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)2u);
        check_equal(fixture.backend_submit_calls, (size_t)1u);
        check_equal(fixture.backend_active, (size_t)1u);

        executor = cflow_io_publisher_test_executor(&run_fixture.owner);
        check_not_null(executor);
        check_equal(cflow_executor_try_post(
                        executor, io_source_tail_barrier_task,
                        &barrier), CFLOW_ADMISSION_ACCEPTED);
        fixture.drive_owner_inline = true;
        check_equal(cmeta_thread_create(
                        &driver, io_source_drive_thread,
                        &drive_context), SALTS_OK);
        entered = io_source_tail_barrier_wait(&barrier);
        check_true(entered);
        if (entered) {
            check_equal(io_source_complete_backend(
                            &fixture, &completed),
                        CFLOW_IO_COMPLETE_ACCEPTED);
            check_equal(fixture.drive_run_busy, (size_t)0u);
            check_equal(fixture.drive_run_busy_progressed,
                        (size_t)0u);
        }
        io_source_tail_barrier_release(&barrier);
        check_equal(cmeta_thread_join(&driver), SALTS_OK);
        cmeta_thread_destroy(&driver);

        check_equal(drive_context.status, SALTS_OK);
        check_equal(drive_context.progressed, (size_t)1u);
        check_equal(run_fixture.sink_probe.values, (size_t)1u);
        check_equal(run_fixture.sink_probe.received[0], 61);
        check_equal(run_fixture.sink_probe.done, (size_t)1u);
        check_true(cflow_subscription_is_done(&run_fixture.run));
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(fixture.drive_run_progressed > (size_t)0u);

        cflow_subscription_close(&run_fixture.run);
        check_true(cflow_io_publisher_owner_is_quiescent(
            &run_fixture.owner));
        if (!cflow_io_publisher_owner_is_quiescent(&run_fixture.owner))
            (void)cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 32u, &progressed);
        check_equal(cflow_io_publisher_owner_close(
                        &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
        cmeta_cond_destroy(&barrier.changed);
        cmeta_mutex_destroy(&barrier.lock);
    }

    it("drains cancellation across the adapter driver tail window") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 67,
            .prepare_valid_operation = true
        };
        io_source_run_fixture run_fixture;
        io_source_tail_barrier barrier = {0};
        io_source_drive_context drive_context = {
            &run_fixture.owner, 1u, 0u, SALTS_EINVAL
        };
        const cflow_io_completion cancelled = {
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
        cflow_executor *executor;
        cmeta_thread_t driver = NULL;
        size_t progressed = 0u;
        bool entered;

        cmeta_mutex_init(&barrier.lock);
        cmeta_cond_init(&barrier.changed);
        check_not_null(barrier.lock);
        check_not_null(barrier.changed);
        check_true(io_source_run_fixture_init_with_scheduler(
            &run_fixture, &fixture, true));
        fixture.drive_owner = &run_fixture.owner;
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        check_equal(cflow_io_publisher_owner_run_ready(
                        &run_fixture.owner, 2u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)2u);
        check_equal(fixture.backend_submit_calls, (size_t)1u);

        executor = cflow_io_publisher_test_executor(&run_fixture.owner);
        check_not_null(executor);
        check_equal(cflow_executor_try_post(
                        executor, io_source_tail_barrier_task,
                        &barrier), CFLOW_ADMISSION_ACCEPTED);
        fixture.drive_owner_inline = true;
        fixture.close_owner_after_successful_drive = true;
        fixture.drive_close_status = SALTS_EINVAL;
        check_equal(cmeta_thread_create(
                        &driver, io_source_drive_thread,
                        &drive_context), SALTS_OK);
        entered = io_source_tail_barrier_wait(&barrier);
        check_true(entered);
        if (entered) {
            cflow_subscription_close(&run_fixture.run);
            check_equal(io_source_complete_backend(
                            &fixture, &cancelled),
                        CFLOW_IO_COMPLETE_ACCEPTED);
            check_equal(fixture.drive_run_busy, (size_t)0u);
            check_equal(fixture.drive_run_busy_progressed,
                        (size_t)0u);
        }
        io_source_tail_barrier_release(&barrier);
        check_equal(cmeta_thread_join(&driver), SALTS_OK);
        cmeta_thread_destroy(&driver);

        check_equal(drive_context.status, SALTS_OK);
        check_equal(drive_context.progressed, (size_t)1u);
        check_equal(run_fixture.sink_probe.values, (size_t)0u);
        check_equal(fixture.encode_calls, (size_t)0u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(fixture.drive_run_progressed > (size_t)0u);
        check_equal(fixture.drive_close_calls, (size_t)1u);
        check_equal(fixture.drive_close_status, SALTS_EBUSY);
        check_true(fixture.drive_close_owner_preserved);
        check_true(cflow_io_publisher_owner_is_quiescent(
            &run_fixture.owner));
        if (!cflow_io_publisher_owner_is_quiescent(&run_fixture.owner))
            (void)cflow_io_publisher_owner_run_ready(
                &run_fixture.owner, 32u, &progressed);
        check_equal(cflow_io_publisher_owner_close(
                        &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
        cmeta_cond_destroy(&barrier.changed);
        cmeta_mutex_destroy(&barrier.lock);
    }

    it("serializes two demanded values through ACK under inline scheduling") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_values = {11, 29},
            .encoded_value_count = 2u,
            .prepare_valid_operation = true,
            .complete_during_submit = true,
            .drive_owner_inline = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};

        check_true(io_source_run_fixture_init_with_scheduler(
            &run_fixture, &fixture, true));
        fixture.drive_owner = &run_fixture.owner;
        fixture.prepare_stats_owner = &run_fixture.owner;
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));

        check_true(cflow_subscription_request(&run_fixture.run, 2u));

        check_equal(run_fixture.sink_probe.values, (size_t)2u);
        check_equal(run_fixture.sink_probe.received[0], 11);
        check_equal(run_fixture.sink_probe.received[1], 29);
        check_equal(fixture.prepare_calls, (size_t)2u);
        check_equal(fixture.acknowledged_before_prepare[0], (uint64_t)0u);
        check_equal(fixture.acknowledged_before_prepare[1], (uint64_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)2u);
        check_equal(fixture.backend_active_max, (size_t)1u);
        check_equal(fixture.backend_active, (size_t)0u);
        check_equal(fixture.encode_calls, (size_t)2u);
        check_equal(fixture.release_calls, (size_t)2u);
        check_equal(fixture.drive_run_busy, (size_t)0u);
        check_equal(fixture.drive_run_busy_progressed, (size_t)0u);
        check_true(run_fixture.inline_scheduler.posts >= (size_t)1u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.accepted, (uint64_t)2u);
        check_equal(stats.actor.acknowledged, (uint64_t)2u);
        check_false(stats.request_active);

        io_source_run_fixture_close(&run_fixture);
        check_equal(fixture.release_calls, (size_t)2u);
    }

    it("waits for an extracted Source waker before close returns") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 41,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_stats close_stats = {0};
        cflow_step step;
        int output = 0;
        io_source_blocking_wake_probe wake = {0};
        io_source_close_context close_context = {&source, &wake};
        io_source_drive_context drive_context = {
            &owner, 32u, 0u, SALTS_EINVAL
        };
        cmeta_thread_t driver = NULL;
        cmeta_thread_t closer = NULL;
        size_t waits = 0u;
        bool stats_valid = true;

        cmeta_mutex_init(&wake.lock);
        cmeta_cond_init(&wake.changed);
        check_not_null(wake.lock);
        check_not_null(wake.changed);
        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable,
            (cflow_waker){io_source_blocking_wake, &wake}));
        check_equal(cmeta_thread_create(
                        &driver, io_source_drive_thread,
                        &drive_context), SALTS_OK);

        cmeta_mutex_lock(&wake.lock);
        while (!wake.entered && waits++ < IO_SOURCE_WAIT_LIMIT)
            (void)cmeta_cond_timedwait(
                &wake.changed, &wake.lock, IO_SOURCE_WAIT_SLICE_NS);
        if (!wake.entered) {
            wake.released = true;
            cmeta_cond_broadcast(&wake.changed);
        }
        check_true(wake.entered);
        cmeta_mutex_unlock(&wake.lock);

        check_equal(cmeta_thread_create(
                        &closer, io_source_close_thread,
                        &close_context), SALTS_OK);
        cmeta_mutex_lock(&wake.lock);
        waits = 0u;
        do {
            stats_valid = cflow_io_publisher_owner_get_stats(
                &owner, &close_stats);
            if (!stats_valid || close_stats.close_requested)
                break;
            (void)cmeta_cond_timedwait(
                &wake.changed, &wake.lock,
                IO_SOURCE_WAIT_SLICE_NS);
        } while (++waits < IO_SOURCE_WAIT_LIMIT);
        check_true(stats_valid);
        check_true(close_stats.close_requested);

        if (!wake.close_returned)
            (void)cmeta_cond_timedwait(
                &wake.changed, &wake.lock,
                IO_SOURCE_CANCEL_OBSERVATION_NS);
        check_false(wake.close_returned);
        check_true(cflow_io_publisher_owner_get_stats(
            &owner, &close_stats));
        check_true(close_stats.close_requested);
        check_true(close_stats.publisher_live);
        wake.released = true;
        cmeta_cond_broadcast(&wake.changed);
        cmeta_mutex_unlock(&wake.lock);

        check_equal(cmeta_thread_join(&driver), SALTS_OK);
        cmeta_thread_destroy(&driver);
        check_equal(cmeta_thread_join(&closer), SALTS_OK);
        cmeta_thread_destroy(&closer);
        check_equal(drive_context.status, SALTS_OK);
        check_true(drive_context.progressed > (size_t)0u);
        check_true(wake.close_returned);
        check_true(cflow_io_publisher_owner_is_quiescent(&owner));
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        cmeta_cond_destroy(&wake.changed);
        cmeta_mutex_destroy(&wake.lock);
    }

    it("allows Source cancel to return from inside its own waker") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 43,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        io_source_reentrant_cancel_probe wake = {&source, false};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable,
            (cflow_waker){io_source_reentrant_cancel, &wake}));
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        check_true(wake.returned);
        check_true(progressed > (size_t)0u);
        cflow_publisher_destroy(&source);
        check_true(cflow_io_publisher_owner_is_quiescent(&owner));
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("keeps A owner alive while nested adapter wakers unwind") {
        io_source_fixture fixture_a = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 53,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        io_source_fixture fixture_b = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 59,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config_a = io_source_config(&fixture_a);
        cflow_io_publisher_config config_b = io_source_config(&fixture_b);
        cflow_publisher source_a = {0};
        cflow_publisher source_b = {0};
        cflow_io_publisher_owner owner_a = {0};
        cflow_io_publisher_owner owner_b = {0};
        cflow_step step_a;
        cflow_step step_b;
        io_source_nested_wake_probe probe = {0};
        cmeta_thread_t worker = NULL;
        size_t progressed = 0u;
        size_t waits = 0u;
        bool worker_completed;
        int output_a = 0;
        int output_b = 0;

        cmeta_mutex_init(&probe.lock);
        cmeta_cond_init(&probe.changed);
        check_not_null(probe.lock);
        check_not_null(probe.changed);
        check_equal(cflow_publisher_from_io_actor(
                        &source_a, &owner_a, &config_a), SALTS_OK);
        check_equal(cflow_publisher_from_io_actor(
                        &source_b, &owner_b, &config_b), SALTS_OK);
        step_a = cflow_publisher_resume(&source_a, NULL, &output_a);
        step_b = cflow_publisher_resume(&source_b, NULL, &output_b);
        check_equal(step_a.kind, CFLOW_STEP_WAIT);
        check_equal(step_b.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner_a, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner_b, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);

        probe.source_a = &source_a;
        probe.owner_a = &owner_a;
        probe.waitable_a = step_a.waitable;
        probe.waitable_b = step_b.waitable;
        probe.owner_a_close_status = SALTS_EINVAL;
        check_equal(cmeta_thread_create(
                        &worker, io_source_nested_arm_thread,
                        &probe), SALTS_OK);
        cmeta_mutex_lock(&probe.lock);
        while (!probe.worker_completed &&
               waits++ < IO_SOURCE_NESTED_WAIT_LIMIT)
            (void)cmeta_cond_timedwait(
                &probe.changed, &probe.lock,
                IO_SOURCE_WAIT_SLICE_NS);
        worker_completed = probe.worker_completed;
        cmeta_mutex_unlock(&probe.lock);

        if (!worker_completed) {
            fprintf(stderr,
                    "fatal: nested IO Source waker worker did not "
                    "quiesce before watchdog timeout\n");
            fflush(stderr);
            abort();
        }
        check_equal(cmeta_thread_join(&worker), SALTS_OK);
        cmeta_thread_destroy(&worker);
        check_true(probe.arm_a_accepted);
        check_true(probe.arm_b_accepted);
        check_true(probe.destroy_a_returned);
        check_true(probe.outer_a_returned);
        check_false(probe.rearm_a_accepted);
        check_equal(probe.wake_a_calls, (size_t)1u);
        check_equal(probe.wake_b_calls, (size_t)1u);
        check_equal(probe.unexpected_a_calls, (size_t)0u);
        check_equal(probe.owner_a_close_status, SALTS_EBUSY);
        check_true(cflow_io_publisher_owner_is_quiescent(&owner_a));
        check_equal(cflow_io_publisher_owner_close(
                        &owner_a), SALTS_OK);
        cflow_publisher_destroy(&source_b);
        check_true(cflow_io_publisher_owner_is_quiescent(&owner_b));
        check_equal(cflow_io_publisher_owner_close(
                        &owner_b), SALTS_OK);
        cmeta_cond_destroy(&probe.changed);
        cmeta_mutex_destroy(&probe.lock);
    }

    it("bounds owner progress and permits VALUE consumption before ACK") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 47,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_stats stats = {0};
        cflow_step step;
        size_t progressed = SIZE_MAX;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);

        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)1u);
        check_equal(fixture.encode_calls, (size_t)0u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(fixture.encode_calls, (size_t)0u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_true(stats.result_ready);
        check_true(stats.request_active);

        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE);
        check_equal(output, 47);
        check_equal(fixture.release_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_false(stats.result_ready);
        check_true(stats.request_active);

        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)0u);

        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)0u);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("cancels before backend submit and retries owner close after drain") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 71,
            .prepare_valid_operation = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_stats stats = {0};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_EBUSY);

        cflow_publisher_cancel(&source);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_true(stats.publisher_live);
        check_true(stats.close_requested);
        check_equal(stats.actor.lifecycle, CFLOW_IO_CLOSING);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_EBUSY);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_EBUSY);

        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(fixture.backend_cancel_calls, (size_t)0u);
        check_equal(fixture.encode_calls, (size_t)0u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_false(stats.publisher_live);
        check_false(stats.request_active);
        check_equal(stats.actor.accepted, (uint64_t)1u);
        check_equal(stats.actor.acknowledged, (uint64_t)1u);
        check_true(cflow_io_publisher_owner_is_quiescent(&owner));
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        check_null(owner.impl);
        check_equal(fixture.release_calls, (size_t)1u);
    }

    it("cancels submitted native work and waits for authoritative completion") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE,
            .encoded_value = 73,
            .prepare_valid_operation = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_stats stats = {0};
        const cflow_io_completion cancelled = {
            CFLOW_IO_COMPLETION_CANCELLED, 0u, SALTS_OK};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 1u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)1u);
        check_equal(fixture.backend_active, (size_t)1u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_equal(stats.actor.backend_pending, (size_t)1u);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_EBUSY);

        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_EBUSY);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        check_equal(fixture.backend_cancel_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)0u);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_EBUSY);

        check_equal(io_source_complete_backend(
                        &fixture, &cancelled),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        check_equal(fixture.backend_active, (size_t)0u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        check_equal(fixture.encode_calls, (size_t)0u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_false(stats.request_active);
        check_equal(stats.actor.acknowledged, (uint64_t)1u);
        check_true(cflow_io_publisher_owner_is_quiescent(&owner));
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        check_null(owner.impl);
        check_equal(fixture.backend_submit_calls, (size_t)1u);
        check_equal(fixture.backend_cancel_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
    }

    it("keeps duplicate backend completion stale and emits one final value") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_VALUE_AND_DONE,
            .encoded_value = 79,
            .prepare_valid_operation = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_stats stats = {0};
        const cflow_io_completion completed = {
            CFLOW_IO_COMPLETION_OK, sizeof(int), SALTS_OK};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 2u, &progressed), SALTS_OK);
        check_equal(progressed, (size_t)2u);
        check_equal(io_source_complete_backend(
                        &fixture, &completed),
                    CFLOW_IO_COMPLETE_ACCEPTED);
        check_equal(io_source_complete_backend(
                        &fixture, &completed),
                    CFLOW_IO_COMPLETE_NOT_PENDING);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);

        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 79);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_DONE);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_equal(stats.actor.stale_completions, (uint64_t)1u);
        check_equal(stats.actor.acknowledged, (uint64_t)1u);
        check_false(stats.request_active);

        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        check_equal(fixture.release_calls, (size_t)1u);
    }

    it("maps encoder DONE to Source completion") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_DONE,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_DONE);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("preserves the stable encoder ERROR") {
        static const char encode_error[] = "literal encode failure";
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_ERROR,
            .encode_error = encode_error,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_true(step.error == encode_error);
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("maps encoder WOULD_BLOCK to its protocol error") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = CFLOW_READ_WOULD_BLOCK,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_equal(step.error,
                    "IO source completion encoder returned WOULD_BLOCK");
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("distinguishes an invalid encoder status") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .encode_status = (cflow_read_status)99,
            .prepare_valid_operation = true,
            .complete_during_submit = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_step step;
        size_t progressed = 0u;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &owner, 32u, &progressed), SALTS_OK);
        step = cflow_publisher_resume(&source, NULL, &output);
        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_equal(step.error,
                    "IO source completion encoder returned an invalid status");
        check_equal(fixture.encode_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        cflow_publisher_destroy(&source);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
    }

    it("reports an accepted operation active during Actor admission transfer") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .prepare_valid_operation = true,
            .capture_stats_on_drive = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};
        size_t progressed = 0u;

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        fixture.stats_owner = &run_fixture.owner;
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_true(fixture.drive_stats_captured);
        check_equal(fixture.drive_stats.actor.accepted, (uint64_t)1u);
        check_equal(fixture.drive_stats.actor.active_requests, (size_t)1u);
        check_true(fixture.drive_stats.request_active);
        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(fixture.release_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.accepted, (uint64_t)1u);
        check_equal(stats.actor.active_requests, (size_t)1u);
        check_true(stats.request_active);

        cflow_subscription_close(&run_fixture.run);
        check_equal(fixture.release_calls, (size_t)0u);
        check_equal(cflow_io_publisher_owner_run_ready(
                        &run_fixture.owner, 32u, &progressed), SALTS_OK);
        check_true(progressed > (size_t)0u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_true(cflow_io_publisher_owner_is_quiescent(
            &run_fixture.owner));
        check_equal(cflow_io_publisher_owner_close(
                        &run_fixture.owner), SALTS_OK);
        cflow_scheduler_destroy(&run_fixture.scheduler);
        cflow_graph_destroy(&run_fixture.normalized);
        cflow_graph_destroy(&run_fixture.surface);
    }

    it("releases an adapter-owned operation once when Actor admission rejects") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .prepare_valid_operation = true
        };
        cflow_io_publisher_config config = io_source_config(&fixture);
        cflow_publisher source = {0};
        cflow_io_publisher_owner owner = {0};
        cflow_io_publisher_stats stats = {0};
        cflow_step step;
        int output = 0;

        check_equal(cflow_publisher_from_io_actor(
                        &source, &owner, &config), SALTS_OK);
        fixture.cancel_source_during_prepare = &source;
        step = cflow_publisher_resume(&source, NULL, &output);

        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.release_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(&owner, &stats));
        check_equal(stats.actor.accepted, (uint64_t)0u);
        check_equal(stats.actor.rejected_closed, (uint64_t)1u);
        check_false(stats.request_active);
        check_true(stats.close_requested);

        cflow_publisher_destroy(&source);
        check_equal(fixture.release_calls, (size_t)1u);
        check_equal(cflow_io_publisher_owner_close(&owner), SALTS_OK);
        check_equal(fixture.release_calls, (size_t)1u);
    }

    it("maps prepare DONE to one terminal sink notification") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_DONE
        };
        io_source_run_fixture run_fixture;

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(run_fixture.sink_probe.values, (size_t)0u);
        check_equal(run_fixture.sink_probe.errors, (size_t)0u);
        check_equal(run_fixture.sink_probe.done, (size_t)1u);
        check_true(cflow_subscription_is_done(&run_fixture.run));
        {
            cflow_io_publisher_stats stats = {0};
            check_true(cflow_io_publisher_owner_get_stats(
                &run_fixture.owner, &stats));
            check_equal(stats.actor.accepted, (uint64_t)0u);
            check_false(stats.request_active);
        }

        io_source_run_fixture_close(&run_fixture);
    }

    it("maps prepare ERROR to the stable callback error") {
        static const char prepare_error[] = "literal prepare failure";
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_ERROR,
            .prepare_error = prepare_error
        };
        io_source_run_fixture run_fixture;

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(run_fixture.sink_probe.values, (size_t)0u);
        check_equal(run_fixture.sink_probe.errors, (size_t)1u);
        check_equal(run_fixture.sink_probe.done, (size_t)0u);
        check_true(run_fixture.sink_probe.error == prepare_error);
        check_true(cflow_subscription_error(&run_fixture.run) == prepare_error);
        {
            cflow_io_publisher_stats stats = {0};
            check_true(cflow_io_publisher_owner_get_stats(
                &run_fixture.owner, &stats));
            check_equal(stats.actor.accepted, (uint64_t)0u);
            check_false(stats.request_active);
        }

        io_source_run_fixture_close(&run_fixture);
    }

    it("rejects an empty operation token without creating a release obligation") {
        io_source_fixture fixture = {
            .prepare_status = CFLOW_IO_PUBLISHER_PREPARE_OPERATION,
            .prepare_empty_operation = true
        };
        io_source_run_fixture run_fixture;
        cflow_io_publisher_stats stats = {0};

        check_true(io_source_run_fixture_init(&run_fixture, &fixture));
        check_true(cflow_subscribe(
            &run_fixture.run, &run_fixture.normalized,
            &run_fixture.source, &run_fixture.scheduler,
            &run_fixture.sink));
        check_true(cflow_subscription_request(&run_fixture.run, 1u));
        (void)cflow_scheduler_run_until_idle(
            &run_fixture.scheduler, 0u);

        check_equal(fixture.prepare_calls, (size_t)1u);
        check_equal(fixture.backend_submit_calls, (size_t)0u);
        check_equal(run_fixture.sink_probe.errors, (size_t)1u);
        check_equal(run_fixture.sink_probe.error,
                    "IO source operation is missing a release callback");
        check_equal(fixture.release_calls, (size_t)0u);
        check_true(cflow_io_publisher_owner_get_stats(
            &run_fixture.owner, &stats));
        check_equal(stats.actor.accepted, (uint64_t)0u);
        check_false(stats.request_active);

        io_source_run_fixture_close(&run_fixture);
        check_equal(fixture.release_calls, (size_t)0u);
    }
}
