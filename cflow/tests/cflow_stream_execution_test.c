#include <cflow/cflow.h>
#include <thread.h>

#include "subscription_internal.h"
#include "tinytest.h"

#include <stdatomic.h>
#include <string.h>

typedef struct execution_range_owner {
    const int *values;
    size_t count;
    cflow_stream_execution *execution;
    bool invoke_controls;
    cflow_stream_execution_status wait_status;
    cflow_stream_execution_status cancel_status;
    cflow_stream_execution_status destroy_status;
} execution_range_owner;

typedef struct execution_collector_state {
    int pending[8];
    int committed[8];
    size_t pending_count;
    size_t committed_count;
    size_t abort_count;
    atomic_int accept_entered;
    atomic_int accept_gate;
    bool block_accept;
    cflow_stream_execution *execution;
    cflow_stream_execution_status wait_in_callback;
    cflow_stream_execution_status cancel_in_callback;
    cflow_stream_execution_status destroy_in_callback;
} execution_collector_state;

typedef struct execution_cancel_args {
    cflow_stream_execution *execution;
    atomic_int entered;
    cflow_stream_execution_status status;
} execution_cancel_args;

typedef struct execution_destroy_release_args {
    execution_collector_state *collector;
    atomic_int destroy_entered;
    atomic_int destroy_returned;
    atomic_int observed_pending;
} execution_destroy_release_args;

typedef struct execution_destroy_probe_source {
    cflow_subscription *run;
    bool active_during_destroy;
} execution_destroy_probe_source;

typedef struct execution_destroy_probe_sink {
    atomic_int done;
} execution_destroy_probe_sink;

static cflow_stream_execution *execution_operator_control;
static cflow_stream_execution_status execution_operator_wait_status;
static cflow_stream_execution_status execution_operator_cancel_status;
static cflow_stream_execution_status execution_operator_destroy_status;

static const char *execution_destroy_probe_name(void *state) {
    (void)state;
    return "execution_destroy_probe";
}

static const cmeta_type_desc *execution_destroy_probe_type(void *state) {
    (void)state;
    return &cmeta_type_int;
}

static cflow_step execution_destroy_probe_resume(
    void *state, cflow_publish_context *ctx, void *out_value) {
    (void)state;
    (void)ctx;
    (void)out_value;
    return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
}

static void execution_destroy_probe_cancel(void *state) {
    (void)state;
}

static void execution_destroy_probe_destroy(void *state) {
    execution_destroy_probe_source *source =
        (execution_destroy_probe_source *)state;
    if (source != NULL)
        source->active_during_destroy =
            cflow_subscription_active_on_current_thread(source->run);
}

static void execution_destroy_probe_bind(void *state, cflow_waker waker) {
    (void)state;
    (void)waker;
}

static cflow_publisher_terminal execution_destroy_probe_poll(
    void *state, const char **error) {
    (void)state;
    if (error != NULL) *error = NULL;
    return CFLOW_PUBLISHER_OPEN;
}

CMETA_IMPLEMENTS(cflow_publisher, execution_destroy_probe, 0,
    .name = execution_destroy_probe_name,
    .output_type = execution_destroy_probe_type,
    .resume = execution_destroy_probe_resume,
    .cancel = execution_destroy_probe_cancel,
    .destroy = execution_destroy_probe_destroy,
    .bind_terminal_waker = execution_destroy_probe_bind,
    .poll_terminal = execution_destroy_probe_poll
);

static bool execution_destroy_probe_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    (void)user;
    (void)type;
    (void)value;
    return true;
}

static void execution_destroy_probe_error(void *user, const char *message) {
    (void)user;
    (void)message;
}

static void execution_destroy_probe_done(void *user) {
    execution_destroy_probe_sink *sink =
        (execution_destroy_probe_sink *)user;
    if (sink != NULL) atomic_store(&sink->done, 1);
}

cmeta_function(filter, value, bool, execution_control_filter, (int value)) {
    (void)value;
    execution_operator_wait_status =
        cflow_stream_execution_wait(execution_operator_control);
    execution_operator_cancel_status =
        cflow_stream_execution_cancel(execution_operator_control);
    execution_operator_destroy_status =
        cflow_stream_execution_destroy(execution_operator_control);
    return true;
}

