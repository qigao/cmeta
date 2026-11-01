#include <cflow/operators.h>
#include "graph_internal.h"
#include <cflow/reactive.h>
#include "scheduler_internal.h"
#include <cflow/lower.h>
#include <cflow/subflow.h>
#include <cflow/coord.h>
#include <cflow/relation_exec.h>
#include <thread.h>

#include "value_storage.h"
#include "subscription_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef CMETA_RUN_QUANTUM
#define CMETA_RUN_QUANTUM 64u
#endif

#ifndef CMETA_RUN_MAX_CONTINUATIONS
#define CMETA_RUN_MAX_CONTINUATIONS 64u
#endif

typedef struct generator_state {
    cmeta_callable fn;
    cflow_value_slot input;
    size_t cursor;
} generator_state;

typedef struct continuation_frame {
    cflow_node_id node_id;
    cflow_resumable machine;
    cflow_value_slot root;
    cflow_value_slot output;
    bool done;
} continuation_frame;

typedef struct run_impl {
    cflow_subscription *owner;
    const cflow_graph *graph;
    cflow_subgraph_id subgraph_id;
    const cflow_subgraph *subgraph;
    cflow_publisher source;
    const cmeta_type_desc *source_type;
    cflow_scheduler *scheduler;
    cflow_subscriber sink;
    cflow_publish_context resume_ctx;

    cflow_value_slot source_slot;
    cflow_value_slot *reduce_value;
    bool *reduce_flushed;
    size_t *slice_counts;
    void **set_states;
    void **sequence_states;
    cmeta_range *sequence_ranges;
    cmeta_range_cursor *sequence_cursors;
    bool *sequence_started;
    bool *sequence_flushed;
    cflow_eval_options eval_options;

    continuation_frame continuations[CMETA_RUN_MAX_CONTINUATIONS];
    size_t continuation_count;
    /* Pump completion is observed across lifecycle and worker boundaries.
     * Keep it atomic and separate from lock-protected lifecycle flags. */
    atomic_bool source_done;

    cmeta_mutex_t lock;
    cmeta_cond_t task_cv;
    size_t task_refs;
    atomic_size_t demand;
    bool identity_path;
    bool task_scheduled;
    /* A posting ref keeps the Run alive when a foreign Scheduler invokes the
     * pump inline; the task ref settles only through run or cancellation. */
    size_t task_posting;
    cflow_task_id scheduled_task_id;
    uint64_t scheduled_task_generation;
    uint64_t next_task_generation;
    bool scheduler_settles_cancel;
    bool rejection_must_fail;
    bool pump_running;
    bool waiting;
    cflow_waitable active_wait;
    bool cancel_requested;
    bool cancelled;
    atomic_bool terminated;
    bool close_requested;
    bool external_closer;
    bool destroying;
    bool closed;
    const char *error;
    cflow_status status;
} run_impl;

static SALTS_THREAD_LOCAL run_impl *active_pump_run;
static SALTS_THREAD_LOCAL const cflow_subscription *active_destroy_owner;
static cmeta_once_t run_lifecycle_once = SALTS_ONCE_INIT;
static cmeta_mutex_t run_lifecycle_lock;
static cmeta_cond_t run_lifecycle_cv;
static bool run_lifecycle_ready;

static void run_lifecycle_init(void) {
    cmeta_mutex_init(&run_lifecycle_lock);
    if (!run_lifecycle_lock) return;
    cmeta_cond_init(&run_lifecycle_cv);
    if (!run_lifecycle_cv) {
        cmeta_mutex_destroy(&run_lifecycle_lock);
        return;
    }
    run_lifecycle_ready = true;
}

static bool run_lifecycle_ensure(void) {
    cmeta_once(&run_lifecycle_once, run_lifecycle_init);
    return run_lifecycle_ready;
}

static run_impl *impl_of(const cflow_subscription *run) {
    return run ? (run_impl *)run->impl : NULL;
}

bool cflow_subscription_active_on_current_thread(const cflow_subscription *run) {
    run_impl *impl = impl_of(run);
    return (impl != NULL && active_pump_run == impl) ||
           (run != NULL && active_destroy_owner == run);
}

static cflow_step generator_resume_machine(void *state, cflow_publish_context *ctx, void *out) {
    (void)ctx;
    generator_state *g = (generator_state *)state;
    if (!g) return (cflow_step){ CFLOW_STEP_ERROR, {0}, "generator state is null" };
    cmeta_gen_status st = cmeta_callable_generate(
        &g->fn, g->input.storage, out, &g->cursor);
    switch (st) {
        case CMETA_GEN_VALUE: return (cflow_step){ CFLOW_STEP_VALUE, {0}, NULL };
        case CMETA_GEN_VALUE_AND_DONE: return (cflow_step){ CFLOW_STEP_VALUE_AND_DONE, {0}, NULL };
        case CMETA_GEN_DONE: return (cflow_step){ CFLOW_STEP_DONE, {0}, NULL };
        case CMETA_GEN_ERROR: return (cflow_step){ CFLOW_STEP_ERROR, {0}, "generator returned error" };
    }
    return (cflow_step){ CFLOW_STEP_ERROR, {0}, "invalid generator status" };
}
static void generator_destroy_machine(void *state) {
    generator_state *g = (generator_state *)state;
    if (g) { cflow_value_slot_destroy(&g->input); free(g); }
}
static const cflow_resumable_ops generator_machine_ops = {
    generator_resume_machine, NULL, generator_destroy_machine
};


static void continuation_clear(continuation_frame *f) {
    if (!f) return;
    if (f->machine.ops && f->machine.ops->destroy) f->machine.ops->destroy(f->machine.state);
    cflow_value_slot_destroy(&f->root);
    cflow_value_slot_destroy(&f->output);
    memset(f, 0, sizeof(*f));
}

static void continuations_clear(run_impl *r) {
    while (r && r->continuation_count) continuation_clear(&r->continuations[--r->continuation_count]);
}

/* Takes ownership of machine on success. root_source is copied so nested
 * child/stateful work can outlive the current process_path() stack frame. */
static bool push_continuation(run_impl *r,
                              cflow_node_id node_id,
                              cflow_resumable machine,
                              const void *root_source) {
    if (!r || !machine.ops || !machine.ops->resume || !machine.output_type ||
        r->continuation_count >= CMETA_RUN_MAX_CONTINUATIONS) return false;
    continuation_frame *f = &r->continuations[r->continuation_count++];
    memset(f, 0, sizeof(*f));
    f->node_id = node_id;
    f->machine = machine;
    if (!cflow_value_slot_init(&f->output, machine.output_type)) {
        continuation_clear(f);
        --r->continuation_count;
        return false;
    }
    if (root_source) {
        const cmeta_type_desc *src_type = cflow_subgraph_input_type(r->graph, r->subgraph_id);
        if (!cflow_value_slot_init(&f->root, src_type) ||
            !cflow_value_slot_copy(&f->root, root_source)) {
            continuation_clear(f);
            --r->continuation_count;
            return false;
        }
    }
    return true;
}

static void reducers_clear(run_impl *r) {
    if (!r) return;
    if (r->reduce_value) {
        for (size_t i = 0; i < r->subgraph->node_count; ++i)
            cflow_value_slot_destroy(&r->reduce_value[i]);
    }
    free(r->reduce_value);
    free(r->reduce_flushed);
    r->reduce_value = NULL;
    r->reduce_flushed = NULL;
    free(r->slice_counts);
    r->slice_counts = NULL;
}

static void set_states_clear(run_impl *r) {
    if (!r || !r->set_states) return;
    if (r->eval_options.set_state && r->eval_options.set_state->close) {
        for (size_t i = 0u; i < r->subgraph->node_count; ++i) {
            if (r->set_states[i])
                r->eval_options.set_state->close(r->set_states[i]);
        }
    }
    free(r->set_states);
    r->set_states = NULL;
}

