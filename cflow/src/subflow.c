#include <cflow/subflow.h>
#include <cflow/publishers.h>
#include <thread.h>

#include "value_storage.h"

#include <stdlib.h>
#include <string.h>

typedef struct subflow_state {
    const cflow_graph *graph;
    cflow_subgraph_id subgraph;
    cflow_value_slot input;
    cflow_value_slot value;
    cflow_subscription run;
    cflow_scheduler *scheduler;
    cflow_eval_options eval_options;

    cmeta_mutex_t lock;
    cflow_waker waiter;
    bool started;
    bool requested;
    bool value_ready;
    bool done;
    bool cancelled;
    const char *error;
} subflow_state;

static bool subflow_on_value(void *user,
                           const cmeta_type_desc *type,
                           const void *value) {
    subflow_state *s = (subflow_state *)user;
    cflow_value_slot incoming = {0};
    cflow_value_slot empty = {0};
    if (!s || !value ||
        !cmeta_type_equal(type, cflow_subgraph_output_type(s->graph, s->subgraph))) return false;
    if (!cflow_value_slot_init(&incoming, type) ||
        !cflow_value_slot_copy(&incoming, value)) {
        cflow_value_slot_destroy(&incoming);
        return false;
    }
    cmeta_mutex_lock(&s->lock);
    if (s->cancelled || s->value_ready) {
        cmeta_mutex_unlock(&s->lock);
        cflow_value_slot_destroy(&incoming);
        return false;
    }
    empty = s->value;
    s->value = incoming;
    memset(&incoming, 0, sizeof(incoming));
    s->value_ready = true;
    s->requested = false;
    cflow_waker w = s->waiter;
    s->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&s->lock);
    cflow_value_slot_destroy(&empty);
    if (w.wake) w.wake(w.user);
    return true;
}

static void subflow_on_error(void *user, const char *message) {
    subflow_state *s = (subflow_state *)user;
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    if (!s->cancelled) s->error = message ? message : "subgraph error";
    s->requested = false;
    cflow_waker w = s->waiter;
    s->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&s->lock);
    if (w.wake) w.wake(w.user);
}

static void subflow_on_done(void *user) {
    subflow_state *s = (subflow_state *)user;
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    s->done = true;
    s->requested = false;
    cflow_waker w = s->waiter;
    s->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&s->lock);
    if (w.wake) w.wake(w.user);
}


CMETA_IMPLEMENTS(cflow_subscriber, subflow_sink, 0,
    .value = subflow_on_value,
    .error = subflow_on_error,
    .done = subflow_on_done
);
static bool subflow_start(subflow_state *s, cflow_publish_context *ctx) {
    if (!s || !ctx || !ctx->scheduler) return false;
    if (s->started) return s->scheduler == ctx->scheduler;

    cflow_publisher source = {0};
    cflow_subscriber sink = subflow_sink_as_cflow_subscriber(s);
    if (!cflow_publisher_from_array(&source,
                                 cflow_subgraph_input_type(s->graph, s->subgraph),
                                 s->input.storage,
                                 1)) return false;
    cflow_status_result opened = cflow_subscribe_subgraph_with_options(
        &s->run, s->graph, s->subgraph, &source, ctx->scheduler, &sink,
        &s->eval_options);
    if (!cflow_status_result_is_ok(opened)) {
        cflow_publisher_destroy(&source);
        return false;
    }
    s->scheduler = ctx->scheduler;
    s->started = true;
    return true;
}

static bool subflow_wait_arm(void *state, cflow_waker w) {
    subflow_state *s = (subflow_state *)state;
    if (!s) return false;
    cmeta_mutex_lock(&s->lock);
    bool ready = s->value_ready || s->done || s->error || s->cancelled;
    if (!ready) s->waiter = w;
    cmeta_mutex_unlock(&s->lock);
    if (ready && w.wake) w.wake(w.user);
    return true;
}

static void subflow_wait_cancel(void *state) {
    subflow_state *s = (subflow_state *)state;
    if (!s) return;
    bool started = false;
    cmeta_mutex_lock(&s->lock);
    s->waiter = (cflow_waker){0};
    started = s->started;
    cmeta_mutex_unlock(&s->lock);
    if (started) cflow_subscription_cancel(&s->run);
}

CMETA_IMPLEMENTS(cflow_waitable, subflow_waitable, 0,
    .arm = subflow_wait_arm,
    .cancel = subflow_wait_cancel
);