static cmeta_gen_status execution_range_next(
    const void *object, cmeta_range_cursor *cursor, void *out_value) {
    const execution_range_owner *owner =
        (const execution_range_owner *)object;
    if (cursor->index >= owner->count)
        return CMETA_GEN_DONE;
    if (cursor->index == 0u && owner->invoke_controls) {
        execution_range_owner *mutable_owner =
            (execution_range_owner *)object;
        mutable_owner->wait_status =
            cflow_stream_execution_wait(owner->execution);
        mutable_owner->cancel_status =
            cflow_stream_execution_cancel(owner->execution);
        mutable_owner->destroy_status =
            cflow_stream_execution_destroy(owner->execution);
    }
    *(int *)out_value = owner->values[cursor->index++];
    return cursor->index == owner->count
        ? CMETA_GEN_VALUE_AND_DONE : CMETA_GEN_VALUE;
}

static cmeta_range execution_range(execution_range_owner *owner) {
    cmeta_range range = {
        owner,
        &cmeta_type_int,
        CMETA_RANGE_SIZED | CMETA_RANGE_ORDERED | CMETA_RANGE_REUSABLE,
        NULL,
        execution_range_next,
        0u,
        NULL
    };
    return range;
}

static cmeta_status execution_collector_begin(
    void *context, const cmeta_type_desc *input, size_t limit) {
    execution_collector_state *state =
        (execution_collector_state *)context;
    if (!cmeta_type_equal(input, &cmeta_type_int) || limit > 8u)
        return CMETA_INVALID_ARGUMENT;
    state->pending_count = 0u;
    state->committed_count = 0u;
    memset(state->pending, 0, sizeof(state->pending));
    memset(state->committed, 0, sizeof(state->committed));
    return CMETA_OK;
}

static cmeta_status execution_collector_accept(void *context,
                                                const void *value) {
    execution_collector_state *state =
        (execution_collector_state *)context;
    if (state->execution != NULL) {
        state->wait_in_callback =
            cflow_stream_execution_wait(state->execution);
        state->cancel_in_callback =
            cflow_stream_execution_cancel(state->execution);
        state->destroy_in_callback =
            cflow_stream_execution_destroy(state->execution);
    }
    atomic_store(&state->accept_entered, 1);
    while (state->block_accept && !atomic_load(&state->accept_gate))
        cmeta_sleep_ms(1u);
    state->pending[state->pending_count++] = *(const int *)value;
    return CMETA_OK;
}

static cmeta_status execution_collector_finish(void *context) {
    execution_collector_state *state =
        (execution_collector_state *)context;
    memcpy(state->committed, state->pending,
           state->pending_count * sizeof(state->pending[0]));
    state->committed_count = state->pending_count;
    return CMETA_OK;
}

static void execution_collector_abort(void *context) {
    execution_collector_state *state =
        (execution_collector_state *)context;
    ++state->abort_count;
    state->pending_count = 0u;
    state->committed_count = 0u;
    memset(state->committed, 0, sizeof(state->committed));
}

static const cmeta_collector_ops execution_collector_ops = {
    execution_collector_begin,
    execution_collector_accept,
    execution_collector_finish,
    execution_collector_abort
};

static cmeta_collector execution_collector(execution_collector_state *state,
                                            size_t limit) {
    cmeta_collector collector = {
        &execution_collector_ops,
        state,
        state->committed,
        &cmeta_type_int,
        limit,
        0u,
        CMETA_COLLECTOR_ZERO,
        CMETA_OK
    };
    return collector;
}

static bool execution_stream_init(cflow_stream *stream,
                                  execution_range_owner *owner) {
    return cflow_stream_from_range(stream, execution_range(owner)) != NULL;
}

static void execution_cancel_thread(void *user) {
    execution_cancel_args *args = (execution_cancel_args *)user;
    atomic_store(&args->entered, 1);
    args->status = cflow_stream_execution_cancel(args->execution);
}