static void sequence_states_clear(run_impl *r) {
    if (!r) return;
    if (r->sequence_states && r->eval_options.sequence_state &&
        r->eval_options.sequence_state->close) {
        for (size_t i = 0u; i < r->subgraph->node_count; ++i) {
            if (r->sequence_states[i])
                r->eval_options.sequence_state->close(
                    r->sequence_states[i]);
        }
    }
    free(r->sequence_states);
    free(r->sequence_ranges);
    free(r->sequence_cursors);
    free(r->sequence_started);
    free(r->sequence_flushed);
    r->sequence_states = NULL;
    r->sequence_ranges = NULL;
    r->sequence_cursors = NULL;
    r->sequence_started = NULL;
    r->sequence_flushed = NULL;
}


static size_t demand_get(run_impl *r) {
    return atomic_load_explicit(&r->demand, memory_order_acquire);
}

static cflow_publish_context *run_resume_context(run_impl *r) {
    r->resume_ctx.downstream_demand = demand_get(r);
    return &r->resume_ctx;
}

static bool demand_consume_one(run_impl *r) {
    size_t current = atomic_load_explicit(&r->demand, memory_order_relaxed);

    while (current != 0u) {
        if (atomic_compare_exchange_weak_explicit(
                &r->demand, &current, current - 1u,
                memory_order_acq_rel, memory_order_relaxed))
            return true;
    }
    return false;
}

static void run_fail_status(run_impl *r,
                            cflow_status status,
                            const char *message) {
    if (!r) return;
    const char *msg = message ? message : "runtime error";
    bool notify = false;
    cmeta_mutex_lock(&r->lock);
    if (!r->terminated) {
        r->error = msg;
        r->status = status == CFLOW_STATUS_OK
            ? CFLOW_STATUS_EXECUTION_ERROR : status;
        r->terminated = true;
        notify = true;
    }
    cmeta_mutex_unlock(&r->lock);
    if (notify && cflow_subscriber_valid(&r->sink)) cflow_subscriber_error(&r->sink, msg);
}

static void run_fail(run_impl *r, const char *message) {
    run_fail_status(r, CFLOW_STATUS_EXECUTION_ERROR, message);
}

static bool terminal_nodes_done(const run_impl *r) {
    if (!r || !r->subgraph) return true;
    for (size_t i = 0; i < r->subgraph->node_count; ++i) {
        const cflow_node *node = cflow_subgraph_node(r->subgraph, (cflow_node_id)i);
        const cflow_op_schema *schema = node ? cflow_op_schema_get(node->op) : NULL;
        if (schema && schema->cardinality == CMETA_CARD_REDUCE &&
            r->reduce_value[i].live && !r->reduce_flushed[i]) return false;
        if (node && node->op == CFLOW_OP_SORTED &&
            (!r->sequence_flushed || !r->sequence_flushed[i])) return false;
    }
    return true;
}


typedef enum node_action {
    NODE_CONTINUE,
    NODE_STOP,
    NODE_FAIL
} node_action;

typedef node_action (*node_handler)(run_impl *r,
                                    cflow_node_id node_id,
                                    const cflow_node *n,
                                    cflow_value_slot *owned,
                                    const cmeta_type_desc **cur_type,
                                    const void *root_source);

static node_action semantic_filter(run_impl *r, cflow_node_id i, const cflow_node *n,
                                   cflow_value_slot *owned, const cmeta_type_desc **cur_type,
                                   const void *root_source) {
    (void)r; (void)i; (void)cur_type; (void)root_source;
    _Bool keep = false;
    const void *args[1] = { owned->storage };
    bool invoked;

    if (n->param_kind == CFLOW_NODE_PARAM_TYPED_ADAPTER) {
        if (!cflow_graph_explicit_adapter_callable_valid(n->fn) ||
            !cmeta_type_equal(n->input_type, n->output_type))
            return NODE_FAIL;
        invoked = n->fn.invoke &&
            n->fn.invoke(&n->fn, &keep, args);
    } else {
        invoked = cmeta_callable_invoke(&n->fn, &keep, args);
    }
    if (!invoked) return NODE_FAIL;
    if (!keep) { cflow_value_slot_reset(owned); return NODE_STOP; }
    return NODE_CONTINUE;
}

static void short_circuit_before_current_node(run_impl *r,
                                              cflow_node_id current_node) {
    if (!r) return;
    if (!r->source_done) cflow_publisher_cancel(&r->source);
    r->source_done = true;
    /* Every frame present while processing this value is upstream of the
     * current node. Mark it terminal without destroying the live frame being
     * unwound; a continuation created later in the same path is downstream
     * and remains allowed to finish. */
    for (size_t index = 0u; index < r->continuation_count; ++index) {
        continuation_frame *frame = &r->continuations[index];
        if (!frame->done && frame->machine.ops && frame->machine.ops->cancel)
            frame->machine.ops->cancel(frame->machine.state);
        frame->done = true;
    }
    if (r->sequence_flushed) {
        for (size_t index = 0u;
             index < r->subgraph->node_count && index < current_node;
             ++index) {
            const cflow_node *node = cflow_subgraph_node(
                r->subgraph, (cflow_node_id)index);
            if (node && node->op == CFLOW_OP_SORTED)
                r->sequence_flushed[index] = true;
        }
    }
}

static node_action semantic_take(run_impl *r, cflow_node_id i,
                                 const cflow_node *n,
                                 cflow_value_slot *owned,
                                 const cmeta_type_desc **cur_type,
                                 const void *root_source) {
    (void)owned;
    (void)cur_type;
    (void)root_source;
    if (!r || !n || !r->slice_counts || !n->has_size_parameter)
        return NODE_FAIL;
    if (r->slice_counts[i] >= n->size_parameter)
        return NODE_STOP;
    ++r->slice_counts[i];
    if (r->slice_counts[i] == n->size_parameter)
        short_circuit_before_current_node(r, i);
    return NODE_CONTINUE;
}

static node_action semantic_skip(run_impl *r, cflow_node_id i,
                                 const cflow_node *n,
                                 cflow_value_slot *owned,
                                 const cmeta_type_desc **cur_type,
                                 const void *root_source) {
    (void)owned;
    (void)cur_type;
    (void)root_source;
    if (!r || !n || !r->slice_counts || !n->has_size_parameter)
        return NODE_FAIL;
    if (r->slice_counts[i] < n->size_parameter) {
        ++r->slice_counts[i];
        return NODE_STOP;
    }
    return NODE_CONTINUE;
}

static node_action semantic_distinct(run_impl *r, cflow_node_id i,
                                     const cflow_node *n,
                                     cflow_value_slot *owned,
                                     const cmeta_type_desc **cur_type,
                                     const void *root_source) {
    bool inserted = false;
    cflow_status status;
    (void)cur_type;
    (void)root_source;
    if (!r || !n || !owned || !owned->live || !r->set_states ||
        !r->set_states[i] ||
        !cflow_set_state_ops_valid(r->eval_options.set_state))
        return NODE_FAIL;
    status = r->eval_options.set_state->insert_if_absent(
        r->set_states[i], owned->storage, &inserted);
    if (status != CFLOW_STATUS_OK) {
        run_fail_status(
            r, status,
            status == CFLOW_STATUS_CAPACITY_EXCEEDED
                ? "distinct capacity exceeded"
                : "distinct backend failed");
        return NODE_FAIL;
    }
    if (!inserted) {
        cflow_value_slot_reset(owned);
        return NODE_STOP;
    }
    return NODE_CONTINUE;
}

static node_action semantic_sorted(run_impl *r, cflow_node_id i,
                                   const cflow_node *n,
                                   cflow_value_slot *owned,
                                   const cmeta_type_desc **cur_type,
                                   const void *root_source) {
    cflow_status status;
    (void)n;
    (void)cur_type;
    (void)root_source;
    if (!r || !owned || !owned->live || !r->sequence_states ||
        !r->sequence_states[i] ||
        !cflow_sequence_state_ops_valid(r->eval_options.sequence_state))
        return NODE_FAIL;
    status = r->eval_options.sequence_state->append(
        r->sequence_states[i], owned->storage);
    if (status != CFLOW_STATUS_OK) {
        run_fail_status(
            r, status,
            status == CFLOW_STATUS_CAPACITY_EXCEEDED
                ? "sorted capacity exceeded"
                : "sorted backend append failed");
        return NODE_FAIL;
    }
    cflow_value_slot_reset(owned);
    return NODE_STOP;
}

