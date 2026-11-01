#include <cflow/stream_execution.h>

#include <cflow/lower.h>
#include <cflow/reactive.h>
#include <cflow/publishers.h>
#include <thread.h>

#include "publishers_internal.h"
#include "subscription_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct cflow_stream_execution_impl {
    cmeta_mutex_t gate;
    cmeta_cond_t changed;
    cflow_stream_execution_state state;
    cmeta_status collector_status;
    size_t count;
    cflow_graph graph;
    cflow_subscription run;
    cflow_scheduler *scheduler;
    cmeta_collector collector;
    cflow_subscriber_callbacks callbacks;
    char *error;
} cflow_stream_execution_impl;

static SALTS_THREAD_LOCAL cflow_stream_execution_impl
    *active_stream_execution = NULL;

static const char stream_execution_generic_error[] =
    "stream execution failed";

static char *stream_execution_copy_error(const char *message) {
    const char *source = message != NULL && message[0] != '\0'
        ? message : stream_execution_generic_error;
    size_t length = strlen(source);
    char *copy;

    if (length == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(length + 1u);
    if (copy != NULL)
        memcpy(copy, source, length + 1u);
    return copy;
}

static bool stream_execution_terminal(cflow_stream_execution_state state) {
    return state == CFLOW_STREAM_EXECUTION_COMPLETED ||
           state == CFLOW_STREAM_EXECUTION_FAILED ||
           state == CFLOW_STREAM_EXECUTION_CANCELLED;
}

static void stream_execution_collector_abort(
    cflow_stream_execution_impl *impl) {
    cflow_stream_execution_impl *previous;

    if (impl == NULL)
        return;
    previous = active_stream_execution;
    active_stream_execution = impl;
    cmeta_collector_abort(&impl->collector);
    active_stream_execution = previous;
}

static void stream_execution_shell_destroy(
    cflow_stream_execution_impl *impl) {
    if (impl == NULL)
        return;
    free(impl->error);
    cflow_graph_destroy(&impl->graph);
    if (impl->changed != NULL)
        cmeta_cond_destroy(&impl->changed);
    if (impl->gate != NULL)
        cmeta_mutex_destroy(&impl->gate);
    free(impl);
}

static void stream_execution_publish_failure(
    cflow_stream_execution_impl *impl, const char *message) {
    char *copy;

    if (impl == NULL)
        return;
    copy = stream_execution_copy_error(message);
    cmeta_mutex_lock(&impl->gate);
    if (impl->error == NULL && copy != NULL) {
        impl->error = copy;
        copy = NULL;
    }
    impl->collector_status = impl->collector.status;
    impl->count = impl->collector.count;
    if (impl->state == CFLOW_STREAM_EXECUTION_RUNNING) {
        impl->state = CFLOW_STREAM_EXECUTION_FAILED;
        cmeta_cond_broadcast(&impl->changed);
    }
    cmeta_mutex_unlock(&impl->gate);
    free(copy);
}

static bool stream_execution_sink_value(void *user,
                                        const cmeta_type_desc *type,
                                        const void *value) {
    cflow_stream_execution_impl *impl =
        (cflow_stream_execution_impl *)user;
    cflow_stream_execution_impl *previous;
    cmeta_status status;

    if (impl == NULL)
        return false;
    previous = active_stream_execution;
    active_stream_execution = impl;
    status = cmeta_collector_accept(&impl->collector, type, value);
    active_stream_execution = previous;

    cmeta_mutex_lock(&impl->gate);
    impl->collector_status = impl->collector.status;
    impl->count = impl->collector.count;
    cmeta_mutex_unlock(&impl->gate);
    return status == CMETA_OK;
}

static void stream_execution_sink_error(void *user, const char *message) {
    cflow_stream_execution_impl *impl =
        (cflow_stream_execution_impl *)user;
    cflow_stream_execution_impl *previous;

    if (impl == NULL)
        return;
    previous = active_stream_execution;
    active_stream_execution = impl;
    if (impl->collector.status == CMETA_OK)
        cmeta_collector_fail(&impl->collector, CMETA_CALLBACK_ERROR);
    active_stream_execution = previous;
    stream_execution_publish_failure(impl, message);
}

static void stream_execution_sink_done(void *user) {
    cflow_stream_execution_impl *impl =
        (cflow_stream_execution_impl *)user;
    cflow_stream_execution_impl *previous;
    cmeta_status status;
    char *copy = NULL;

    if (impl == NULL)
        return;
    previous = active_stream_execution;
    active_stream_execution = impl;
    status = cmeta_collector_finish(&impl->collector);
    active_stream_execution = previous;
    if (status != CMETA_OK)
        copy = stream_execution_copy_error("collector finish failed");

    cmeta_mutex_lock(&impl->gate);
    impl->collector_status = impl->collector.status;
    impl->count = impl->collector.count;
    if (impl->state == CFLOW_STREAM_EXECUTION_RUNNING) {
        if (status == CMETA_OK) {
            impl->state = CFLOW_STREAM_EXECUTION_COMPLETED;
        } else {
            if (impl->error == NULL && copy != NULL) {
                impl->error = copy;
                copy = NULL;
            }
            impl->state = CFLOW_STREAM_EXECUTION_FAILED;
        }
        cmeta_cond_broadcast(&impl->changed);
    }
    cmeta_mutex_unlock(&impl->gate);
    free(copy);
}

static void stream_execution_rollback(
    cflow_stream_execution *execution,
    cflow_stream_execution_impl *impl,
    cflow_publisher *source) {
    if (impl == NULL)
        return;
    if (impl->run.impl != NULL)
        cflow_subscription_close(&impl->run);
    if (source != NULL && cflow_publisher_valid(source))
        cflow_publisher_destroy(source);
    stream_execution_collector_abort(impl);
    if (execution != NULL)
        execution->impl = NULL;
    stream_execution_shell_destroy(impl);
}

cflow_stream_execution_status cflow_stream_execution_start(
    cflow_stream_execution *execution,
    const cflow_stream *stream,
    cflow_scheduler *scheduler,
    cmeta_collector collector) {
    cflow_stream_execution_impl *impl;
    cflow_publisher source = {0};
    cflow_subscriber sink;
    const cmeta_type_desc *output_type;
    const char *source_error = NULL;
    cmeta_status source_status;
    cflow_stream_execution_impl *previous;

    if (execution == NULL)
        return CFLOW_STREAM_EXECUTION_INVALID_ARGUMENT;
    if (execution->impl != NULL)
        return CFLOW_STREAM_EXECUTION_ALREADY_STARTED;
    if (scheduler == NULL || !cflow_scheduler_valid(scheduler) ||
        (cflow_scheduler_capabilities(scheduler) &
         CMETA_SCHED_CAP_CONCURRENT) == 0u)
        return CFLOW_STREAM_EXECUTION_INVALID_SCHEDULER;
    if (stream == NULL || !stream->has_input_range ||
        !cflow_stream_ok(stream))
        return CFLOW_STREAM_EXECUTION_STREAM_REJECTED;
    output_type = cflow_stream_output_type(stream);
    if (collector.state != CMETA_COLLECTOR_ZERO ||
        !cmeta_collector_ops_valid(collector.ops) ||
        !cmeta_type_equal(output_type, collector.input_type))
        return CFLOW_STREAM_EXECUTION_COLLECTOR_REJECTED;

    impl = (cflow_stream_execution_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return CFLOW_STREAM_EXECUTION_ALLOCATION_FAILED;
    impl->graph.root = CMETA_INVALID_ID;
    impl->state = CFLOW_STREAM_EXECUTION_RUNNING;
    impl->collector_status = CMETA_OK;
    impl->scheduler = scheduler;
    impl->collector = collector;
    cmeta_mutex_init(&impl->gate);
    cmeta_cond_init(&impl->changed);
    if (impl->gate == NULL || impl->changed == NULL) {
        stream_execution_shell_destroy(impl);
        return CFLOW_STREAM_EXECUTION_ALLOCATION_FAILED;
    }
    execution->impl = impl;

    if (!cflow_graph_normalize(&impl->graph, &stream->graph)) {
        stream_execution_rollback(execution, impl, &source);
        return CFLOW_STREAM_EXECUTION_GRAPH_REJECTED;
    }
    source_status = cflow_publisher_from_range_checked(
        &source, stream->input_range, &source_error);
    (void)source_error;
    if (source_status != CMETA_OK) {
        stream_execution_rollback(execution, impl, &source);
        return source_status == CMETA_OUT_OF_MEMORY
            ? CFLOW_STREAM_EXECUTION_ALLOCATION_FAILED
            : CFLOW_STREAM_EXECUTION_PUBLISHER_REJECTED;
    }

    previous = active_stream_execution;
    active_stream_execution = impl;
    source_status = cmeta_collector_begin(&impl->collector);
    active_stream_execution = previous;
    if (source_status != CMETA_OK) {
        stream_execution_rollback(execution, impl, &source);
        return CFLOW_STREAM_EXECUTION_COLLECTOR_REJECTED;
    }
    impl->callbacks = (cflow_subscriber_callbacks){
        stream_execution_sink_value,
        stream_execution_sink_error,
        stream_execution_sink_done,
        impl
    };
    sink = cflow_subscriber_from_callbacks(&impl->callbacks);
    if (!cflow_subscribe(&impl->run, &impl->graph, &source,
                        scheduler, &sink)) {
        stream_execution_rollback(execution, impl, &source);
        return CFLOW_STREAM_EXECUTION_RUN_REJECTED;
    }
    if (!cflow_subscription_request(&impl->run, SIZE_MAX)) {
        stream_execution_rollback(execution, impl, &source);
        return CFLOW_STREAM_EXECUTION_DEMAND_REJECTED;
    }
    return CFLOW_STREAM_EXECUTION_OK;
}

cflow_stream_execution_status cflow_stream_execution_cancel(
    cflow_stream_execution *execution) {
    cflow_stream_execution_impl *impl = execution != NULL
        ? (cflow_stream_execution_impl *)execution->impl : NULL;
    cflow_stream_execution_state state;

    if (impl == NULL)
        return CFLOW_STREAM_EXECUTION_INVALID_ARGUMENT;
    if (active_stream_execution == impl ||
        cflow_subscription_active_on_current_thread(&impl->run))
        return CFLOW_STREAM_EXECUTION_WOULD_BLOCK;

    cmeta_mutex_lock(&impl->gate);
    state = impl->state;
    cmeta_mutex_unlock(&impl->gate);
    if (stream_execution_terminal(state)) {
        /* Terminal publication happens inside the pump callback. Close remains
         * the barrier that waits for the pump and releases the moved Source. */
        cflow_subscription_close(&impl->run);
        return CFLOW_STREAM_EXECUTION_TERMINATED;
    }

    cflow_subscription_close(&impl->run);
    stream_execution_collector_abort(impl);

    cmeta_mutex_lock(&impl->gate);
    impl->collector_status = impl->collector.status;
    impl->count = impl->collector.count;
    if (impl->state == CFLOW_STREAM_EXECUTION_RUNNING) {
        impl->state = CFLOW_STREAM_EXECUTION_CANCELLED;
        cmeta_cond_broadcast(&impl->changed);
    }
    state = impl->state;
    cmeta_mutex_unlock(&impl->gate);
    return state == CFLOW_STREAM_EXECUTION_CANCELLED
        ? CFLOW_STREAM_EXECUTION_OK : CFLOW_STREAM_EXECUTION_TERMINATED;
}

cflow_stream_execution_status cflow_stream_execution_wait(
    cflow_stream_execution *execution) {
    cflow_stream_execution_impl *impl = execution != NULL
        ? (cflow_stream_execution_impl *)execution->impl : NULL;

    if (impl == NULL)
        return CFLOW_STREAM_EXECUTION_INVALID_ARGUMENT;
    if (active_stream_execution == impl ||
        cflow_subscription_active_on_current_thread(&impl->run))
        return CFLOW_STREAM_EXECUTION_WOULD_BLOCK;
    cmeta_mutex_lock(&impl->gate);
    while (!stream_execution_terminal(impl->state))
        cmeta_cond_wait(&impl->changed, &impl->gate);
    cmeta_mutex_unlock(&impl->gate);
    return CFLOW_STREAM_EXECUTION_OK;
}

bool cflow_stream_execution_get_snapshot(
    const cflow_stream_execution *execution,
    cflow_stream_execution_snapshot *out) {
    cflow_stream_execution_impl *impl = execution != NULL
        ? (cflow_stream_execution_impl *)execution->impl : NULL;

    if (impl == NULL || out == NULL)
        return false;
    cmeta_mutex_lock(&impl->gate);
    out->state = impl->state;
    out->collector_status = impl->collector_status;
    out->count = impl->count;
    out->error = impl->error != NULL ? impl->error
        : (impl->state == CFLOW_STREAM_EXECUTION_FAILED
            ? stream_execution_generic_error : NULL);
    cmeta_mutex_unlock(&impl->gate);
    return true;
}

cflow_stream_execution_status cflow_stream_execution_destroy(
    cflow_stream_execution *execution) {
    cflow_stream_execution_impl *impl;
    cflow_stream_execution_state state;

    if (execution == NULL)
        return CFLOW_STREAM_EXECUTION_INVALID_ARGUMENT;
    impl = (cflow_stream_execution_impl *)execution->impl;
    if (impl == NULL)
        return CFLOW_STREAM_EXECUTION_OK;
    if (active_stream_execution == impl ||
        cflow_subscription_active_on_current_thread(&impl->run))
        return CFLOW_STREAM_EXECUTION_WOULD_BLOCK;

    cmeta_mutex_lock(&impl->gate);
    state = impl->state;
    cmeta_mutex_unlock(&impl->gate);
    if (state == CFLOW_STREAM_EXECUTION_RUNNING)
        (void)cflow_stream_execution_cancel(execution);
    /* cancel may observe a natural terminal transition after the state sample.
     * The idempotent close is still required as the destruction barrier. */
    cflow_subscription_close(&impl->run);
    execution->impl = NULL;
    stream_execution_shell_destroy(impl);
    return CFLOW_STREAM_EXECUTION_OK;
}