static void execution_destroy_release_thread(void *user) {
    execution_destroy_release_args *args =
        (execution_destroy_release_args *)user;
    while (!atomic_load(&args->destroy_entered))
        cmeta_sleep_ms(1u);
    cmeta_sleep_ms(50u);
    atomic_store(&args->observed_pending,
                 !atomic_load(&args->destroy_returned));
    atomic_store(&args->collector->accept_gate, 1);
}

static void execution_noop_task(void *user) {
    (void)user;
}

suite("CFlow Stream execution") {
    it("marks Run destruction callbacks as active control contexts") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler workers = {0};
        cflow_subscription run = {0};
        execution_destroy_probe_source source_state = {&run, false};
        execution_destroy_probe_sink sink_state = {0};
        cflow_publisher source =
            execution_destroy_probe_as_cflow_publisher(&source_state);
        cflow_subscriber_callbacks callbacks = {
            execution_destroy_probe_value,
            execution_destroy_probe_error,
            execution_destroy_probe_done,
            &sink_state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &workers, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        check_true(cflow_scheduler_wait_idle(&workers));
        check_equal(atomic_load(&sink_state.done), 1);

        cflow_subscription_close(&run);
        check_true(source_state.active_during_destroy);

        cflow_scheduler_destroy(&workers);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("rejects invalid admission without publishing a live handle") {
        const int values[] = {1, 2};
        execution_range_owner owner = {values, 2u};
        execution_collector_state collector_state = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_scheduler manual = {0};
        cflow_scheduler workers = {0};
        cmeta_collector wrong = execution_collector(&collector_state, 2u);

        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_test_init(&manual));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &manual, wrong),
                    CFLOW_STREAM_EXECUTION_INVALID_SCHEDULER);
        check_null(execution.impl);
        check_equal(cflow_stream_execution_destroy(&execution),
                    CFLOW_STREAM_EXECUTION_OK);

        wrong.input_type = &cmeta_type_long;
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers, wrong),
                    CFLOW_STREAM_EXECUTION_COLLECTOR_REJECTED);
        check_null(execution.impl);

        cflow_scheduler_destroy(&workers);
        cflow_scheduler_destroy(&manual);
        cflow_stream_destroy(&stream);
    }

    it("rolls back when the worker scheduler rejects demand") {
        const int values[] = {1};
        execution_range_owner owner = {values, 1u};
        execution_collector_state collector_state = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_scheduler workers = {0};
        cflow_task_id delayed_task;

        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_worker_init_with_capacity(
            &workers, 1u, 1u, 1u));
        delayed_task = cflow_scheduler_post_after(
            &workers, 10000u, execution_noop_task, NULL);
        check_true(delayed_task != 0u);

        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 1u)),
                    CFLOW_STREAM_EXECUTION_DEMAND_REJECTED);
        check_null(execution.impl);
        check_equal(collector_state.abort_count, (size_t)1u);
        check_equal(collector_state.committed_count, (size_t)0u);

        check_true(cflow_scheduler_cancel(&workers, delayed_task));
        check_true(cflow_scheduler_wait_idle(&workers));
        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("collects one Stream asynchronously on a worker scheduler") {
        const int values[] = {2, 4, 6};
        execution_range_owner owner = {values, 3u};
        execution_collector_state collector_state = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_stream_execution_snapshot snapshot = {0};
        cflow_scheduler workers = {0};

        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_worker_init(&workers, 2u));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 3u)),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_wait(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        check_true(cflow_stream_execution_get_snapshot(&execution, &snapshot));
        check_equal(snapshot.state, CFLOW_STREAM_EXECUTION_COMPLETED);
        check_equal(snapshot.collector_status, CMETA_OK);
        check_equal(snapshot.count, (size_t)3u);
        check_null(snapshot.error);
        check_equal(cflow_stream_execution_cancel(&execution),
                    CFLOW_STREAM_EXECUTION_TERMINATED);
        check_equal(cflow_stream_execution_destroy(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(collector_state.committed_count, (size_t)3u);
        check_equal(collector_state.committed[0], 2);
        check_equal(collector_state.committed[1], 4);
        check_equal(collector_state.committed[2], 6);
        check_equal(collector_state.abort_count, (size_t)0u);

        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("rejects control calls from a Range callback on the active Run") {
        const int values[] = {7};
        execution_range_owner owner = {values, 1u};
        execution_collector_state collector_state = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_stream_execution_snapshot snapshot = {0};
        cflow_scheduler workers = {0};

        owner.execution = &execution;
        owner.invoke_controls = true;
        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 1u)),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_wait(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(owner.wait_status,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_equal(owner.cancel_status,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_equal(owner.destroy_status,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_true(cflow_stream_execution_get_snapshot(&execution, &snapshot));
        check_equal(snapshot.state, CFLOW_STREAM_EXECUTION_COMPLETED);

        check_equal(cflow_stream_execution_destroy(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("rejects control calls from an operator callback on the active Run") {
        const int values[] = {8};
        execution_range_owner owner = {values, 1u};
        execution_collector_state collector_state = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_stream_execution_snapshot snapshot = {0};
        cflow_scheduler workers = {0};

        check_true(execution_stream_init(&stream, &owner));
        check_not_null(stream.filter(&stream, execution_control_filter));
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        execution_operator_control = &execution;
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 1u)),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_wait(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        execution_operator_control = NULL;
        check_equal(execution_operator_wait_status,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_equal(execution_operator_cancel_status,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_equal(execution_operator_destroy_status,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_true(cflow_stream_execution_get_snapshot(&execution, &snapshot));
        check_equal(snapshot.state, CFLOW_STREAM_EXECUTION_COMPLETED);

        check_equal(cflow_stream_execution_destroy(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("aborts the Collector transaction when its hard limit is exceeded") {
        const int values[] = {1, 2};
        execution_range_owner owner = {values, 2u};
        execution_collector_state collector_state = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_stream_execution_snapshot snapshot = {0};
        cflow_scheduler workers = {0};

        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 1u)),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_wait(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        check_true(cflow_stream_execution_get_snapshot(&execution, &snapshot));
        check_equal(snapshot.state, CFLOW_STREAM_EXECUTION_FAILED);
        check_equal(snapshot.collector_status, CMETA_CAPACITY_EXCEEDED);
        check_equal(snapshot.count, (size_t)1u);
        check_equal(cflow_stream_execution_destroy(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(collector_state.committed_count, (size_t)0u);
        check_equal(collector_state.abort_count, (size_t)1u);

        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("closes synchronously on cancel and rejects control calls in callbacks") {
        const int values[] = {9, 10};
        execution_range_owner owner = {values, 2u};
        execution_collector_state collector_state = {0};
        execution_cancel_args cancel_args = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_stream_execution_snapshot snapshot = {0};
        cflow_scheduler workers = {0};
        cmeta_thread_t cancel_thread = NULL;

        collector_state.block_accept = true;
        collector_state.execution = &execution;
        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 2u)),
                    CFLOW_STREAM_EXECUTION_OK);
        while (!atomic_load(&collector_state.accept_entered))
            cmeta_sleep_ms(1u);
        check_equal(collector_state.wait_in_callback,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_equal(collector_state.cancel_in_callback,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);
        check_equal(collector_state.destroy_in_callback,
                    CFLOW_STREAM_EXECUTION_WOULD_BLOCK);

        cancel_args.execution = &execution;
        check_equal(cmeta_thread_create(
                        &cancel_thread, execution_cancel_thread, &cancel_args),
                    0);
        while (!atomic_load(&cancel_args.entered))
            cmeta_sleep_ms(1u);
        atomic_store(&collector_state.accept_gate, 1);
        check_equal(cmeta_thread_join(&cancel_thread), 0);
        check_true(cancel_args.status == CFLOW_STREAM_EXECUTION_OK ||
                   cancel_args.status == CFLOW_STREAM_EXECUTION_TERMINATED);
        check_true(cflow_stream_execution_get_snapshot(&execution, &snapshot));
        if (cancel_args.status == CFLOW_STREAM_EXECUTION_OK)
            check_equal(snapshot.state, CFLOW_STREAM_EXECUTION_CANCELLED);
        else
            check_equal(snapshot.state, CFLOW_STREAM_EXECUTION_COMPLETED);
        check_equal(cflow_stream_execution_destroy(&execution),
                    CFLOW_STREAM_EXECUTION_OK);
        if (cancel_args.status == CFLOW_STREAM_EXECUTION_OK) {
            check_equal(collector_state.committed_count, (size_t)0u);
            check_equal(collector_state.abort_count, (size_t)1u);
        } else {
            check_equal(collector_state.committed_count, (size_t)2u);
            check_equal(collector_state.abort_count, (size_t)0u);
        }

        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("does not return from destroy while a callback is active") {
        const int values[] = {11, 12};
        execution_range_owner owner = {values, 2u};
        execution_collector_state collector_state = {0};
        execution_destroy_release_args release_args = {0};
        cflow_stream stream = {0};
        cflow_stream_execution execution = {0};
        cflow_scheduler workers = {0};
        cmeta_thread_t release_thread = NULL;
        cflow_stream_execution_status destroy_status;

        collector_state.block_accept = true;
        release_args.collector = &collector_state;
        atomic_init(&release_args.destroy_entered, 0);
        atomic_init(&release_args.destroy_returned, 0);
        atomic_init(&release_args.observed_pending, 0);
        check_true(execution_stream_init(&stream, &owner));
        check_true(cflow_scheduler_worker_init(&workers, 1u));
        check_equal(cflow_stream_execution_start(
                        &execution, &stream, &workers,
                        execution_collector(&collector_state, 2u)),
                    CFLOW_STREAM_EXECUTION_OK);
        while (!atomic_load(&collector_state.accept_entered))
            cmeta_sleep_ms(1u);
        check_equal(cmeta_thread_create(
                        &release_thread,
                        execution_destroy_release_thread,
                        &release_args),
                    0);

        atomic_store(&release_args.destroy_entered, 1);
        destroy_status = cflow_stream_execution_destroy(&execution);
        atomic_store(&release_args.destroy_returned, 1);
        check_equal(cmeta_thread_join(&release_thread), 0);

        check_true(atomic_load(&release_args.observed_pending));
        check_equal(destroy_status, CFLOW_STREAM_EXECUTION_OK);
        check_null(execution.impl);
        check_equal(collector_state.committed_count, (size_t)0u);
        check_equal(collector_state.abort_count, (size_t)1u);

        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&stream);
    }

    it("runs independent handles through the same worker scheduler") {
        const int first_values[] = {1, 3};
        const int second_values[] = {2, 4};
        execution_range_owner first_owner = {first_values, 2u};
        execution_range_owner second_owner = {second_values, 2u};
        execution_collector_state first_output = {0};
        execution_collector_state second_output = {0};
        cflow_stream first_stream = {0};
        cflow_stream second_stream = {0};
        cflow_stream_execution first = {0};
        cflow_stream_execution second = {0};
        cflow_scheduler workers = {0};

        check_true(execution_stream_init(&first_stream, &first_owner));
        check_true(execution_stream_init(&second_stream, &second_owner));
        check_true(cflow_scheduler_worker_init(&workers, 2u));
        check_equal(cflow_stream_execution_start(
                        &first, &first_stream, &workers,
                        execution_collector(&first_output, 2u)),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_start(
                        &second, &second_stream, &workers,
                        execution_collector(&second_output, 2u)),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_wait(&first),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_wait(&second),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_destroy(&second),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(cflow_stream_execution_destroy(&first),
                    CFLOW_STREAM_EXECUTION_OK);
        check_equal(first_output.committed[0], 1);
        check_equal(first_output.committed[1], 3);
        check_equal(second_output.committed[0], 2);
        check_equal(second_output.committed[1], 4);

        cflow_scheduler_destroy(&workers);
        cflow_stream_destroy(&second_stream);
        cflow_stream_destroy(&first_stream);
    }
}