static node_action semantic_map(run_impl *r, cflow_node_id i, const cflow_node *n,
                                cflow_value_slot *owned, const cmeta_type_desc **cur_type,
                                const void *root_source) {
    (void)r; (void)i; (void)root_source;
    if (n->param_kind == CFLOW_NODE_PARAM_TYPED_ADAPTER) {
        cflow_value_slot next = {0};
        const void *args[1];

        if (!owned || !owned->live || !cur_type ||
            !cmeta_type_equal(*cur_type, n->input_type) ||
            n->fn.invoke == NULL ||
            !cflow_value_slot_init(&next, n->output_type))
            return NODE_FAIL;

        args[0] = owned->storage;
        if (!n->fn.invoke(&n->fn, next.storage, args)) {
            cflow_value_slot_destroy(&next);
            return NODE_FAIL;
        }
        next.live = true;
        cflow_value_slot_destroy(owned);
        *owned = next;
        *cur_type = n->output_type;
        return NODE_CONTINUE;
    }

    const cmeta_callable *chain = n->fn_chain_count ? n->fn_chain : &n->fn;
    size_t count = n->fn_chain_count ? n->fn_chain_count : 1u;
    const cmeta_type_desc *type = *cur_type;
    for (size_t k = 0; k < count; ++k) {
        const cmeta_sig_desc *sig = cmeta_callable_signature(chain[k]);
        if (!sig || sig->param_count != 1u || !cmeta_type_equal(sig->params[0], type))
            return NODE_FAIL;
        const cmeta_type_desc *next_type =
            k + 1u == count ? n->output_type : sig->return_type;
        cflow_value_slot next = {0};
        const void *args[1] = { owned->storage };
        if (!cflow_value_slot_init(&next, next_type)) return NODE_FAIL;
        if (!cmeta_callable_invoke(&chain[k], next.storage, args)) {
            cflow_value_slot_destroy(&next);
            return NODE_FAIL;
        }
        next.live = true;
        cflow_value_slot_destroy(owned);
        *owned = next;
        type = next_type;
    }
    *cur_type = type;
    return cmeta_type_equal(type, n->output_type) ? NODE_CONTINUE : NODE_FAIL;
}

static node_action semantic_high_level(run_impl *r, cflow_node_id i, const cflow_node *n,
                                       cflow_value_slot *owned, const cmeta_type_desc **cur_type,
                                       const void *root_source) {
    (void)r; (void)i; (void)n; (void)owned; (void)cur_type; (void)root_source;
    return NODE_FAIL;
}

static node_action semantic_relation(run_impl *r, cflow_node_id i, const cflow_node *n,
                                     cflow_value_slot *owned, const cmeta_type_desc **cur_type,
                                     const void *root_source) {
    (void)cur_type;
    if (!r || !n || !owned || !owned->live || r->continuation_count >= CMETA_RUN_MAX_CONTINUATIONS)
        return NODE_FAIL;
    cflow_resumable machine = {0};
    if (!cflow_resumable_from_relation_with_options(
            &machine, r->graph, n, owned->storage,
            &r->eval_options)) return NODE_FAIL;
    cflow_value_slot_reset(owned);
    if (!push_continuation(r, i, machine, root_source)) {
        if (machine.ops && machine.ops->destroy) machine.ops->destroy(machine.state);
        return NODE_FAIL;
    }
    return NODE_STOP;
}

static node_action semantic_reduce(run_impl *r, cflow_node_id i, const cflow_node *n,
                                   cflow_value_slot *owned, const cmeta_type_desc **cur_type,
                                   const void *root_source) {
    (void)root_source;
    if (!r->reduce_value[i].live) {
        if (!cflow_value_slot_move(&r->reduce_value[i], owned))
            return NODE_FAIL;
    } else {
        cflow_value_slot next = {0};
        const void *args[2] = {
            r->reduce_value[i].storage, owned->storage };
        if (!cflow_value_slot_init(&next, n->output_type))
            return NODE_FAIL;
        if (n->param_kind == CFLOW_NODE_PARAM_TYPED_ADAPTER) {
            if (!cflow_graph_explicit_adapter_callable_valid(n->fn) ||
                !cmeta_type_equal(n->input_type, n->output_type) ||
                n->fn.invoke == NULL ||
                !n->fn.invoke(&n->fn, next.storage, args)) {
                cflow_value_slot_destroy(&next);
                return NODE_FAIL;
            }
        } else if (!cmeta_callable_invoke(&n->fn, next.storage, args)) {
            cflow_value_slot_destroy(&next);
            return NODE_FAIL;
        }
        next.live = true;
        cflow_value_slot_reset(&r->reduce_value[i]);
        cflow_value_slot_reset(owned);
        if (!cflow_value_slot_move(&r->reduce_value[i], &next)) {
            cflow_value_slot_destroy(&next);
            return NODE_FAIL;
        }
        cflow_value_slot_destroy(&next);
    }
    return NODE_STOP;
}

static node_action semantic_flat_map(run_impl *r, cflow_node_id i, const cflow_node *n,
                                     cflow_value_slot *owned, const cmeta_type_desc **cur_type,
                                     const void *root_source) {
    (void)cur_type;
    if (r->continuation_count >= CMETA_RUN_MAX_CONTINUATIONS) return NODE_FAIL;
    generator_state *gs = calloc(1, sizeof(*gs));
    if (!gs) return NODE_FAIL;
    gs->fn = n->fn;
    if (!cflow_value_slot_init(&gs->input, owned->type) ||
        !cflow_value_slot_move(&gs->input, owned)) {
        generator_destroy_machine(gs);
        return NODE_FAIL;
    }
    cflow_resumable machine = { "flatMap-generator", n->output_type,
                                &generator_machine_ops, gs };
    if (!push_continuation(r, i, machine, root_source)) {
        generator_destroy_machine(gs);
        return NODE_FAIL;
    }
    return NODE_STOP;
}