static cflow_step subflow_resume(void *state,
                               cflow_publish_context *ctx,
                               void *out_value) {
    subflow_state *s = (subflow_state *)state;
    if (!s || !out_value) return (cflow_step){ CFLOW_STEP_ERROR, {0}, "subflow state is invalid" };
    if (!subflow_start(s, ctx))
        return (cflow_step){ CFLOW_STEP_ERROR, {0}, "subgraph could not start" };

    bool need_request = false;
    cmeta_mutex_lock(&s->lock);

    if (s->cancelled) {
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
    }
    if (s->error) {
        const char *err = s->error;
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_ERROR, {0}, err };
    }
    if (s->value_ready) {
        const cmeta_type_desc *type = cflow_subgraph_output_type(s->graph, s->subgraph);
        cflow_value_slot value = s->value;
        memset(&s->value, 0, sizeof(s->value));
        s->value_ready = false;
        bool done = s->done;
        cmeta_mutex_unlock(&s->lock);
        if (!cflow_value_construct(type, out_value, value.storage)) {
            cflow_value_slot_destroy(&value);
            return (cflow_step){ CFLOW_STEP_ERROR, {0},
                                 "subgraph value construction failed" };
        }
        cflow_value_slot_destroy(&value);
        return (cflow_step){ done ? CFLOW_STEP_VALUE_AND_DONE : CFLOW_STEP_VALUE, {0}, NULL };
    }
    if (s->done) {
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
    }
    if (!s->requested) {
        s->requested = true;
        need_request = true;
    }
    cmeta_mutex_unlock(&s->lock);

    if (need_request && !cflow_subscription_request(&s->run, 1)) {
        cmeta_mutex_lock(&s->lock);
        s->requested = false;
        cmeta_mutex_unlock(&s->lock);
        return (cflow_step){ CFLOW_STEP_ERROR, {0}, "subgraph request failed" };
    }

    /* A concurrent scheduler may have completed between request() and here. */
    cmeta_mutex_lock(&s->lock);
    bool ready = s->value_ready || s->done || s->error || s->cancelled;
    cmeta_mutex_unlock(&s->lock);
    if (ready) return subflow_resume(state, ctx, out_value);
    return (cflow_step){ CFLOW_STEP_WAIT, subflow_waitable_as_cflow_waitable(s), NULL };
}

static void subflow_cancel(void *state) {
    subflow_state *s = (subflow_state *)state;
    if (!s) return;
    cmeta_mutex_lock(&s->lock);
    s->cancelled = true;
    s->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&s->lock);
    if (s->started) cflow_subscription_cancel(&s->run);
}

static void subflow_destroy(void *state) {
    subflow_state *s = (subflow_state *)state;
    if (!s) return;
    subflow_cancel(s);
    if (s->started) cflow_subscription_close(&s->run);
    cmeta_mutex_destroy(&s->lock);
    cflow_value_slot_destroy(&s->value);
    cflow_value_slot_destroy(&s->input);
    free(s);
}

static const cflow_resumable_ops subflow_ops = {
    subflow_resume,
    subflow_cancel,
    subflow_destroy
};

bool cflow_resumable_from_subgraph(cflow_resumable *out,
                                    const cflow_graph *graph,
                                    cflow_subgraph_id subgraph,
                                    const void *input_value) {
    return cflow_resumable_from_subgraph_with_options(
        out, graph, subgraph, input_value, NULL);
}

bool cflow_resumable_from_subgraph_with_options(
    cflow_resumable *out,
    const cflow_graph *graph,
    cflow_subgraph_id subgraph,
    const void *input_value,
    const cflow_eval_options *options) {
    if (!out || !graph || !cflow_graph_subgraph(graph, subgraph) ||
        !input_value || !cflow_value_runtime_graph_supported(graph))
        return false;
    subflow_state *s = calloc(1, sizeof(*s));
    if (!s) return false;
    cmeta_mutex_init(&s->lock);
    if (!s->lock) {
        free(s);
        return false;
    }
    const cmeta_type_desc *in = cflow_subgraph_input_type(graph, subgraph);
    const cmeta_type_desc *out_type = cflow_subgraph_output_type(graph, subgraph);
    if (!cflow_value_slot_init(&s->input, in) ||
        !cflow_value_slot_copy(&s->input, input_value) ||
        !cflow_value_slot_init(&s->value, out_type)) {
        cflow_value_slot_destroy(&s->value);
        cflow_value_slot_destroy(&s->input);
        cmeta_mutex_destroy(&s->lock);
        free(s);
        return false;
    }
    s->graph = graph;
    s->subgraph = subgraph;
    if (options) s->eval_options = *options;
    *out = (cflow_resumable){ "subflow", out_type, &subflow_ops, s };
    return true;
}