static node_handler handlers[CFLOW_OP_COUNT] = {
    [CFLOW_OP_RELATION] = semantic_relation,
    [CFLOW_OP_TAKE] = semantic_take,
    [CFLOW_OP_SKIP] = semantic_skip,
    [CFLOW_OP_DISTINCT] = semantic_distinct,
    [CFLOW_OP_SORTED] = semantic_sorted,
#define CFLOW_OP_ROW(E, method, margc, fnarg, childarg, farity, p0, p1, p2, ret, out, card, childrule, semantic, intrinsic_effects) \
    [CFLOW_OP_##E] = semantic_##semantic,
Replay(CFlowOperators, CFLOW_OP_ROW)
#undef CFLOW_OP_ROW
};

static bool successor_of(run_impl *r,
                         cflow_node_id node,
                         cflow_node_id *next,
                         bool *has_next) {
    if (!r || !r->subgraph || !next || !has_next) return false;
    size_t degree = cflow_subgraph_out_degree(r->subgraph, node);
    if (degree == 0) {
        *has_next = false;
        *next = CMETA_INVALID_ID;
        return true;
    }
    if (degree != 1 || !cflow_subgraph_single_successor(r->subgraph, node, next)) {
        run_fail(r, "fan-out requires an explicit RELATION semantic");
        return false;
    }
    *has_next = true;
    return true;
}

static bool process_path(run_impl *r,
                         cflow_node_id node_id,
                         const void *value,
                         const cmeta_type_desc *type,
                         const void *root_source) {
    cflow_value_slot owned = {0};
    if (!cflow_value_slot_init(&owned, type) ||
        !cflow_value_slot_copy(&owned, value)) {
        cflow_value_slot_destroy(&owned);
        run_fail(r, "value construction failed");
        return false;
    }
    const cmeta_type_desc *cur_type = type;

    cflow_node_id current = node_id;
    while (current != CMETA_INVALID_ID) {
        const cflow_node *n = cflow_subgraph_node(r->subgraph, current);
        if (!n || n->op <= CFLOW_OP_INPUT || n->op >= CFLOW_OP_COUNT || !handlers[n->op]) {
            cflow_value_slot_destroy(&owned); run_fail(r, "operator has no execution semantic"); return false;
        }
        node_action a = handlers[n->op](r, current, n, &owned, &cur_type, root_source);
        if (a == NODE_FAIL) {
            cflow_value_slot_destroy(&owned); run_fail(r, "operator semantic failed"); return false;
        }
        if (a == NODE_STOP) {
            cflow_value_slot_destroy(&owned);
            return true;
        }

        cflow_node_id next = CMETA_INVALID_ID;
        bool has_next = false;
        if (!successor_of(r, current, &next, &has_next)) { cflow_value_slot_destroy(&owned); return false; }
        current = has_next ? next : CMETA_INVALID_ID;
    }

    if (!demand_consume_one(r)) {
        cflow_value_slot_destroy(&owned); run_fail(r, "runtime produced value without demand"); return false;
    }
    if (cflow_subscriber_valid(&r->sink) && !cflow_subscriber_value(&r->sink, cur_type, owned.storage)) {
        cflow_value_slot_destroy(&owned); run_fail(r, "observer rejected value"); return false;
    }
    cflow_value_slot_destroy(&owned); return true;
}


static void wake_cb(void *user);

static bool arm_waitable(run_impl *r, cflow_waitable waitable) {
    if (!r || !cflow_waitable_valid(&waitable)) {
        run_fail(r, "WAIT step has no armable waitable");
        return false;
    }
    cmeta_mutex_lock(&r->lock);
    r->waiting = true;
    r->active_wait = waitable;
    cmeta_mutex_unlock(&r->lock);
    cflow_waker w = { wake_cb, r->owner };
    if (!cflow_waitable_arm(&waitable, w)) {
        cmeta_mutex_lock(&r->lock);
        r->waiting = false;
        memset(&r->active_wait, 0, sizeof(r->active_wait));
        cmeta_mutex_unlock(&r->lock);
        run_fail(r, "waitable arm failed");
        return false;
    }
    return true;
}

static bool resume_top_continuation(run_impl *r) {
    if (!r->continuation_count) return true;
    continuation_frame *f = &r->continuations[r->continuation_count - 1];
    const cflow_node *n = cflow_subgraph_node(r->subgraph, f->node_id);
    if (!n) { run_fail(r, "continuation references invalid node"); return false; }
    if (f->done) {
        continuation_clear(f); --r->continuation_count; return true;
    }
    if (f->output.live) {
        run_fail(r, "continuation output slot is not empty");
        return false;
    }
    cflow_step step = f->machine.ops->resume(
        f->machine.state, run_resume_context(r), f->output.storage);
    if (step.kind == CFLOW_STEP_ERROR) { run_fail(r, step.error ? step.error : "operator resumable error"); return false; }
    if (step.kind == CFLOW_STEP_WAIT) return arm_waitable(r, step.waitable);
    if (step.kind == CFLOW_STEP_DONE) { f->done = true; return true; }
    if (step.kind != CFLOW_STEP_VALUE && step.kind != CFLOW_STEP_VALUE_AND_DONE) {
        run_fail(r, "invalid operator resumable step"); return false;
    }
    f->output.live = true;
    if (step.kind == CFLOW_STEP_VALUE_AND_DONE) f->done = true;
    cflow_node_id next = CMETA_INVALID_ID;
    bool has_next = false;
    if (!successor_of(r, f->node_id, &next, &has_next)) return false;
    if (!process_path(r, has_next ? next : CMETA_INVALID_ID,
                      f->output.storage, n->output_type,
                      f->root.live ? f->root.storage : NULL)) return false;
    cflow_value_slot_reset(&f->output);
    return true;
}


static bool terminal_predecessors_done(const run_impl *r, size_t node_index) {
    size_t index;
    if (!r || !r->subgraph) return false;
    for (index = 0u; index < node_index; ++index) {
        const cflow_node *node = cflow_subgraph_node(
            r->subgraph, (cflow_node_id)index);
        const cflow_op_schema *schema = node
            ? cflow_op_schema_get(node->op) : NULL;
        if (schema && schema->cardinality == CMETA_CARD_REDUCE &&
            r->reduce_value[index].live && !r->reduce_flushed[index])
            return false;
        if (node && node->op == CFLOW_OP_SORTED &&
            !r->sequence_flushed[index])
            return false;
    }
    return true;
}

static bool flush_sorted_value(run_impl *r, size_t index,
                               const cflow_node *node) {
    cflow_value_slot value = {0};
    cmeta_gen_status range_status;
    cflow_status status;
    cflow_node_id next = CMETA_INVALID_ID;
    bool has_next = false;

    if (!r->sequence_started[index]) {
        status = r->eval_options.sequence_state->stable_sort(
            r->sequence_states[index]);
        if (status != CFLOW_STATUS_OK) {
            run_fail_status(r, status, "sorted backend sort failed");
            return false;
        }
        r->sequence_ranges[index] =
            r->eval_options.sequence_state->range(
                r->sequence_states[index]);
        if (!r->sequence_ranges[index].object ||
            !r->sequence_ranges[index].next ||
            !cmeta_type_equal(r->sequence_ranges[index].element_type,
                              node->output_type) ||
            (!cflow_value_storage_type_supported(node->output_type) &&
             (r->sequence_ranges[index].flags &
              CMETA_RANGE_CONSTRUCTS_VALUES) == 0u)) {
            run_fail_status(r, CFLOW_STATUS_EXECUTION_ERROR,
                            "sorted backend returned an invalid range");
            return false;
        }
        r->sequence_started[index] = true;
    }
    if (!cflow_value_slot_init(&value, node->output_type)) {
        run_fail_status(r, CFLOW_STATUS_ALLOCATION_FAILED,
                        "sorted output allocation failed");
        return false;
    }
    range_status = cmeta_range_next(
        &r->sequence_ranges[index], &r->sequence_cursors[index],
        value.storage);
    if (range_status == CMETA_GEN_DONE) {
        r->sequence_flushed[index] = true;
        cflow_value_slot_destroy(&value);
        return true;
    }
    if (range_status != CMETA_GEN_VALUE &&
        range_status != CMETA_GEN_VALUE_AND_DONE) {
        cflow_value_slot_destroy(&value);
        run_fail_status(
            r, CFLOW_STATUS_EXECUTION_ERROR,
            range_status == CMETA_GEN_MUTATED
                ? "sorted backend range mutated"
                : "sorted backend range failed");
        return false;
    }
    value.live = true;
    if (!successor_of(r, (cflow_node_id)index, &next, &has_next) ||
        !process_path(r, has_next ? next : CMETA_INVALID_ID,
                      value.storage, node->output_type, NULL)) {
        cflow_value_slot_destroy(&value);
        return false;
    }
    cflow_value_slot_destroy(&value);
    if (range_status == CMETA_GEN_VALUE_AND_DONE)
        r->sequence_flushed[index] = true;
    return true;
}

static bool flush_one_terminal_node(run_impl *r) {
    for (size_t i = 0; i < r->subgraph->node_count; ++i) {
        const cflow_node *node = cflow_subgraph_node(r->subgraph, (cflow_node_id)i);
        const cflow_op_schema *schema = node ? cflow_op_schema_get(node->op) : NULL;
        if (!terminal_predecessors_done(r, i)) continue;
        if (node && node->op == CFLOW_OP_SORTED &&
            !r->sequence_flushed[i])
            return flush_sorted_value(r, i, node);
        if (!schema || schema->cardinality != CMETA_CARD_REDUCE ||
            !r->reduce_value[i].live || r->reduce_flushed[i]) continue;
        r->reduce_flushed[i] = true;
        cflow_node_id next = CMETA_INVALID_ID;
        bool has_next = false;
        if (!successor_of(r, (cflow_node_id)i, &next, &has_next)) return false;
        if (!process_path(r, has_next ? next : CMETA_INVALID_ID,
                          r->reduce_value[i].storage,
                          node->output_type, NULL)) return false;
        cflow_value_slot_reset(&r->reduce_value[i]);
        return true;
    }
    return true;
}


static void finish_if_possible(run_impl *r) {
    if (!r || !r->source_done || r->continuation_count ||
        !terminal_nodes_done(r)) return;
    bool notify = false;
    cmeta_mutex_lock(&r->lock);
    if (!r->terminated) { r->terminated = true; notify = true; }
    cmeta_mutex_unlock(&r->lock);
    if (notify && cflow_subscriber_valid(&r->sink)) cflow_subscriber_done(&r->sink);
}

static cflow_status schedule_pump(
    run_impl *r, bool fail_on_rejection, bool request_published_demand);

static void wake_cb(void *user) {
    cflow_subscription *run = (cflow_subscription *)user;
    run_impl *r = impl_of(run);
    if (!r) return;
    cmeta_mutex_lock(&r->lock);
    r->waiting = false;
    memset(&r->active_wait, 0, sizeof(r->active_wait));
    cmeta_mutex_unlock(&r->lock);
    (void)schedule_pump(r, true, false);
}

void cflow_subscription_wake(cflow_subscription *run) { wake_cb(run); }

static bool process_source_value(run_impl *r,
                                 bool has_first,
                                 cflow_node_id first,
                                 const cmeta_type_desc *source_type) {
    r->source_slot.live = true;
    if (!has_first) {
        bool accepted;
        if (!demand_consume_one(r)) {
            cflow_value_slot_reset(&r->source_slot);
            run_fail(r, "runtime produced value without demand");
            return false;
        }
        accepted = !cflow_subscriber_valid(&r->sink) ||
            cflow_subscriber_value(&r->sink, source_type,
                             r->source_slot.storage);
        cflow_value_slot_reset(&r->source_slot);
        if (!accepted)
            run_fail(r, "observer rejected value");
        return accepted;
    }
    bool processed = process_path(
        r, first, r->source_slot.storage,
        source_type, r->source_slot.storage);
    cflow_value_slot_reset(&r->source_slot);
    return processed;
}

static bool process_source_step(run_impl *r) {
    const cmeta_type_desc *source_type = r->source_type;
    cflow_step step;
    cflow_node_id first = CMETA_INVALID_ID;
    bool has_first = false;

    if (r->source_slot.live) {
        run_fail(r, "source output slot is not empty");
        return false;
    }
    step = cflow_publisher_resume(&r->source, run_resume_context(r),
                               r->source_slot.storage);
    if (!r->identity_path &&
        !successor_of(r, r->subgraph->entry, &first, &has_first))
        return false;
    switch (step.kind) {
        case CFLOW_STEP_VALUE:
            return process_source_value(r, has_first, first, source_type);
        case CFLOW_STEP_VALUE_AND_DONE:
            r->source_done = true;
            return process_source_value(r, has_first, first, source_type);
        case CFLOW_STEP_WAIT:
            return arm_waitable(r, step.waitable);
        case CFLOW_STEP_DONE:
            r->source_done = true;
            return true;
        case CFLOW_STEP_ERROR:
            run_fail(r, step.error ? step.error : "source error");
            return false;
    }
    run_fail(r, "invalid source step"); return false;
}


static bool poll_source_terminal(run_impl *r) {
    if (!r || !cflow_publisher_valid(&r->source)) return false;
    const char *err = NULL;
    cflow_publisher_terminal st = cflow_publisher_poll_terminal(&r->source, &err);
    if (st == CFLOW_PUBLISHER_ERROR) { run_fail(r, err ? err : "source terminal error"); return true; }
    if (st == CFLOW_PUBLISHER_DONE) { r->source_done = true; return true; }
    return false;
}

static bool take_cancel_request(run_impl *r) {
    bool cancel = false;
    cmeta_mutex_lock(&r->lock);
    if (r->cancel_requested && !r->terminated) {
        r->cancel_requested = false;
        r->cancelled = true;
        r->terminated = true;
        cancel = true;
    }
    cmeta_mutex_unlock(&r->lock);
    return cancel;
}

static bool process_unit(run_impl *r) {
    if (r->terminated) return false;
    if (take_cancel_request(r)) {
        cflow_publisher_cancel(&r->source);
        continuations_clear(r);
        return false;
    }
    if (r->continuation_count) {
        continuation_frame *top = &r->continuations[r->continuation_count - 1];
        if (top->done) return resume_top_continuation(r); /* pop is terminal bookkeeping, not a value */
        if (demand_get(r) == 0) return false;
        return resume_top_continuation(r);
    }
    if (r->source_done) {
        if (!terminal_nodes_done(r)) {
            if (demand_get(r) == 0) return false;
            return flush_one_terminal_node(r);
        }
        finish_if_possible(r);
        return false;
    }
    if (demand_get(r) == 0) {
        if (poll_source_terminal(r)) return true;
        return false;
    }
    return process_source_step(r);
}

static void run_destroy_claimed(run_impl *r) {
    cflow_subscription *owner;
    const cflow_subscription *previous_destroy_owner;
    if (!r) return;
    owner = r->owner;
    previous_destroy_owner = active_destroy_owner;
    active_destroy_owner = owner;
    if (cflow_publisher_valid(&r->source)) cflow_publisher_destroy(&r->source);
    continuations_clear(r);
    set_states_clear(r);
    sequence_states_clear(r);
    reducers_clear(r);
    cflow_value_slot_destroy(&r->source_slot);
    cmeta_mutex_lock(&r->lock);
    r->closed = true;
    cmeta_mutex_unlock(&r->lock);
    cmeta_cond_destroy(&r->task_cv);
    cmeta_mutex_destroy(&r->lock);
    active_destroy_owner = previous_destroy_owner;

    cmeta_mutex_lock(&run_lifecycle_lock);
    if (owner && owner->impl == r) owner->impl = NULL;
    free(r);
    cmeta_cond_broadcast(&run_lifecycle_cv);
    cmeta_mutex_unlock(&run_lifecycle_lock);
}

static bool run_release_task_ref(run_impl *r) {
    bool destroy = false;
    if (!r) return false;
    cmeta_mutex_lock(&run_lifecycle_lock);
    cmeta_mutex_lock(&r->lock);
    if (r->task_refs != 0u) --r->task_refs;
    if (r->task_refs == 0u) {
        if (r->close_requested && r->owner && r->owner->impl == r &&
            !r->external_closer && !r->destroying) {
            r->destroying = true;
            destroy = true;
        }
        cmeta_cond_broadcast(&r->task_cv);
    }
    cmeta_mutex_unlock(&r->lock);
    cmeta_mutex_unlock(&run_lifecycle_lock);
    return destroy;
}

static void run_clear_scheduled_task_locked(run_impl *r) {
    r->task_scheduled = false;
    r->scheduled_task_id = 0u;
    r->scheduler_settles_cancel = false;
}

static void pump_task(void *user) {
    run_impl *r = (run_impl *)user;
    run_impl *previous_active_run;
    bool destroy;

    if (!r) return;
    previous_active_run = active_pump_run;
    active_pump_run = r;
    cmeta_mutex_lock(&r->lock);
    run_clear_scheduled_task_locked(r);
    r->rejection_must_fail = false;
    r->pump_running = true;
    cmeta_mutex_unlock(&r->lock);

    for (unsigned i = 0; i < CMETA_RUN_QUANTUM; ++i) {
        bool waiting = false;
        cmeta_mutex_lock(&r->lock);
        waiting = r->waiting;
        cmeta_mutex_unlock(&r->lock);
        if (waiting || r->terminated) break;
        if (demand_get(r) == 0 && r->continuation_count &&
            !r->continuations[r->continuation_count - 1].done) break;
        if (r->source_done && !terminal_nodes_done(r) &&
            demand_get(r) == 0) break;
        if (!process_unit(r)) break;
    }

    bool terminated = false, waiting = false;
    cmeta_mutex_lock(&r->lock);
    terminated = r->terminated;
    waiting = r->waiting;
    /* Retain exclusive pump ownership through the last source/operator read.
     * A concurrent wake may admit the next pump as soon as we unlock. */
    size_t d = demand_get(r);
    bool continuation_runnable = r->continuation_count &&
                          (r->continuations[r->continuation_count - 1].done || d > 0);
    bool runnable = r->cancel_requested || continuation_runnable ||
                    (!r->source_done && d > 0) ||
                    (r->source_done && r->continuation_count == 0 &&
                     terminal_nodes_done(r)) ||
                    (r->source_done && !terminal_nodes_done(r) && d > 0);
    r->pump_running = false;
    cmeta_mutex_unlock(&r->lock);
    if (!terminated && !waiting && runnable) (void)schedule_pump(r, true, false);

    active_pump_run = previous_active_run;
    destroy = run_release_task_ref(r);
    if (destroy) run_destroy_claimed(r);
}

static void pump_task_cancel(void *user) {
    run_impl *r = (run_impl *)user;
    run_impl *previous_active_run;
    bool cancel_source = false;
    bool destroy;

    if (!r) return;
    previous_active_run = active_pump_run;
    active_pump_run = r;
    cmeta_mutex_lock(&r->lock);
    run_clear_scheduled_task_locked(r);
    r->rejection_must_fail = false;
    if (!r->terminated) {
        r->cancel_requested = false;
        r->cancelled = true;
        r->terminated = true;
        cancel_source = true;
    }
    cmeta_mutex_unlock(&r->lock);

    if (cancel_source) {
        cflow_publisher_cancel(&r->source);
        continuations_clear(r);
    }
    active_pump_run = previous_active_run;
    destroy = run_release_task_ref(r);
    if (destroy) run_destroy_claimed(r);
}

static const char *scheduler_rejection_error(cflow_admission_status status) {
    switch (status) {
        case CFLOW_ADMISSION_FULL:
            return "scheduler is full";
        case CFLOW_ADMISSION_CLOSED:
            return "scheduler is closed";
        case CFLOW_ADMISSION_ALLOCATION_FAILED:
            return "scheduler could not allocate Run pump";
        case CFLOW_ADMISSION_INVALID_ARGUMENT:
        case CFLOW_ADMISSION_ACCEPTED:
        default:
            return "scheduler rejected Run pump";
    }
}

static cflow_status scheduler_rejection_status(
    cflow_admission_status status) {
    switch (status) {
        case CFLOW_ADMISSION_FULL:
            return CFLOW_STATUS_CAPACITY_EXCEEDED;
        case CFLOW_ADMISSION_CLOSED:
            return CFLOW_STATUS_CLOSED;
        case CFLOW_ADMISSION_ALLOCATION_FAILED:
            return CFLOW_STATUS_ALLOCATION_FAILED;
        case CFLOW_ADMISSION_INVALID_ARGUMENT:
            return CFLOW_STATUS_INVALID_ARGUMENT;
        case CFLOW_ADMISSION_ACCEPTED:
        default:
            return CFLOW_STATUS_EXECUTION_ERROR;
    }
}

static cflow_status terminal_request_status_locked(const run_impl *r) {
    if (r->closed || (!r->cancelled && r->status == CFLOW_STATUS_OK))
        return CFLOW_STATUS_CLOSED;
    return r->cancelled ? CFLOW_STATUS_CANCELLED : r->status;
}

static cflow_status schedule_pump(
    run_impl *r, bool fail_on_rejection, bool request_published_demand) {
    cflow_schedule_result result;
    const cflow_executor_task task = {
        .run = pump_task,
        .cancel = pump_task_cancel,
        .finalize = NULL,
        .user = r
    };
    bool destroy;
    bool scheduler_settles_cancel;
    bool notify = false;
    bool must_fail = false;
    uint64_t generation;
    const char *error = NULL;
    run_impl *previous_active_run;
    if (!r || !r->scheduler) return CFLOW_STATUS_INVALID_ARGUMENT;
    cmeta_mutex_lock(&r->lock);
    if (r->closed || r->terminated) {
        /* A demand admission linearizes when request_result() publishes
         * demand under r->lock. An already-running Worker may consume all
         * values and terminate normally before its caller re-enters this
         * lock for scheduling. That is a successful admitted request, NOT
         * a new request after terminal. Preserve all true Scheduler,
         * cancellation, close, and error results. */
        const bool completed_after_admission =
            request_published_demand && r->terminated &&
            !r->closed && !r->close_requested &&
            !r->cancel_requested && !r->cancelled &&
            r->status == CFLOW_STATUS_OK;
        const cflow_status status = completed_after_admission
            ? CFLOW_STATUS_OK : terminal_request_status_locked(r);
        cmeta_mutex_unlock(&r->lock);
        return status;
    }
    if (r->pump_running || r->waiting) {
        cmeta_mutex_unlock(&r->lock);
        return CFLOW_STATUS_OK;
    }
    if (r->task_scheduled) {
        r->rejection_must_fail =
            r->rejection_must_fail || fail_on_rejection;
        cmeta_mutex_unlock(&r->lock);
        return CFLOW_STATUS_OK;
    }
    if (r->task_refs > SIZE_MAX - 2u || r->task_posting == SIZE_MAX ||
        r->next_task_generation == UINT64_MAX) {
        cmeta_mutex_unlock(&r->lock);
        return CFLOW_STATUS_EXECUTION_ERROR;
    }
    r->task_scheduled = true;
    r->scheduled_task_id = 0u;
    r->scheduler_settles_cancel = false;
    generation = ++r->next_task_generation;
    r->scheduled_task_generation = generation;
    r->rejection_must_fail = fail_on_rejection;
    ++r->task_posting;
    r->task_refs += 2u;
    cmeta_mutex_unlock(&r->lock);
    scheduler_settles_cancel =
        cflow_scheduler_try_post_task_after_internal(
            r->scheduler, 0u, &task, &result);
    if (!scheduler_settles_cancel) {
        result = cflow_scheduler_try_post_after(
            r->scheduler, 0u, pump_task, r);
    }
    cmeta_mutex_lock(&r->lock);
    --r->task_posting;
    if (result.status == CFLOW_ADMISSION_ACCEPTED && result.task_id != 0u) {
        if (r->task_scheduled &&
            r->scheduled_task_generation == generation) {
            r->scheduled_task_id = result.task_id;
            r->scheduler_settles_cancel = scheduler_settles_cancel;
        }
    } else if (r->task_scheduled &&
               r->scheduled_task_generation == generation) {
        run_clear_scheduled_task_locked(r);
        must_fail = r->rejection_must_fail;
        r->rejection_must_fail = false;
        if (must_fail && !r->cancel_requested &&
            !r->close_requested && !r->terminated) {
            error = scheduler_rejection_error(result.status);
            r->error = error;
            r->status = scheduler_rejection_status(result.status);
            r->terminated = true;
            notify = true;
        }
    }
    cmeta_cond_broadcast(&r->task_cv);
    cmeta_mutex_unlock(&r->lock);
    if (result.status != CFLOW_ADMISSION_ACCEPTED || result.task_id == 0u) {
        /* The rejected task and posting refs protect r through synchronous
         * Sink delivery; the TLS marker preserves callback-close deferral. */
        previous_active_run = active_pump_run;
        active_pump_run = r;
        if (notify && cflow_subscriber_valid(&r->sink))
            cflow_subscriber_error(&r->sink, error);

        active_pump_run = previous_active_run;
        destroy = run_release_task_ref(r);
        if (!destroy) destroy = run_release_task_ref(r);
        if (destroy) run_destroy_claimed(r);
        return scheduler_rejection_status(result.status);
    }
    destroy = run_release_task_ref(r);
    if (destroy) run_destroy_claimed(r);
    return CFLOW_STATUS_OK;
}


static cflow_status backend_admit_graph(
    const cflow_graph *graph,
    const cflow_eval_options *options) {
    if (!graph) return CFLOW_STATUS_INVALID_ARGUMENT;
    for (size_t subgraph_id = 0u;
         subgraph_id < graph->subgraph_count; ++subgraph_id) {
        const cflow_subgraph *subgraph = cflow_graph_subgraph(
            graph, (cflow_subgraph_id)subgraph_id);
        if (!subgraph) return CFLOW_STATUS_INVALID_ARGUMENT;
        for (size_t i = 0u; i < subgraph->node_count; ++i) {
            const cflow_node *node = cflow_subgraph_node(
                subgraph, (cflow_node_id)i);
            if (!node) return CFLOW_STATUS_INVALID_ARGUMENT;
            if (node->op == CFLOW_OP_DISTINCT &&
                (!options ||
                 !cflow_set_state_ops_valid(options->set_state)))
                return CFLOW_STATUS_UNSUPPORTED;
            if (node->op == CFLOW_OP_SORTED &&
                (!options || !cflow_sequence_state_ops_valid(
                    options->sequence_state)))
                return CFLOW_STATUS_UNSUPPORTED;
        }
    }
    return CFLOW_STATUS_OK;
}

cflow_status_result cflow_subscribe_subgraph_with_options(
    cflow_subscription *run,
    const cflow_graph *graph,
    cflow_subgraph_id subgraph_id,
    cflow_publisher *source,
    cflow_scheduler *scheduler,
    const cflow_subscriber *sink,
    const cflow_eval_options *options) {
    const cflow_subgraph *subgraph;
    const cmeta_type_desc *source_type;
    const char *validation_error = NULL;
    cflow_status backend_status;

    if (!run || run->impl || !graph || !scheduler || !source ||
        !cflow_publisher_valid(source) || !cflow_graph_is_normalized(graph))
        return (cflow_status_result){CFLOW_STATUS_INVALID_ARGUMENT};
    subgraph = cflow_graph_subgraph(graph, subgraph_id);
    source_type = cflow_publisher_output_type(source);
    if (!subgraph || !source_type)
        return (cflow_status_result){CFLOW_STATUS_INVALID_ARGUMENT};
    if (!cflow_graph_validate(graph, &validation_error))
        return (cflow_status_result){CFLOW_STATUS_INVALID_ARGUMENT};
    if (!cflow_value_type_supported(source_type) ||
        !cflow_value_runtime_graph_supported(graph) ||
        (!cflow_value_storage_type_supported(source_type) &&
         !cflow_publisher_has(source, CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES)))
        return (cflow_status_result){CFLOW_STATUS_UNSUPPORTED};
    if (!cmeta_type_equal(cflow_subgraph_input_type(graph, subgraph_id),
                          source_type))
        return (cflow_status_result){CFLOW_STATUS_TYPE_MISMATCH};
    backend_status = backend_admit_graph(graph, options);
    if (backend_status != CFLOW_STATUS_OK)
        return (cflow_status_result){backend_status};
    if (!run_lifecycle_ensure())
        return (cflow_status_result){CFLOW_STATUS_EXECUTION_ERROR};

    run_impl *r = calloc(1, sizeof(*r));
    if (!r) return (cflow_status_result){CFLOW_STATUS_ALLOCATION_FAILED};
    cmeta_mutex_init(&r->lock);
    if (!r->lock) {
        free(r);
        return (cflow_status_result){CFLOW_STATUS_ALLOCATION_FAILED};
    }
    cmeta_cond_init(&r->task_cv);
    if (!r->task_cv) {
        cmeta_mutex_destroy(&r->lock);
        free(r);
        return (cflow_status_result){CFLOW_STATUS_ALLOCATION_FAILED};
    }
    r->owner = run;
    r->graph = graph;
    r->subgraph_id = subgraph_id;
    r->subgraph = subgraph;
    r->status = CFLOW_STATUS_OK;
    if (options) r->eval_options = *options;
    r->source = *source;
    r->source_type = source_type;
    r->scheduler = scheduler;
    r->resume_ctx.scheduler = scheduler;
    if (sink) r->sink = *sink;
    atomic_init(&r->demand, 0u);
    atomic_init(&r->source_done, false);
    atomic_init(&r->terminated, false);
    r->identity_path =
        cflow_subgraph_out_degree(subgraph, subgraph->entry) == 0u;
    if (!cflow_value_slot_init(&r->source_slot, source_type)) {
        cmeta_cond_destroy(&r->task_cv);
        cmeta_mutex_destroy(&r->lock);
        free(r);
        return (cflow_status_result){CFLOW_STATUS_ALLOCATION_FAILED};
    }
    r->reduce_value = calloc(subgraph->node_count ? subgraph->node_count : 1, sizeof(*r->reduce_value));
    r->reduce_flushed = calloc(subgraph->node_count ? subgraph->node_count : 1, sizeof(*r->reduce_flushed));
    r->slice_counts = calloc(subgraph->node_count ? subgraph->node_count : 1,
                             sizeof(*r->slice_counts));
    r->set_states = calloc(subgraph->node_count ? subgraph->node_count : 1,
                           sizeof(*r->set_states));
    r->sequence_states = calloc(
        subgraph->node_count ? subgraph->node_count : 1,
        sizeof(*r->sequence_states));
    r->sequence_ranges = calloc(
        subgraph->node_count ? subgraph->node_count : 1,
        sizeof(*r->sequence_ranges));
    r->sequence_cursors = calloc(
        subgraph->node_count ? subgraph->node_count : 1,
        sizeof(*r->sequence_cursors));
    r->sequence_started = calloc(
        subgraph->node_count ? subgraph->node_count : 1,
        sizeof(*r->sequence_started));
    r->sequence_flushed = calloc(
        subgraph->node_count ? subgraph->node_count : 1,
        sizeof(*r->sequence_flushed));
    if (!r->reduce_value || !r->reduce_flushed || !r->slice_counts ||
        !r->set_states || !r->sequence_states || !r->sequence_ranges ||
        !r->sequence_cursors || !r->sequence_started ||
        !r->sequence_flushed) {
        cflow_value_slot_destroy(&r->source_slot); reducers_clear(r);
        set_states_clear(r);
        sequence_states_clear(r);
        cmeta_cond_destroy(&r->task_cv);
        cmeta_mutex_destroy(&r->lock);
        free(r);
        return (cflow_status_result){CFLOW_STATUS_ALLOCATION_FAILED};
    }
    for (size_t i = 0; i < subgraph->node_count; ++i) {
        const cflow_node *node = cflow_subgraph_node(
            subgraph, (cflow_node_id)i);
        const cflow_op_schema *schema = node
            ? cflow_op_schema_get(node->op) : NULL;
        if (schema && schema->cardinality == CMETA_CARD_REDUCE) {
            if (!cflow_value_slot_init(&r->reduce_value[i],
                                       node->output_type)) {
                cflow_value_slot_destroy(&r->source_slot);
                set_states_clear(r);
                sequence_states_clear(r);
                reducers_clear(r);
                cmeta_cond_destroy(&r->task_cv);
                cmeta_mutex_destroy(&r->lock);
                free(r);
                return (cflow_status_result){CFLOW_STATUS_ALLOCATION_FAILED};
            }
            if (node->param_kind == CFLOW_NODE_PARAM_REDUCE_SEED &&
                !cflow_value_slot_copy(
                    &r->reduce_value[i], cflow_node_reduce_seed(node))) {
                cflow_value_slot_destroy(&r->source_slot);
                set_states_clear(r);
                sequence_states_clear(r);
                reducers_clear(r);
                cmeta_cond_destroy(&r->task_cv);
                cmeta_mutex_destroy(&r->lock);
                free(r);
                return (cflow_status_result){CFLOW_STATUS_EXECUTION_ERROR};
            }
        }
        if (node && node->op == CFLOW_OP_DISTINCT) {
            cflow_status state_status = r->eval_options.set_state->open(
                &r->set_states[i], node->input_type,
                node->params.distinct.max_unique);
            if (state_status != CFLOW_STATUS_OK || !r->set_states[i]) {
                set_states_clear(r);
                cflow_value_slot_destroy(&r->source_slot);
                reducers_clear(r);
                cmeta_cond_destroy(&r->task_cv);
                cmeta_mutex_destroy(&r->lock);
                free(r);
                return (cflow_status_result){
                    state_status == CFLOW_STATUS_OK
                        ? CFLOW_STATUS_EXECUTION_ERROR : state_status};
            }
        }
        if (node && node->op == CFLOW_OP_SORTED) {
            cflow_status state_status =
                r->eval_options.sequence_state->open(
                    &r->sequence_states[i], node->input_type,
                    node->params.sorted.max_elements);
            if (state_status != CFLOW_STATUS_OK ||
                !r->sequence_states[i]) {
                sequence_states_clear(r);
                set_states_clear(r);
                cflow_value_slot_destroy(&r->source_slot);
                reducers_clear(r);
                cmeta_cond_destroy(&r->task_cv);
                cmeta_mutex_destroy(&r->lock);
                free(r);
                return (cflow_status_result){
                    state_status == CFLOW_STATUS_OK
                        ? CFLOW_STATUS_EXECUTION_ERROR : state_status};
            }
        }
    }
    run->impl = r;
    memset(source, 0, sizeof(*source));
    {
        cflow_waker w = { wake_cb, run };
        cflow_publisher_bind_terminal_waker(&r->source, w);
    }
    for (size_t i = 0; i < subgraph->node_count; ++i) {
        const cflow_node *node = cflow_subgraph_node(
            subgraph, (cflow_node_id)i);
        if (node && node->op == CFLOW_OP_TAKE &&
            node->has_size_parameter && node->size_parameter == 0u) {
            r->source_done = true;
            cflow_publisher_cancel(&r->source);
            break;
        }
    }
    return (cflow_status_result){CFLOW_STATUS_OK};
}

bool cflow_subscribe_subgraph(cflow_subscription *run,
                             const cflow_graph *graph,
                             cflow_subgraph_id subgraph_id,
                             cflow_publisher *source,
                             cflow_scheduler *scheduler,
                             const cflow_subscriber *sink) {
    return cflow_status_result_is_ok(cflow_subscribe_subgraph_with_options(
        run, graph, subgraph_id, source, scheduler, sink, NULL));
}

bool cflow_subscribe(cflow_subscription *run,
                    const cflow_graph *graph,
                    cflow_publisher *source,
                    cflow_scheduler *scheduler,
                    const cflow_subscriber *sink) {
    return cflow_status_result_is_ok(cflow_subscribe_with_options(
        run, graph, source, scheduler, sink, NULL));
}

cflow_status_result cflow_subscribe_with_options(
    cflow_subscription *run,
    const cflow_graph *graph,
    cflow_publisher *source,
    cflow_scheduler *scheduler,
    const cflow_subscriber *sink,
    const cflow_eval_options *options) {
    if (!graph || graph->root >= graph->subgraph_count)
        return (cflow_status_result){CFLOW_STATUS_INVALID_ARGUMENT};
    return cflow_subscribe_subgraph_with_options(
        run, graph, graph->root, source, scheduler, sink, options);
}


cflow_status_result cflow_subscription_request_result(
    cflow_subscription *run, size_t n) {
    run_impl *r = impl_of(run);
    size_t current;
    size_t next;
    cflow_status status;
    if (!r || n == 0)
        return (cflow_status_result){CFLOW_STATUS_INVALID_ARGUMENT};
    cmeta_mutex_lock(&r->lock);
    if (r->closed || r->terminated) {
        status = terminal_request_status_locked(r);
        cmeta_mutex_unlock(&r->lock);
        return (cflow_status_result){status};
    }
    current = atomic_load_explicit(&r->demand, memory_order_relaxed);
    do {
        next = SIZE_MAX - current < n ? SIZE_MAX : current + n;
    } while (!atomic_compare_exchange_weak_explicit(
        &r->demand, &current, next,
        memory_order_release, memory_order_relaxed));
    cmeta_mutex_unlock(&r->lock);
    return (cflow_status_result){schedule_pump(r, false, true)};
}

bool cflow_subscription_request(cflow_subscription *run, size_t n) {
    return cflow_status_result_is_ok(
        cflow_subscription_request_result(run, n));
}

void cflow_subscription_cancel(cflow_subscription *run) {
    run_impl *r = impl_of(run);
    if (!r) return;
    cflow_waitable wait = {0};
    cmeta_mutex_lock(&r->lock);
    r->cancel_requested = true;
    if (r->waiting) { wait = r->active_wait; r->waiting = false; memset(&r->active_wait, 0, sizeof(r->active_wait)); }
    cmeta_mutex_unlock(&r->lock);
    if (cflow_waitable_valid(&wait)) cflow_waitable_cancel(&wait);
    (void)schedule_pump(r, false, false);
}

void cflow_subscription_close(cflow_subscription *run) {
    run_impl *r;
    bool initiate_close = false;
    uint64_t cancel_attempted_generation = 0u;

    if (!run || active_destroy_owner == run || !run_lifecycle_ensure()) return;
    cmeta_mutex_lock(&run_lifecycle_lock);
    for (;;) {
        r = (run_impl *)run->impl;
        if (!r) {
            cmeta_mutex_unlock(&run_lifecycle_lock);
            return;
        }
        if (active_pump_run == r) break;
        if (!r->external_closer && !r->destroying) {
            r->external_closer = true;
            break;
        }
        cmeta_cond_wait(&run_lifecycle_cv, &run_lifecycle_lock);
    }
    cmeta_mutex_unlock(&run_lifecycle_lock);

    cmeta_mutex_lock(&r->lock);
    initiate_close = !r->close_requested;
    r->close_requested = true;
    cmeta_mutex_unlock(&r->lock);
    if (initiate_close) {
        cflow_subscription_cancel(run);
        cflow_publisher_bind_terminal_waker(&r->source, (cflow_waker){0});
    }

    if (active_pump_run == r) return;

    unsigned caps = cflow_scheduler_capabilities(r->scheduler);
    for (;;) {
        size_t refs = 0;
        cflow_task_id task_id = 0u;
        uint64_t task_generation = 0u;
        bool scheduler_settles_cancel = false;
        cmeta_mutex_lock(&r->lock);
        refs = r->task_refs;
        if (!refs) { cmeta_mutex_unlock(&r->lock); break; }
        if (r->task_posting != 0u) {
            cmeta_cond_wait(&r->task_cv, &r->lock);
            cmeta_mutex_unlock(&r->lock);
            continue;
        }
        if (r->task_scheduled && r->scheduled_task_id != 0u &&
            r->scheduled_task_generation != cancel_attempted_generation) {
            task_id = r->scheduled_task_id;
            task_generation = r->scheduled_task_generation;
            scheduler_settles_cancel = r->scheduler_settles_cancel;
        }
        /* Concurrent admission does not always imply an independent worker.
         * The borrowed owner Scheduler must cooperatively run queued work
         * when close is invoked on that same owner, not wait for itself. */
        if (task_id == 0u && (caps & CMETA_SCHED_CAP_CONCURRENT) &&
            !cflow_scheduler_owner_is_thread_internal(r->scheduler)) {
            cmeta_cond_wait(&r->task_cv, &r->lock);
            cmeta_mutex_unlock(&r->lock);
            continue;
        }
        cmeta_mutex_unlock(&r->lock);
        if (task_id != 0u) {
            cancel_attempted_generation = task_generation;
            /* Built-ins own the descriptor cancel callback. A foreign
             * Scheduler only removes fn/user, so Run settles that task ref. */
            if (cflow_scheduler_cancel(r->scheduler, task_id) &&
                !scheduler_settles_cancel)
                pump_task_cancel(r);
            continue;
        }
        (void)cflow_scheduler_run_until_idle(r->scheduler, 0);
    }

    cmeta_mutex_lock(&run_lifecycle_lock);
    if (run->impl == r && !r->destroying) r->destroying = true;
    cmeta_mutex_unlock(&run_lifecycle_lock);
    run_destroy_claimed(r);
}

bool cflow_subscription_is_done(const cflow_subscription *run) {
    run_impl *r = impl_of(run); bool v = false;
    if (r) {
        cmeta_mutex_lock(&r->lock);
        v = r->terminated && !r->cancelled && !r->error;
        cmeta_mutex_unlock(&r->lock);
    }
    return v;
}
bool cflow_subscription_is_cancelled(const cflow_subscription *run) {
    run_impl *r = impl_of(run); bool v = false;
    if (r) { cmeta_mutex_lock(&r->lock); v = r->cancelled; cmeta_mutex_unlock(&r->lock); }
    return v;
}
const char *cflow_subscription_error(const cflow_subscription *run) {
    run_impl *r = impl_of(run); const char *v = "run is null";
    if (r) { cmeta_mutex_lock(&r->lock); v = r->error; cmeta_mutex_unlock(&r->lock); }
    return v;
}
cflow_status cflow_subscription_status(const cflow_subscription *run) {
    run_impl *r = impl_of(run);
    cflow_status status = CFLOW_STATUS_INVALID_ARGUMENT;
    if (r) {
        cmeta_mutex_lock(&r->lock);
        status = r->cancelled && r->status == CFLOW_STATUS_OK
            ? CFLOW_STATUS_CANCELLED : r->status;
        cmeta_mutex_unlock(&r->lock);
    }
    return status;
}
size_t cflow_subscription_outstanding_demand(const cflow_subscription *run) {
    run_impl *r = impl_of(run); return r ? demand_get(r) : 0;
}
