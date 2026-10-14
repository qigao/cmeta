#include <cflow/cflow.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <cflow/opt.h>
#include "cflow_test_ops.h"
#include "../src/value_storage.h"
#include "tinytest.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RUNTIME_SATURATION_TIMEOUT_MS = 5000
};

typedef struct lifecycle_test_value {
    int *resource;
} lifecycle_test_value;

typedef struct coalescing_scheduler_state {
    atomic_bool entered;
    atomic_bool release;
    atomic_bool timed_out;
    atomic_int posts;
    uint64_t timeout_ms;
    cflow_admission_status rejection_status;
} coalescing_scheduler_state;

typedef struct coalescing_request_context {
    cflow_subscription *run;
    atomic_bool returned;
    bool result;
} coalescing_request_context;

typedef struct coalescing_close_context {
    cflow_subscription *run;
    atomic_int started;
    atomic_int returned;
} coalescing_close_context;

typedef struct rejection_callback_close_state {
    cflow_subscription *run;
    atomic_int errors;
    atomic_bool close_returned;
} rejection_callback_close_state;

typedef struct pending_foreign_scheduler_state {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    cflow_task_fn pending_fn;
    void *pending_user;
    cflow_task_id pending_id;
    cflow_task_id next_id;
    cflow_task_id last_cancelled_id;
    size_t post_calls;
    size_t block_on_post;
    bool release_blocked_post;
    bool run_inline;
    atomic_int blocked_posts;
    atomic_int cancel_calls;
} pending_foreign_scheduler_state;

typedef struct pending_foreign_run_context {
    pending_foreign_scheduler_state *state;
    atomic_int returned;
    bool ran;
} pending_foreign_run_context;

static cflow_schedule_result pending_foreign_try_post_after(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    pending_foreign_scheduler_state *state =
        (pending_foreign_scheduler_state *)self;
    cflow_task_id id;
    (void)delay;
    if (state == NULL || fn == NULL)
        return (cflow_schedule_result){CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    cmeta_mutex_lock(&state->mutex);
    if (state->pending_fn != NULL) {
        cmeta_mutex_unlock(&state->mutex);
        return (cflow_schedule_result){CFLOW_ADMISSION_FULL, 0u};
    }
    ++state->post_calls;
    if (state->block_on_post == state->post_calls) {
        atomic_fetch_add(&state->blocked_posts, 1);
        cmeta_cond_broadcast(&state->changed);
        while (!state->release_blocked_post)
            cmeta_cond_wait(&state->changed, &state->mutex);
    }
    id = ++state->next_id;
    if (state->run_inline) {
        cmeta_mutex_unlock(&state->mutex);
        fn(user);
        return (cflow_schedule_result){CFLOW_ADMISSION_ACCEPTED, id};
    }
    state->pending_fn = fn;
    state->pending_user = user;
    state->pending_id = id;
    cmeta_mutex_unlock(&state->mutex);
    return (cflow_schedule_result){CFLOW_ADMISSION_ACCEPTED, id};
}

static cflow_task_id pending_foreign_post_after(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    return pending_foreign_try_post_after(self, delay, fn, user).task_id;
}

static bool pending_foreign_cancel(void *self, cflow_task_id id) {
    pending_foreign_scheduler_state *state =
        (pending_foreign_scheduler_state *)self;
    bool cancelled = false;
    if (state == NULL || id == 0u) return false;
    cmeta_mutex_lock(&state->mutex);
    if (state->pending_fn != NULL && state->pending_id == id) {
        state->pending_fn = NULL;
        state->pending_user = NULL;
        state->pending_id = 0u;
        state->last_cancelled_id = id;
        cancelled = true;
    }
    cmeta_mutex_unlock(&state->mutex);
    if (cancelled) atomic_fetch_add(&state->cancel_calls, 1);
    return cancelled;
}

static bool pending_foreign_run_one(void *self) {
    pending_foreign_scheduler_state *state =
        (pending_foreign_scheduler_state *)self;
    cflow_task_fn fn;
    void *user;
    if (state == NULL) return false;
    cmeta_mutex_lock(&state->mutex);
    fn = state->pending_fn;
    user = state->pending_user;
    state->pending_fn = NULL;
    state->pending_user = NULL;
    state->pending_id = 0u;
    cmeta_mutex_unlock(&state->mutex);
    if (fn == NULL) return false;
    fn(user);
    return true;
}

static void pending_foreign_run_one_thread(void *user) {
    pending_foreign_run_context *context =
        (pending_foreign_run_context *)user;
    context->ran = pending_foreign_run_one(context->state);
    atomic_store(&context->returned, 1);
}

static void pending_foreign_release_blocked_post(
    pending_foreign_scheduler_state *state) {
    cmeta_mutex_lock(&state->mutex);
    state->release_blocked_post = true;
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->mutex);
}

static size_t pending_foreign_run_ready(void *self) {
    return pending_foreign_run_one(self) ? 1u : 0u;
}

static size_t pending_foreign_advance(void *self, uint64_t ticks) {
    (void)ticks;
    return pending_foreign_run_ready(self);
}

static size_t pending_foreign_run_until_idle(void *self, size_t max_steps) {
    size_t ran = 0u;
    while ((max_steps == 0u || ran < max_steps) &&
           pending_foreign_run_one(self))
        ++ran;
    return ran;
}

static bool pending_foreign_wait_idle(void *self) {
    pending_foreign_scheduler_state *state =
        (pending_foreign_scheduler_state *)self;
    bool idle;
    if (state == NULL) return false;
    cmeta_mutex_lock(&state->mutex);
    idle = state->pending_fn == NULL;
    cmeta_mutex_unlock(&state->mutex);
    return idle;
}

static uint64_t pending_foreign_now(void *self) {
    (void)self;
    return 0u;
}

static size_t pending_foreign_pending(void *self) {
    return pending_foreign_wait_idle(self) ? 0u : 1u;
}

static bool pending_foreign_shutdown(void *self) {
    return self != NULL;
}

static bool pending_foreign_get_stats(void *self,
                                      cflow_scheduler_stats *out) {
    if (self == NULL || out == NULL) return false;
    *out = (cflow_scheduler_stats){0};
    out->ready_pending = pending_foreign_pending(self);
    return true;
}

static void pending_foreign_destroy(void *self) {
    (void)self;
}

CMETA_IMPLEMENTS(cflow_scheduler, pending_foreign_scheduler,
    CMETA_SCHED_CAP_CONCURRENT,
    .try_post_after = pending_foreign_try_post_after,
    .post_after = pending_foreign_post_after,
    .cancel = pending_foreign_cancel,
    .run_one = pending_foreign_run_one,
    .run_ready = pending_foreign_run_ready,
    .advance = pending_foreign_advance,
    .run_until_idle = pending_foreign_run_until_idle,
    .wait_idle = pending_foreign_wait_idle,
    .now = pending_foreign_now,
    .pending = pending_foreign_pending,
    .shutdown = pending_foreign_shutdown,
    .get_stats = pending_foreign_get_stats,
    .destroy = pending_foreign_destroy
);

static cflow_schedule_result coalescing_try_post_after(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    coalescing_scheduler_state *state = (coalescing_scheduler_state *)self;
    (void)delay;
    (void)user;
    if (state == NULL || fn == NULL)
        return (cflow_schedule_result){CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    atomic_fetch_add(&state->posts, 1);
    atomic_store(&state->entered, true);
    {
        const uint64_t started = cmeta_monotonic_ms();
        const uint64_t timeout = state->timeout_ms != 0u
            ? state->timeout_ms : RUNTIME_SATURATION_TIMEOUT_MS;
        while (cmeta_monotonic_ms() - started < timeout &&
               !atomic_load(&state->release))
            cmeta_sleep_ms(1u);
    }
    if (!atomic_load(&state->release))
        atomic_store(&state->timed_out, true);
    return (cflow_schedule_result){
        state->rejection_status == CFLOW_ADMISSION_ACCEPTED
            ? CFLOW_ADMISSION_FULL : state->rejection_status,
        0u};
}

static void coalescing_barrier_task(void *user) { (void)user; }

static cflow_task_id coalescing_post_after(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    return coalescing_try_post_after(self, delay, fn, user).task_id;
}
static bool coalescing_cancel(void *self, cflow_task_id id) {
    (void)self; (void)id; return false;
}
static bool coalescing_run_one(void *self) { (void)self; return false; }
static size_t coalescing_run_ready(void *self) { (void)self; return 0u; }
static size_t coalescing_advance(void *self, uint64_t ticks) {
    (void)self; (void)ticks; return 0u;
}
static size_t coalescing_run_until_idle(void *self, size_t steps) {
    (void)self; (void)steps; return 0u;
}
static bool coalescing_wait_idle(void *self) { (void)self; return true; }
static uint64_t coalescing_now(void *self) { (void)self; return 0u; }
static size_t coalescing_pending(void *self) { (void)self; return 0u; }
static bool coalescing_shutdown(void *self) { (void)self; return true; }
static bool coalescing_get_stats(void *self, cflow_scheduler_stats *out) {
    (void)self;
    if (out == NULL) return false;
    *out = (cflow_scheduler_stats){0};
    return true;
}
static void coalescing_destroy(void *self) { (void)self; }

CMETA_IMPLEMENTS(cflow_scheduler, coalescing_scheduler,
    CMETA_SCHED_CAP_CONCURRENT,
    .try_post_after = coalescing_try_post_after,
    .post_after = coalescing_post_after,
    .cancel = coalescing_cancel,
    .run_one = coalescing_run_one,
    .run_ready = coalescing_run_ready,
    .advance = coalescing_advance,
    .run_until_idle = coalescing_run_until_idle,
    .wait_idle = coalescing_wait_idle,
    .now = coalescing_now,
    .pending = coalescing_pending,
    .shutdown = coalescing_shutdown,
    .get_stats = coalescing_get_stats,
    .destroy = coalescing_destroy
);

static void coalescing_request(void *user) {
    coalescing_request_context *context =
        (coalescing_request_context *)user;
    context->result = cflow_subscription_request(context->run, 1u);
    atomic_store(&context->returned, true);
}

static void coalescing_close(void *user) {
    coalescing_close_context *context = (coalescing_close_context *)user;
    atomic_store(&context->started, 1);
    cflow_subscription_close(context->run);
    atomic_store(&context->returned, 1);
}

static void rejection_callback_close_error(void *user, const char *message) {
    rejection_callback_close_state *state =
        (rejection_callback_close_state *)user;
    if (state == NULL || message == NULL) return;
    atomic_fetch_add(&state->errors, 1);
    cflow_subscription_close(state->run);
    atomic_store(&state->close_returned, true);
}

static size_t lifecycle_test_copies;
static size_t lifecycle_test_moves;
static size_t lifecycle_test_destroys;
static bool lifecycle_test_copy_fails;
static size_t lifecycle_test_copy_fail_at;

static bool lifecycle_test_copy(void *destination_, const void *source_) {
    lifecycle_test_value *destination = (lifecycle_test_value *)destination_;
    const lifecycle_test_value *source =
        (const lifecycle_test_value *)source_;

    ++lifecycle_test_copies;
    destination->resource = NULL;
    if (lifecycle_test_copy_fails ||
        (lifecycle_test_copy_fail_at != 0u &&
         lifecycle_test_copies == lifecycle_test_copy_fail_at))
        return false;
    if (!source->resource)
        return true;
    destination->resource = (int *)malloc(sizeof(*destination->resource));
    if (!destination->resource)
        return false;
    *destination->resource = *source->resource;
    return true;
}

static void lifecycle_test_move(void *destination_, void *source_) {
    lifecycle_test_value *destination = (lifecycle_test_value *)destination_;
    lifecycle_test_value *source = (lifecycle_test_value *)source_;

    ++lifecycle_test_moves;
    destination->resource = source->resource;
    source->resource = NULL;
}

static void lifecycle_test_destroy(void *value_) {
    lifecycle_test_value *value = (lifecycle_test_value *)value_;

    ++lifecycle_test_destroys;
    free(value->resource);
    value->resource = NULL;
}

static const cmeta_type_traits lifecycle_test_traits = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .copy_construct = lifecycle_test_copy,
    .move_construct = lifecycle_test_move,
    .destroy = lifecycle_test_destroy
};

static const cmeta_type_desc lifecycle_test_type = {
    .name = "lifecycle_test_value",
    .size = sizeof(lifecycle_test_value),
    .align = _Alignof(lifecycle_test_value),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &lifecycle_test_traits,
    .identity = NULL
};

static size_t tracked_int_copies;
static size_t tracked_int_moves;
static size_t tracked_int_destroys;
static size_t tracked_long_copies;
static size_t tracked_long_moves;
static size_t tracked_long_destroys;

static bool tracked_int_copy(void *destination, const void *source) {
    ++tracked_int_copies;
    memcpy(destination, source, sizeof(int));
    return true;
}

static void tracked_int_move(void *destination, void *source) {
    ++tracked_int_moves;
    memcpy(destination, source, sizeof(int));
    memset(source, 0, sizeof(int));
}

static void tracked_int_destroy(void *value) {
    ++tracked_int_destroys;
    memset(value, 0, sizeof(int));
}

static bool tracked_long_copy(void *destination, const void *source) {
    ++tracked_long_copies;
    memcpy(destination, source, sizeof(long));
    return true;
}

static void tracked_long_move(void *destination, void *source) {
    ++tracked_long_moves;
    memcpy(destination, source, sizeof(long));
    memset(source, 0, sizeof(long));
}

static void tracked_long_destroy(void *value) {
    ++tracked_long_destroys;
    memset(value, 0, sizeof(long));
}

static const cmeta_type_traits tracked_int_traits = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .copy_construct = tracked_int_copy,
    .move_construct = tracked_int_move,
    .destroy = tracked_int_destroy
};

static const cmeta_type_traits tracked_long_traits = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .copy_construct = tracked_long_copy,
    .move_construct = tracked_long_move,
    .destroy = tracked_long_destroy
};

cmeta_function(reduce, associative, long, runtime_sum_long,
      (long left, long right)) {
    return left + right;
}

typedef struct tracked_long_sink_state {
    long value;
    size_t values;
    const char *error;
    bool done;
} tracked_long_sink_state;

static bool tracked_long_sink_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    tracked_long_sink_state *state = (tracked_long_sink_state *)user;
    if (!state || !value || !cmeta_type_equal(type, &cmeta_type_long))
        return false;
    state->value = *(const long *)value;
    ++state->values;
    return true;
}

static void tracked_long_sink_error(void *user, const char *message) {
    tracked_long_sink_state *state = (tracked_long_sink_state *)user;
    if (state) state->error = message;
}

static void tracked_long_sink_done(void *user) {
    tracked_long_sink_state *state = (tracked_long_sink_state *)user;
    if (state) state->done = true;
}

static const cmeta_type_traits lifecycle_overaligned_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};

static const cmeta_type_desc lifecycle_overaligned_type = {
    .name = "lifecycle_overaligned",
    .size = 64u,
    .align = 64u,
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &lifecycle_overaligned_traits,
    .identity = NULL
};

static lifecycle_test_value lifecycle_test_make(int value) {
    lifecycle_test_value out = {0};

    out.resource = (int *)malloc(sizeof(*out.resource));
    if (out.resource)
        *out.resource = value;
    return out;
}

static void lifecycle_test_reset(void) {
    lifecycle_test_copies = 0u;
    lifecycle_test_moves = 0u;
    lifecycle_test_destroys = 0u;
    lifecycle_test_copy_fails = false;
    lifecycle_test_copy_fail_at = 0u;
}

typedef struct lifecycle_range_owner {
    lifecycle_test_value values[2];
    size_t count;
} lifecycle_range_owner;

typedef struct lifecycle_collect_output {
    lifecycle_test_value values[2];
    size_t count;
} lifecycle_collect_output;

typedef struct lifecycle_collector_state {
    lifecycle_collect_output staged;
    lifecycle_collect_output *output;
    size_t aborts;
} lifecycle_collector_state;

static size_t lifecycle_range_size(const void *object) {
    const lifecycle_range_owner *owner =
        (const lifecycle_range_owner *)object;

    return owner ? owner->count : 0u;
}

static cmeta_gen_status lifecycle_range_next(const void *object,
                                              cmeta_range_cursor *cursor,
                                              void *out_value) {
    const lifecycle_range_owner *owner =
        (const lifecycle_range_owner *)object;

    if (!owner || !cursor || !out_value)
        return CMETA_GEN_ERROR;
    if (cursor->index >= owner->count)
        return CMETA_GEN_DONE;
    if (!lifecycle_test_copy(out_value, &owner->values[cursor->index]))
        return CMETA_GEN_ERROR;
    ++cursor->index;
    return cursor->index == owner->count ? CMETA_GEN_VALUE_AND_DONE
                                         : CMETA_GEN_VALUE;
}

static cmeta_status lifecycle_collector_begin(
    void *context, const cmeta_type_desc *input, size_t limit) {
    lifecycle_collector_state *state =
        (lifecycle_collector_state *)context;

    if (!state || !state->output ||
        !cmeta_type_equal(input, &lifecycle_test_type) || limit > 2u)
        return CMETA_INVALID_ARGUMENT;
    return CMETA_OK;
}

static cmeta_status lifecycle_collector_accept(void *context,
                                                const void *value) {
    lifecycle_collector_state *state =
        (lifecycle_collector_state *)context;

    if (!state || !value)
        return CMETA_INVALID_ARGUMENT;
    if (state->staged.count >= 2u)
        return CMETA_CAPACITY_EXCEEDED;
    if (!lifecycle_test_copy(
            &state->staged.values[state->staged.count], value))
        return CMETA_OUT_OF_MEMORY;
    ++state->staged.count;
    return CMETA_OK;
}

static cmeta_status lifecycle_collector_finish(void *context) {
    lifecycle_collector_state *state =
        (lifecycle_collector_state *)context;

    if (!state || !state->output)
        return CMETA_INVALID_ARGUMENT;
    *state->output = state->staged;
    memset(&state->staged, 0, sizeof(state->staged));
    return CMETA_OK;
}

static void lifecycle_collector_abort(void *context) {
    lifecycle_collector_state *state =
        (lifecycle_collector_state *)context;
    size_t index;

    if (!state)
        return;
    for (index = 0u; index < state->staged.count; ++index)
        lifecycle_test_destroy(&state->staged.values[index]);
    memset(&state->staged, 0, sizeof(state->staged));
    ++state->aborts;
}

static const cmeta_collector_ops lifecycle_collector_ops = {
    lifecycle_collector_begin,
    lifecycle_collector_accept,
    lifecycle_collector_finish,
    lifecycle_collector_abort
};

static cmeta_collector lifecycle_collector(
    lifecycle_collector_state *state,
    lifecycle_collect_output *output) {
    cmeta_collector collector = {
        .ops = &lifecycle_collector_ops,
        .context = state,
        .zero_output = output,
        .input_type = &lifecycle_test_type,
        .limit = 2u,
        .count = 0u,
        .state = CMETA_COLLECTOR_ZERO,
        .status = CMETA_OK
    };

    state->output = output;
    return collector;
}

static void lifecycle_collect_output_destroy(
    lifecycle_collect_output *output) {
    size_t index;

    if (!output)
        return;
    for (index = 0u; index < output->count; ++index)
        lifecycle_test_destroy(&output->values[index]);
    memset(output, 0, sizeof(*output));
}

typedef struct lifecycle_sink_state {
    size_t values;
    int sum;
    bool reject;
    bool done;
    bool failed;
} lifecycle_sink_state;

static bool lifecycle_sink_value(void *user,
                                 const cmeta_type_desc *type,
                                 const void *value_) {
    lifecycle_sink_state *state = (lifecycle_sink_state *)user;
    const lifecycle_test_value *value =
        (const lifecycle_test_value *)value_;

    if (!state || !cmeta_type_equal(type, &lifecycle_test_type) ||
        !value || !value->resource)
        return false;
    ++state->values;
    state->sum += *value->resource;
    return !state->reject;
}

static void lifecycle_sink_error(void *user, const char *message) {
    lifecycle_sink_state *state = (lifecycle_sink_state *)user;

    (void)message;
    if (state)
        state->failed = true;
}

static void lifecycle_sink_done(void *user) {
    lifecycle_sink_state *state = (lifecycle_sink_state *)user;

    if (state)
        state->done = true;
}

typedef struct lifecycle_close_sink_state {
    cflow_subscription *run;
    size_t values;
    bool close_returned;
} lifecycle_close_sink_state;

static bool lifecycle_close_sink_value(void *user,
                                       const cmeta_type_desc *type,
                                       const void *value_) {
    lifecycle_close_sink_state *state =
        (lifecycle_close_sink_state *)user;
    const lifecycle_test_value *value =
        (const lifecycle_test_value *)value_;

    if (!state || !state->run ||
        !cmeta_type_equal(type, &lifecycle_test_type) || !value ||
        !value->resource)
        return false;
    ++state->values;
    cflow_subscription_close(state->run);
    state->close_returned = true;
    return true;
}

static void lifecycle_close_sink_error(void *user, const char *message) {
    (void)user;
    (void)message;
}

static void lifecycle_close_sink_done(void *user) {
    (void)user;
}

typedef struct close_from_sink_state {
    cflow_subscription *run;
    size_t values;
    bool close_returned;
} close_from_sink_state;

static bool close_from_sink_value(void *user,
                                  const cmeta_type_desc *type,
                                  const void *value) {
    close_from_sink_state *state = (close_from_sink_state *)user;
    if (!state || !cmeta_type_equal(type, &cmeta_type_int) || !value)
        return false;
    ++state->values;
    cflow_subscription_close(state->run);
    state->close_returned = true;
    return true;
}

static void close_from_sink_error(void *user, const char *message) {
    (void)user;
    (void)message;
}

static void close_from_sink_done(void *user) {
    (void)user;
}

typedef struct concurrent_close_state {
    cflow_subscription *run;
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    bool callback_entered;
    bool external_started;
    bool callback_returned;
    bool external_returned;
} concurrent_close_state;

static bool concurrent_close_value(void *user,
                                   const cmeta_type_desc *type,
                                   const void *value) {
    concurrent_close_state *state = (concurrent_close_state *)user;
    if (!state || !cmeta_type_equal(type, &cmeta_type_int) || !value)
        return false;

    cmeta_mutex_lock(&state->lock);
    state->callback_entered = true;
    cmeta_cond_broadcast(&state->changed);
    while (!state->external_started)
        cmeta_cond_wait(&state->changed, &state->lock);
    cmeta_mutex_unlock(&state->lock);

    cflow_subscription_close(state->run);

    cmeta_mutex_lock(&state->lock);
    state->callback_returned = true;
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->lock);
    return true;
}

static void concurrent_external_close(void *user) {
    concurrent_close_state *state = (concurrent_close_state *)user;
    if (!state) return;

    cmeta_mutex_lock(&state->lock);
    state->external_started = true;
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->lock);

    cflow_subscription_close(state->run);

    cmeta_mutex_lock(&state->lock);
    state->external_returned = true;
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->lock);
}

typedef struct destroy_reentrant_close_state {
    cflow_subscription *run;
    bool close_returned;
} destroy_reentrant_close_state;

static cflow_read_status destroy_reentrant_read(void *user,
                                                void *out_value,
                                                const char **error) {
    (void)user;
    (void)out_value;
    (void)error;
    return CFLOW_READ_DONE;
}

static bool destroy_reentrant_arm(void *user, cflow_waker waker) {
    (void)user;
    (void)waker;
    return true;
}

static void destroy_reentrant_cancel(void *user) { (void)user; }

typedef struct readiness_order_state {
    size_t sequence;
    size_t cancel_calls;
    size_t cancel_order;
    size_t close_order;
} readiness_order_state;

static void readiness_order_cancel(void *user) {
    readiness_order_state *state = (readiness_order_state *)user;
    if (state != NULL) {
        ++state->cancel_calls;
        state->cancel_order = ++state->sequence;
    }
}

static void readiness_order_close(void *user) {
    readiness_order_state *state = (readiness_order_state *)user;
    if (state != NULL) state->close_order = ++state->sequence;
}

typedef struct timer_wake_gate {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    bool entered;
    bool release;
} timer_wake_gate;

typedef struct source_destroy_context {
    cflow_publisher *source;
    atomic_int started;
    atomic_int returned;
} source_destroy_context;

typedef struct reentrant_timer_wake_state {
    cflow_publisher *source;
    bool returned;
} reentrant_timer_wake_state;

typedef struct timer_wake_probe {
    size_t wakes;
} timer_wake_probe;

static void timer_count_wake(void *user) {
    timer_wake_probe *probe = (timer_wake_probe *)user;
    if (probe != NULL) ++probe->wakes;
}

static void timer_blocking_wake(void *user) {
    timer_wake_gate *gate = (timer_wake_gate *)user;
    if (gate == NULL) return;
    cmeta_mutex_lock(&gate->mutex);
    gate->entered = true;
    cmeta_cond_broadcast(&gate->changed);
    while (!gate->release)
        cmeta_cond_wait(&gate->changed, &gate->mutex);
    cmeta_mutex_unlock(&gate->mutex);
}

static void source_destroy_thread(void *user) {
    source_destroy_context *context = (source_destroy_context *)user;
    atomic_store(&context->started, 1);
    cflow_publisher_destroy(context->source);
    *context->source = (cflow_publisher){0};
    atomic_store(&context->returned, 1);
}

static void timer_reentrant_destroy_wake(void *user) {
    reentrant_timer_wake_state *state = (reentrant_timer_wake_state *)user;
    if (state == NULL) return;
    cflow_publisher_destroy(state->source);
    *state->source = (cflow_publisher){0};
    state->returned = true;
}

static void destroy_reentrant_close(void *user) {
    destroy_reentrant_close_state *state =
        (destroy_reentrant_close_state *)user;
    if (!state) return;
    cflow_subscription_close(state->run);
    state->close_returned = true;
}

static size_t owned_range_size(const void *object) {
    (void)object;
    return 1u;
}

static cmeta_gen_status owned_range_next(const void *object,
                                         cmeta_range_cursor *cursor,
                                         void *out_value) {
    (void)object;
    (void)cursor;
    (void)out_value;
    return CMETA_GEN_DONE;
}

static cmeta_gen_status lifecycle_constructing_range_next(
    const void *object,
    cmeta_range_cursor *cursor,
    void *out_value) {
    if (!object || !cursor || !out_value)
        return CMETA_GEN_ERROR;
    if (cursor->index != 0u)
        return CMETA_GEN_DONE;
    if (!lifecycle_test_copy(out_value, object))
        return CMETA_GEN_ERROR;
    ++cursor->index;
    return CMETA_GEN_VALUE_AND_DONE;
}

typedef struct owned_source_state {
    bool destroyed;
} owned_source_state;

static const char *owned_source_name(void *state) {
    (void)state;
    return "owned_source";
}

static const cmeta_type_desc *owned_source_type(void *state) {
    (void)state;
    return &cflow_test_owned_value_type;
}

static cflow_step owned_source_resume(void *state,
                                      cflow_publish_context *ctx,
                                      void *out_value) {
    (void)state;
    (void)ctx;
    (void)out_value;
    return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
}

static void owned_source_noop(void *state) {
    (void)state;
}

static void owned_source_destroy(void *state) {
    owned_source_state *owned = (owned_source_state *)state;

    if (owned)
        owned->destroyed = true;
}

static void owned_source_bind(void *state, cflow_waker waker) {
    (void)state;
    (void)waker;
}

static cflow_publisher_terminal owned_source_poll(void *state,
                                                const char **error) {
    (void)state;
    (void)error;
    return CFLOW_PUBLISHER_OPEN;
}

static const cflow_resumable_ops owned_machine_ops = {
    owned_source_resume,
    owned_source_noop,
    owned_source_destroy
};

CMETA_IMPLEMENTS(cflow_publisher, owned_source, 0,
    .name = owned_source_name,
    .output_type = owned_source_type,
    .resume = owned_source_resume,
    .cancel = owned_source_noop,
    .destroy = owned_source_destroy,
    .bind_terminal_waker = owned_source_bind,
    .poll_terminal = owned_source_poll
);

typedef struct machine_run_sink_state {
    long value;
    size_t values;
    size_t dones;
    const char *error;
} machine_run_sink_state;

static bool machine_run_action(void *user,
                               const void *state,
                               const void *event,
                               void *out_target_state,
                               void *out_observation,
                               const char **out_error) {
    (void)user;
    if (state == NULL || event == NULL || out_target_state == NULL ||
        out_observation == NULL || out_error == NULL)
        return false;
    *(long *)out_target_state =
        (long)*(const int *)state + (long)*(const bool *)event;
    *(long *)out_observation = 42L;
    *out_error = NULL;
    return true;
}

static bool machine_run_sink_value(void *user,
                                   const cmeta_type_desc *type,
                                   const void *value) {
    machine_run_sink_state *state = (machine_run_sink_state *)user;
    if (state == NULL || value == NULL ||
        !cmeta_type_equal(type, &cmeta_type_long))
        return false;
    state->value = *(const long *)value;
    ++state->values;
    return true;
}

static void machine_run_sink_error(void *user, const char *message) {
    machine_run_sink_state *state = (machine_run_sink_state *)user;
    if (state != NULL) state->error = message;
}

static void machine_run_sink_done(void *user) {
    machine_run_sink_state *state = (machine_run_sink_state *)user;
    if (state != NULL) ++state->dones;
}

typedef struct saturation_source_state {
    atomic_bool cancelled;
    atomic_bool destroyed;
    int next;
} saturation_source_state;

typedef struct saturation_sink_state {
    cflow_scheduler *scheduler;
    atomic_int values;
    atomic_int errors;
    atomic_int dones;
    atomic_int blocker_status;
} saturation_sink_state;

static const char *saturation_source_name(void *state) {
    (void)state;
    return "saturation_source";
}

static const cmeta_type_desc *saturation_source_type(void *state) {
    (void)state;
    return &cmeta_type_int;
}

static cflow_step saturation_source_resume(void *state,
                                           cflow_publish_context *ctx,
                                           void *out_value) {
    saturation_source_state *source = (saturation_source_state *)state;
    (void)ctx;
    if (source == NULL || out_value == NULL)
        return (cflow_step){
            CFLOW_STEP_ERROR, {0}, "saturation source is invalid"};
    if (atomic_load(&source->cancelled))
        return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    *(int *)out_value = source->next++;
    return (cflow_step){CFLOW_STEP_VALUE, {0}, NULL};
}

static void saturation_source_cancel(void *state) {
    saturation_source_state *source = (saturation_source_state *)state;
    if (source != NULL) atomic_store(&source->cancelled, true);
}

static void saturation_source_destroy(void *state) {
    saturation_source_state *source = (saturation_source_state *)state;
    if (source != NULL) atomic_store(&source->destroyed, true);
}

static void saturation_source_bind(void *state, cflow_waker waker) {
    (void)state;
    (void)waker;
}

static cflow_publisher_terminal saturation_source_poll(void *state,
                                                    const char **error) {
    saturation_source_state *source = (saturation_source_state *)state;
    if (error != NULL) *error = NULL;
    return source != NULL && atomic_load(&source->cancelled)
        ? CFLOW_PUBLISHER_DONE : CFLOW_PUBLISHER_OPEN;
}

CMETA_IMPLEMENTS(cflow_publisher, saturation_source, 0,
    .name = saturation_source_name,
    .output_type = saturation_source_type,
    .resume = saturation_source_resume,
    .cancel = saturation_source_cancel,
    .destroy = saturation_source_destroy,
    .bind_terminal_waker = saturation_source_bind,
    .poll_terminal = saturation_source_poll
);

static void saturation_blocker(void *user) {
    (void)user;
}

static bool saturation_sink_value(void *user,
                                  const cmeta_type_desc *type,
                                  const void *value) {
    saturation_sink_state *state = (saturation_sink_state *)user;
    int previous;
    if (state == NULL || !cmeta_type_equal(type, &cmeta_type_int) ||
        value == NULL)
        return false;
    previous = atomic_fetch_add(&state->values, 1);
    if (previous == 0) {
        const cflow_schedule_result result =
            cflow_scheduler_try_post_after(
                state->scheduler, UINT64_C(1000), saturation_blocker, NULL);
        atomic_store(&state->blocker_status, (int)result.status);
    }
    return true;
}

static void saturation_sink_error(void *user, const char *message) {
    saturation_sink_state *state = (saturation_sink_state *)user;
    if (state != NULL && message != NULL)
        atomic_fetch_add(&state->errors, 1);
}

static void saturation_sink_done(void *user) {
    saturation_sink_state *state = (saturation_sink_state *)user;
    if (state != NULL) atomic_fetch_add(&state->dones, 1);
}

static bool runtime_wait_until_at_least_for(atomic_int *value, int expected,
                                            uint64_t timeout_ms) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < timeout_ms) {
        if (atomic_load(value) >= expected) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(value) >= expected;
}

static bool runtime_wait_until_at_least(atomic_int *value, int expected) {
    return runtime_wait_until_at_least_for(
        value, expected, RUNTIME_SATURATION_TIMEOUT_MS);
}

static bool runtime_wait_until_true(atomic_bool *value) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started <
           RUNTIME_SATURATION_TIMEOUT_MS) {
        if (atomic_load(value)) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(value);
}

typedef struct runtime_demand_probe {
    size_t seen[4];
    size_t calls;
} runtime_demand_probe;

static const char *runtime_demand_source_name(void *state) {
    (void)state;
    return "runtime-demand-probe";
}

static const cmeta_type_desc *runtime_demand_source_type(void *state) {
    (void)state;
    return &cmeta_type_int;
}

static cflow_step runtime_demand_source_resume(
    void *state, cflow_publish_context *ctx, void *out_value) {
    runtime_demand_probe *probe = (runtime_demand_probe *)state;

    if (probe == NULL || ctx == NULL || out_value == NULL ||
        probe->calls >= 4u)
        return (cflow_step){
            CFLOW_STEP_ERROR, {0}, "runtime demand probe is invalid"};
    probe->seen[probe->calls] = ctx->downstream_demand;
    ++probe->calls;
    *(int *)out_value = (int)probe->calls;
    return (cflow_step){
        probe->calls == 4u ? CFLOW_STEP_VALUE_AND_DONE
                           : CFLOW_STEP_VALUE,
        {0}, NULL};
}

static void runtime_demand_source_noop(void *state) {
    (void)state;
}

static void runtime_demand_source_bind(void *state, cflow_waker waker) {
    (void)state;
    (void)waker;
}

static cflow_publisher_terminal runtime_demand_source_poll(
    void *state, const char **error) {
    (void)state;
    if (error != NULL)
        *error = NULL;
    return CFLOW_PUBLISHER_OPEN;
}

CMETA_IMPLEMENTS(cflow_publisher, runtime_demand_source, 0,
    .name = runtime_demand_source_name,
    .output_type = runtime_demand_source_type,
    .resume = runtime_demand_source_resume,
    .cancel = runtime_demand_source_noop,
    .destroy = runtime_demand_source_noop,
    .bind_terminal_waker = runtime_demand_source_bind,
    .poll_terminal = runtime_demand_source_poll
);

typedef struct concurrent_wake_probe {
    cflow_subscription *run;
    atomic_bool stop;
    atomic_int wakes;
    atomic_int values;
    atomic_int errors;
    atomic_int dones;
} concurrent_wake_probe;

static void runtime_concurrent_wake(void *user) {
    concurrent_wake_probe *probe = (concurrent_wake_probe *)user;
    const uint64_t started = cmeta_monotonic_ms();
    while (!atomic_load(&probe->stop) &&
           cmeta_monotonic_ms() - started < RUNTIME_SATURATION_TIMEOUT_MS) {
        cflow_subscription_wake(probe->run);
        atomic_fetch_add(&probe->wakes, 1);
    }
}

typedef struct runtime_wake_once_context {
    cflow_subscription *run;
    atomic_int returned;
} runtime_wake_once_context;

static void runtime_wake_once(void *user) {
    runtime_wake_once_context *context =
        (runtime_wake_once_context *)user;
    if (context == NULL || context->run == NULL) return;
    cflow_subscription_wake(context->run);
    atomic_store(&context->returned, 1);
}

static bool runtime_concurrent_wake_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    concurrent_wake_probe *probe = (concurrent_wake_probe *)user;
    const int expected = atomic_fetch_add(&probe->values, 1);
    if (!cmeta_type_equal(type, &cmeta_type_int) ||
        *(const int *)value != expected) {
        atomic_fetch_add(&probe->errors, 1);
        return false;
    }
    return true;
}

static void runtime_concurrent_wake_error(void *user, const char *error) {
    concurrent_wake_probe *probe = (concurrent_wake_probe *)user;
    (void)error;
    atomic_fetch_add(&probe->errors, 1);
}

static void runtime_concurrent_wake_done(void *user) {
    concurrent_wake_probe *probe = (concurrent_wake_probe *)user;
    atomic_fetch_add(&probe->dones, 1);
}

suite("CFlow runtime") {
    group("worker pump ownership") {
        static cflow_graph surface, normalized;
        static cflow_scheduler scheduler;
        static cflow_subscription run;
        static concurrent_wake_probe probe;
        static cflow_subscriber_callbacks callbacks;
        static cmeta_thread_t waker_thread;
        static bool waker_started;
        static int inputs[1025];

        before_each() {
            memset(&surface, 0, sizeof(surface));
            memset(&normalized, 0, sizeof(normalized));
            memset(&scheduler, 0, sizeof(scheduler));
            memset(&run, 0, sizeof(run));
            memset(&probe, 0, sizeof(probe));
            probe.run = &run;
            atomic_init(&probe.stop, false);
            atomic_init(&probe.wakes, 0);
            atomic_init(&probe.values, 0);
            atomic_init(&probe.errors, 0);
            atomic_init(&probe.dones, 0);
            waker_started = false;
            normalized.root = CMETA_INVALID_ID;
            cflow_graph_init(&surface, &cmeta_type_int);
            check_true(cflow_graph_normalize(&normalized, &surface));
            check_true(cflow_scheduler_worker_init(&scheduler, 4u));
            callbacks = (cflow_subscriber_callbacks){
                runtime_concurrent_wake_value, runtime_concurrent_wake_error,
                runtime_concurrent_wake_done, &probe};
            for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i)
                inputs[i] = (int)i;
        }

        after_each() {
            atomic_store(&probe.stop, true);
            if (waker_started) cmeta_thread_join(&waker_thread);
            cflow_subscription_close(&run);
            cflow_scheduler_destroy(&scheduler);
            cflow_graph_destroy(&normalized);
            cflow_graph_destroy(&surface);
        }

        it("serializes quantum handoff and terminal completion with concurrent wakes") {
            for (unsigned round = 0; round < 32u; ++round) {
                cflow_publisher source = {0};
                cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
                atomic_store(&probe.stop, false);
                atomic_store(&probe.wakes, 0);
                atomic_store(&probe.values, 0);
                atomic_store(&probe.errors, 0);
                atomic_store(&probe.dones, 0);
                check_true(cflow_publisher_from_array(
                    &source, &cmeta_type_int, inputs,
                    sizeof(inputs) / sizeof(inputs[0])));
                check_true(cflow_subscribe(
                    &run, &normalized, &source, &scheduler, &sink));
                const int rc = cmeta_thread_create(
                    &waker_thread, runtime_concurrent_wake, &probe);
                waker_started = rc == 0;
                check_equal(rc, 0);
                check_true(runtime_wait_until_at_least(&probe.wakes, 1));
                /* #1210 diagnostic: preserve the exact concurrency test and
                 * its admission assertion, but report the returned status
                 * and concurrent producer/terminal counters on failure.
                 * The bool wrapper previously discarded the only causal
                 * evidence. Do NOT retry, extend budgets or mask failures. */
                const cflow_status_result request =
                    cflow_subscription_request_result(&run, SIZE_MAX);
                if (request.status != CFLOW_STATUS_OK) {
                    fprintf(stderr,
                        "cflow wake-before-demand: round=%u status=%d "
                        "wakes=%d values=%d errors=%d dones=%d\n",
                        round, (int)request.status,
                        atomic_load(&probe.wakes),
                        atomic_load(&probe.values),
                        atomic_load(&probe.errors),
                        atomic_load(&probe.dones));
                }
                check_equal(request.status, CFLOW_STATUS_OK);
                check_true(runtime_wait_until_at_least(&probe.dones, 1));
                atomic_store(&probe.stop, true);
                cmeta_thread_join(&waker_thread);
                waker_started = false;
                cflow_subscription_close(&run);
                check_equal(atomic_load(&probe.values), 1025);
                check_equal(atomic_load(&probe.errors), 0);
                check_equal(atomic_load(&probe.dones), 1);
            }
        }
    }

    it("coalesces first demand while a preceding wake admission is blocked") {
        const int inputs[] = {0, 1, 2};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        pending_foreign_scheduler_state scheduler_state = {0};
        cflow_scheduler scheduler;
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        concurrent_wake_probe probe = {0};
        cflow_subscriber_callbacks callbacks;
        cflow_subscriber sink;
        runtime_wake_once_context wake_context = {&run};
        cmeta_thread_t wake_thread = 0;
        bool blocked;

        cmeta_mutex_init(&scheduler_state.mutex);
        cmeta_cond_init(&scheduler_state.changed);
        check_not_null(scheduler_state.mutex);
        check_not_null(scheduler_state.changed);
        scheduler_state.block_on_post = 1u;
        scheduler = pending_foreign_scheduler_as_cflow_scheduler(
            &scheduler_state);
        normalized.root = CMETA_INVALID_ID;
        probe.run = &run;
        atomic_init(&probe.stop, false);
        atomic_init(&probe.wakes, 0);
        atomic_init(&probe.values, 0);
        atomic_init(&probe.errors, 0);
        atomic_init(&probe.dones, 0);
        atomic_init(&wake_context.returned, 0);
        callbacks = (cflow_subscriber_callbacks){
            runtime_concurrent_wake_value,
            runtime_concurrent_wake_error,
            runtime_concurrent_wake_done,
            &probe};
        sink = cflow_subscriber_from_callbacks(&callbacks);

        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, inputs,
            sizeof(inputs) / sizeof(inputs[0])));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        {
            const int created = cmeta_thread_create(
                &wake_thread, runtime_wake_once, &wake_context);
            check_equal(created, 0);
            if (created != 0) abort();
        }

        /* Synchronize on an actual in-flight Scheduler post, not a sleep.
         * Wake owns the first admission while the demand thread arrives. */
        blocked = runtime_wait_until_at_least(
            &scheduler_state.blocked_posts, 1);
        check_true(blocked);
        if (!blocked) abort();
        check_equal(
            cflow_subscription_request_result(&run, SIZE_MAX).status,
            CFLOW_STATUS_OK);
        check_equal(cflow_subscription_outstanding_demand(&run),
                    (size_t)SIZE_MAX);
        check_equal(atomic_load(&probe.values), 0);
        check_equal(atomic_load(&probe.errors), 0);

        pending_foreign_release_blocked_post(&scheduler_state);
        check_equal(cmeta_thread_join(&wake_thread), 0);
        check_equal(atomic_load(&wake_context.returned), 1);
        check_true(pending_foreign_run_one(&scheduler_state));
        check_false(pending_foreign_run_one(&scheduler_state));
        check_equal(scheduler_state.post_calls, (size_t)1u);
        check_equal(atomic_load(&probe.values), 3);
        check_equal(atomic_load(&probe.errors), 0);
        check_equal(atomic_load(&probe.dones), 1);
        check_true(cflow_subscription_is_done(&run));
        check_null(cflow_subscription_error(&run));

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cmeta_cond_destroy(&scheduler_state.changed);
        cmeta_mutex_destroy(&scheduler_state.mutex);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("records a full Scheduler rejection before first demand without retry") {
        const int input = 0;
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        coalescing_scheduler_state scheduler_state = {0};
        cflow_scheduler scheduler =
            coalescing_scheduler_as_cflow_scheduler(&scheduler_state);
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        concurrent_wake_probe probe = {0};
        cflow_subscriber_callbacks callbacks;
        cflow_subscriber sink;

        scheduler_state.rejection_status = CFLOW_ADMISSION_FULL;
        atomic_init(&scheduler_state.release, true);
        probe.run = &run;
        atomic_init(&probe.stop, false);
        atomic_init(&probe.wakes, 0);
        atomic_init(&probe.values, 0);
        atomic_init(&probe.errors, 0);
        atomic_init(&probe.dones, 0);
        callbacks = (cflow_subscriber_callbacks){
            runtime_concurrent_wake_value,
            runtime_concurrent_wake_error,
            runtime_concurrent_wake_done,
            &probe};
        sink = cflow_subscriber_from_callbacks(&callbacks);

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, &input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));

        /* Capture the existing failure contract without pretending it is
         * fixed: a pre-demand autonomous wake can terminate on rejection. */
        cflow_subscription_wake(&run);
        check_equal(atomic_load(&scheduler_state.posts), 1);
        check_equal(cflow_subscription_status(&run),
                    CFLOW_STATUS_CAPACITY_EXCEEDED);
        check_equal(cflow_subscription_request_result(&run, SIZE_MAX).status,
                    CFLOW_STATUS_CAPACITY_EXCEEDED);
        check_equal(atomic_load(&scheduler_state.posts), 1);
        check_equal(atomic_load(&probe.values), 0);
        check_equal(atomic_load(&probe.errors), 1);
        check_equal(atomic_load(&probe.dones), 0);

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("passes outstanding downstream demand to each source resume") {
        runtime_demand_probe probe = {0};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source =
            runtime_demand_source_as_cflow_publisher(&probe);
        cflow_subscription run = {0};

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));
        check_equal(cflow_subscription_request_result(&run, 4u).status,
                    CFLOW_STATUS_OK);

        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(probe.calls, (size_t)4u);
        check_equal(probe.seen[0], (size_t)4u);
        check_equal(probe.seen[1], (size_t)3u);
        check_equal(probe.seen[2], (size_t)2u);
        check_equal(probe.seen[3], (size_t)1u);
        check_true(cflow_subscription_is_done(&run));
        check_null(cflow_subscription_error(&run));
        check_equal(cflow_subscription_request_result(&run, 1u).status,
                    CFLOW_STATUS_CLOSED);

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("reports exact Subscription demand admission and retains rejected demand") {
        static const cflow_admission_status scheduler_statuses[] = {
            CFLOW_ADMISSION_FULL,
            CFLOW_ADMISSION_CLOSED,
            CFLOW_ADMISSION_ALLOCATION_FAILED,
            CFLOW_ADMISSION_INVALID_ARGUMENT
        };
        static const cflow_status expected_statuses[] = {
            CFLOW_STATUS_CAPACITY_EXCEEDED,
            CFLOW_STATUS_CLOSED,
            CFLOW_STATUS_ALLOCATION_FAILED,
            CFLOW_STATUS_INVALID_ARGUMENT
        };

        check_equal(cflow_subscription_request_result(NULL, 1u).status,
                    CFLOW_STATUS_INVALID_ARGUMENT);
        for (size_t i = 0u;
             i < sizeof(scheduler_statuses) / sizeof(scheduler_statuses[0]);
             ++i) {
            const int input = 17;
            coalescing_scheduler_state scheduler_state = {0};
            cflow_graph surface = {0};
            cflow_graph normalized = {0};
            cflow_scheduler scheduler;
            cflow_publisher source = {0};
            cflow_subscription run = {0};
            cflow_status_result result;

            scheduler_state.rejection_status = scheduler_statuses[i];
            atomic_init(&scheduler_state.release, true);
            scheduler = coalescing_scheduler_as_cflow_scheduler(
                &scheduler_state);
            normalized.root = CMETA_INVALID_ID;
            cflow_graph_init(&surface, &cmeta_type_int);
            check_true(cflow_graph_normalize(&normalized, &surface));
            check_true(cflow_publisher_from_array(
                &source, &cmeta_type_int, &input, 1u));
            check_true(cflow_subscribe(
                &run, &normalized, &source, &scheduler, NULL));
            check_equal(cflow_subscription_request_result(&run, 0u).status,
                        CFLOW_STATUS_INVALID_ARGUMENT);

            result = cflow_subscription_request_result(&run, 1u);

            check_equal(result.status, expected_statuses[i]);
            check_equal(cflow_subscription_outstanding_demand(&run),
                        (size_t)1u);
            check_false(cflow_subscription_request(&run, 1u));
            check_equal(cflow_subscription_outstanding_demand(&run),
                        (size_t)2u);

            cflow_subscription_close(&run);
            cflow_graph_destroy(&normalized);
            cflow_graph_destroy(&surface);
        }
    }

    it("reports accepted demand when a foreign Scheduler runs the pump inline") {
        const int input = 23;
        pending_foreign_scheduler_state scheduler_state = {0};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler;
        cflow_publisher source = {0};
        cflow_subscription run = {0};

        cmeta_mutex_init(&scheduler_state.mutex);
        cmeta_cond_init(&scheduler_state.changed);
        scheduler_state.run_inline = true;
        scheduler = pending_foreign_scheduler_as_cflow_scheduler(
            &scheduler_state);
        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, &input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));

        check_equal(cflow_subscription_request_result(&run, 1u).status,
                    CFLOW_STATUS_OK);
        check_true(cflow_subscription_is_done(&run));
        check_equal(scheduler_state.post_calls, (size_t)1u);

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cmeta_cond_destroy(&scheduler_state.changed);
        cmeta_mutex_destroy(&scheduler_state.mutex);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("preserves downstream demand when a source value is filtered") {
        runtime_demand_probe probe = {0};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source =
            runtime_demand_source_as_cflow_publisher(&probe);
        cflow_subscription run = {0};

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_add(
            &surface, CFLOW_OP_FILTER, cflow_test_even.fn, NULL));
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));
        check_true(cflow_subscription_request(&run, 2u));

        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(probe.calls, (size_t)4u);
        check_equal(probe.seen[0], (size_t)2u);
        check_equal(probe.seen[1], (size_t)2u);
        check_equal(probe.seen[2], (size_t)1u);
        check_equal(probe.seen[3], (size_t)1u);
        check_true(cflow_subscription_is_done(&run));
        check_null(cflow_subscription_error(&run));

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("preserves a live Run when a second open is rejected") {
        const int first_input[] = {11};
        const int second_input[] = {22};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher first_source = {0};
        cflow_publisher second_source = {0};
        cflow_subscription run = {0};
        cflow_subscription original;
        bool reopened;

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &first_source, &cmeta_type_int, first_input, 1u));
        check_true(cflow_publisher_from_array(
            &second_source, &cmeta_type_int, second_input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &first_source, &scheduler, NULL));
        original = run;

        reopened = cflow_subscribe(
            &run, &normalized, &second_source, &scheduler, NULL);
        check_false(reopened);
        check_true(run.impl == original.impl);

        if (run.impl != original.impl) {
            cflow_subscription_close(&run);
            run = original;
        }
        cflow_subscription_close(&run);
        if (!reopened) cflow_publisher_destroy(&second_source);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("settles an accepted Run pump when Scheduler shutdown cancels it") {
        const int input[] = {33};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));
        check_true(cflow_subscription_request(&run, 1u));

        check_true(cflow_scheduler_shutdown(&scheduler));
        check_equal(cflow_subscription_request_result(&run, 1u).status,
                    CFLOW_STATUS_CANCELLED);
        cflow_subscription_close(&run);

        check_null(run.impl);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("cancels an accepted foreign Scheduler pump before closing Run") {
        const int input[] = {44};
        pending_foreign_scheduler_state state = {0};
        cflow_scheduler scheduler;
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        coalescing_close_context close_context = {0};
        cmeta_thread_t close_thread = 0;
        bool cancel_observed;
        bool close_returned_before_rescue;
        bool close_started;

        cmeta_mutex_init(&state.mutex);
        cmeta_cond_init(&state.changed);
        scheduler = pending_foreign_scheduler_as_cflow_scheduler(&state);
        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_not_null(state.mutex);
        check_not_null(state.changed);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));
        check_true(cflow_subscription_request(&run, 1u));
        check_equal(pending_foreign_pending(&state), (size_t)1u);

        close_context.run = &run;
        check_equal(cmeta_thread_create(
            &close_thread, coalescing_close, &close_context), 0);
        close_started = runtime_wait_until_at_least(
            &close_context.started, 1);
        if (!close_started) abort();
        cancel_observed = runtime_wait_until_at_least(
            &state.cancel_calls, 1);
        close_returned_before_rescue = runtime_wait_until_at_least(
            &close_context.returned, 1);

        if (!close_returned_before_rescue) {
            if (cancel_observed) abort();
            (void)pending_foreign_run_one(&state);
            if (!runtime_wait_until_at_least(&close_context.returned, 1))
                abort();
        }
        check_equal(cmeta_thread_join(&close_thread), 0);
        close_thread = 0;

        check_true(close_started);
        check_true(cancel_observed);
        check_true(close_returned_before_rescue);
        check_equal(atomic_load(&state.cancel_calls), 1);
        check_equal(state.last_cancelled_id, (cflow_task_id)1u);
        check_null(run.impl);

        cflow_scheduler_destroy(&scheduler);
        cmeta_cond_destroy(&state.changed);
        cmeta_mutex_destroy(&state.mutex);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("closes while a running pump publishes its next generation") {
        pending_foreign_scheduler_state scheduler_state = {0};
        saturation_source_state source_state = {0};
        cflow_scheduler scheduler;
        cflow_publisher source = saturation_source_as_cflow_publisher(&source_state);
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_subscription run = {0};
        pending_foreign_run_context pump_context = {&scheduler_state};
        coalescing_close_context close_context = {&run};
        cmeta_thread_t pump_thread = 0;
        cmeta_thread_t close_thread = 0;
        bool pump_returned;
        bool close_started;
        bool close_returned_before_rescue;

        cmeta_mutex_init(&scheduler_state.mutex);
        cmeta_cond_init(&scheduler_state.changed);
        scheduler_state.block_on_post = 2u;
        scheduler = pending_foreign_scheduler_as_cflow_scheduler(
            &scheduler_state);
        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_not_null(scheduler_state.mutex);
        check_not_null(scheduler_state.changed);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));
        check_true(cflow_subscription_request(&run, SIZE_MAX));
        check_equal(pending_foreign_pending(&scheduler_state), (size_t)1u);

        check_equal(cmeta_thread_create(
            &pump_thread, pending_foreign_run_one_thread, &pump_context), 0);
        if (!runtime_wait_until_at_least(
                &scheduler_state.blocked_posts, 1))
            abort();

        check_equal(cmeta_thread_create(
            &close_thread, coalescing_close, &close_context), 0);
        close_started = runtime_wait_until_at_least(
            &close_context.started, 1);
        if (!close_started) abort();

        pending_foreign_release_blocked_post(&scheduler_state);
        pump_returned = runtime_wait_until_at_least(
            &pump_context.returned, 1);
        if (!pump_returned) abort();
        close_returned_before_rescue = runtime_wait_until_at_least(
            &close_context.returned, 1);
        if (!close_returned_before_rescue) {
            (void)pending_foreign_run_one(&scheduler_state);
            if (!runtime_wait_until_at_least(&close_context.returned, 1))
                abort();
        }

        check_equal(cmeta_thread_join(&pump_thread), 0);
        check_equal(cmeta_thread_join(&close_thread), 0);
        check_true(pump_context.ran);
        check_true(close_started);
        check_true(close_returned_before_rescue);
        check_equal(atomic_load(&scheduler_state.cancel_calls), 1);
        check_equal(scheduler_state.last_cancelled_id, (cflow_task_id)2u);
        check_true(atomic_load(&source_state.cancelled));
        check_true(atomic_load(&source_state.destroyed));
        check_null(run.impl);

        cflow_scheduler_destroy(&scheduler);
        cmeta_cond_destroy(&scheduler_state.changed);
        cmeta_mutex_destroy(&scheduler_state.mutex);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("bounds a stalled controlled Scheduler admission barrier") {
        enum { CONTROLLED_BARRIER_TIMEOUT_MS = 10 };
        coalescing_scheduler_state state = {0};
        cflow_scheduler scheduler =
            coalescing_scheduler_as_cflow_scheduler(&state);
        const uint64_t started = cmeta_monotonic_ms();
        cflow_schedule_result result;

        state.timeout_ms = CONTROLLED_BARRIER_TIMEOUT_MS;
        result = cflow_scheduler_try_post_after(
            &scheduler, 0u, coalescing_barrier_task, NULL);

        check_equal(result.status, CFLOW_ADMISSION_FULL);
        check_equal(result.task_id, (cflow_task_id)0u);
        check_true(atomic_load(&state.entered));
        check_true(atomic_load(&state.timed_out));
        check_false(atomic_load(&state.release));
        check_equal(atomic_load(&state.posts), 1);
        check_less(cmeta_monotonic_ms() - started,
                   (uint64_t)RUNTIME_SATURATION_TIMEOUT_MS);
    }

    group("managed value slots") {
        before_each() {
            lifecycle_test_reset();
        }

        it("copy-constructs and destroys an owning value exactly once") {
            lifecycle_test_value source = lifecycle_test_make(17);
            cflow_value_slot slot = {0};

            check_not_null(source.resource);
            check_true(cflow_value_slot_init(&slot, &lifecycle_test_type));
            check_true(cflow_value_slot_copy(&slot, &source));
            check_not_null(((lifecycle_test_value *)slot.storage)->resource);
            check_true(((lifecycle_test_value *)slot.storage)->resource !=
                       source.resource);
            check_equal(
                *((lifecycle_test_value *)slot.storage)->resource,
                17);

            cflow_value_slot_destroy(&slot);
            lifecycle_test_destroy(&source);
            check_equal(lifecycle_test_copies, (size_t)1u);
            check_equal(lifecycle_test_destroys, (size_t)2u);
        }

        it("leaves a failed copy destination empty") {
            lifecycle_test_value source = lifecycle_test_make(23);
            cflow_value_slot slot = {0};

            lifecycle_test_copy_fails = true;
            check_not_null(source.resource);
            check_true(cflow_value_slot_init(&slot, &lifecycle_test_type));
            check_false(cflow_value_slot_copy(&slot, &source));
            check_false(slot.live);

            cflow_value_slot_destroy(&slot);
            check_equal(lifecycle_test_destroys, (size_t)0u);
            lifecycle_test_destroy(&source);
            check_equal(lifecycle_test_destroys, (size_t)1u);
        }

        it("move-constructs then destroys the moved-from slot") {
            lifecycle_test_value source = lifecycle_test_make(31);
            cflow_value_slot from = {0};
            cflow_value_slot to = {0};

            check_not_null(source.resource);
            check_true(cflow_value_slot_init(&from, &lifecycle_test_type));
            check_true(cflow_value_slot_init(&to, &lifecycle_test_type));
            check_true(cflow_value_slot_copy(&from, &source));
            lifecycle_test_destroy(&source);
            check_true(cflow_value_slot_move(&to, &from));
            check_false(from.live);
            check_true(to.live);
            check_equal(lifecycle_test_moves, (size_t)1u);
            check_equal(lifecycle_test_destroys, (size_t)2u);

            cflow_value_slot_destroy(&from);
            cflow_value_slot_destroy(&to);
            check_equal(lifecycle_test_destroys, (size_t)3u);
        }

        it("honors descriptor alignment") {
            cflow_value_slot slot = {0};

            check_true(cflow_value_slot_init(
                &slot, &lifecycle_overaligned_type));
            check_equal((uintptr_t)slot.storage % 64u, (uintptr_t)0u);
            cflow_value_slot_destroy(&slot);
        }
    }

    it("owns one-shot values that require lifecycle callbacks") {
        lifecycle_test_value value = lifecycle_test_make(37);
        cflow_resumable machine = {0};
        lifecycle_test_value output = {0};
        cflow_publish_context context = {0};
        cflow_step step;

        lifecycle_test_reset();
        const bool initialized = cflow_resumable_from_value(
            &machine, &lifecycle_test_type, &value);

        check_true(initialized);
        check_not_null(machine.ops);
        if (initialized) {
            step = machine.ops->resume(machine.state, &context, &output);
            check_true(step.kind == CFLOW_STEP_VALUE_AND_DONE);
            check_not_null(output.resource);
            check_equal(*output.resource, 37);
            lifecycle_test_destroy(&output);
            machine.ops->destroy(machine.state);
        }
        lifecycle_test_destroy(&value);
        check_equal(lifecycle_test_copies, (size_t)2u);
        check_equal(lifecycle_test_destroys, (size_t)3u);
    }

    it("fails when a quantum reschedule reaches a full Scheduler") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        saturation_source_state source_state = {0};
        saturation_sink_state sink_state = {0};
        cflow_publisher source;
        cflow_subscription run = {0};
        cflow_subscriber_callbacks callbacks;
        cflow_subscriber sink;
        const char *error;

        normalized.root = CMETA_INVALID_ID;
        source = saturation_source_as_cflow_publisher(&source_state);
        sink_state.scheduler = &scheduler;
        atomic_init(&sink_state.blocker_status,
                    (int)CFLOW_ADMISSION_INVALID_ARGUMENT);
        callbacks = (cflow_subscriber_callbacks){
            saturation_sink_value,
            saturation_sink_error,
            saturation_sink_done,
            &sink_state};
        sink = cflow_subscriber_from_callbacks(&callbacks);

        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_worker_init_with_capacity(
            &scheduler, 1u, 1u, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, SIZE_MAX));

        check_true(runtime_wait_until_at_least(&sink_state.errors, 1));
        check_equal(atomic_load(&sink_state.blocker_status),
                    (int)CFLOW_ADMISSION_ACCEPTED);
        check_greater(atomic_load(&sink_state.values), 0);
        check_equal(atomic_load(&sink_state.errors), 1);
        check_equal(atomic_load(&sink_state.dones), 0);
        error = cflow_subscription_error(&run);
        check_not_null(error);
        if (error != NULL) check_contains(error, "scheduler is full");
        check_equal(cflow_subscription_status(&run),
                    CFLOW_STATUS_CAPACITY_EXCEEDED);
        check_equal(cflow_subscription_request_result(&run, 1u).status,
                    CFLOW_STATUS_CAPACITY_EXCEEDED);

        cflow_subscription_close(&run);
        check_true(atomic_load(&source_state.destroyed));
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("preserves an autonomous rejection failure coalesced behind a request") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        coalescing_scheduler_state scheduler_state = {0};
        saturation_source_state source_state = {0};
        saturation_sink_state sink_state = {0};
        cflow_publisher source = saturation_source_as_cflow_publisher(&source_state);
        cflow_scheduler scheduler =
            coalescing_scheduler_as_cflow_scheduler(&scheduler_state);
        cflow_subscription run = {0};
        cflow_subscriber_callbacks callbacks = {
            saturation_sink_value,
            saturation_sink_error,
            saturation_sink_done,
            &sink_state};
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        coalescing_request_context request = {&run};
        cmeta_thread_t thread = {0};

        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        {
            const int create_status = cmeta_thread_create(
                &thread, coalescing_request, &request);
            check_equal(create_status, 0);
            if (create_status != 0) abort();
        }
        check_true(runtime_wait_until_at_least(&scheduler_state.posts, 1));
        check_true(atomic_load(&scheduler_state.entered));

        cflow_subscription_wake(&run);
        atomic_store(&scheduler_state.release, true);
        check_true(runtime_wait_until_at_least(&sink_state.errors, 1));
        {
            const bool returned = runtime_wait_until_true(&request.returned);
            check_true(returned);
            if (!returned) abort();
        }
        check_equal(cmeta_thread_join(&thread), 0);
        check_true(atomic_load(&request.returned));
        check_false(request.result);
        check_equal(atomic_load(&scheduler_state.posts), 1);
        check_false(atomic_load(&scheduler_state.timed_out));
        check_equal(atomic_load(&sink_state.errors), 1);
        check_contains(cflow_subscription_error(&run), "scheduler is full");

        cflow_subscription_close(&run);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("hands rejected task destruction to a closing error callback") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        coalescing_scheduler_state scheduler_state = {0};
        saturation_source_state source_state = {0};
        cflow_publisher source = saturation_source_as_cflow_publisher(&source_state);
        cflow_scheduler scheduler =
            coalescing_scheduler_as_cflow_scheduler(&scheduler_state);
        cflow_subscription run = {0};
        rejection_callback_close_state close_state = {&run};
        cflow_subscriber_callbacks callbacks = {
            NULL, rejection_callback_close_error, NULL, &close_state};
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        coalescing_request_context request = {&run};
        cmeta_thread_t thread = {0};

        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        {
            const int create_status = cmeta_thread_create(
                &thread, coalescing_request, &request);
            check_equal(create_status, 0);
            if (create_status != 0) abort();
        }
        check_true(runtime_wait_until_at_least(&scheduler_state.posts, 1));
        cflow_subscription_wake(&run);
        atomic_store(&scheduler_state.release, true);

        {
            const bool returned = runtime_wait_until_true(&request.returned);
            check_true(returned);
            if (!returned) abort();
        }
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(atomic_load(&close_state.errors), 1);
        check_true(atomic_load(&close_state.close_returned));
        check_null(run.impl);
        check_true(atomic_load(&source_state.destroyed));
        check_false(atomic_load(&scheduler_state.timed_out));

        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("hands the last rejected task reference to a concurrent closer") {
        enum { CLOSE_RACE_REPETITIONS = 64 };
        int repetition;
        for (repetition = 0; repetition < CLOSE_RACE_REPETITIONS;
             ++repetition) {
            cflow_graph surface = {0};
            cflow_graph normalized = {0};
            coalescing_scheduler_state scheduler_state = {0};
            saturation_source_state source_state = {0};
            saturation_sink_state sink_state = {0};
            cflow_publisher source =
                saturation_source_as_cflow_publisher(&source_state);
            cflow_scheduler scheduler =
                coalescing_scheduler_as_cflow_scheduler(&scheduler_state);
            cflow_subscription run = {0};
            cflow_subscriber_callbacks callbacks = {
                saturation_sink_value, saturation_sink_error,
                saturation_sink_done, &sink_state};
            cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
            coalescing_request_context request = {&run};
            coalescing_close_context close = {&run};
            cmeta_thread_t request_thread = {0};
            cmeta_thread_t close_thread = {0};

            cflow_graph_init(&surface, &cmeta_type_int);
            check_true(cflow_graph_normalize(&normalized, &surface));
            check_true(cflow_subscribe(
                &run, &normalized, &source, &scheduler, &sink));
            {
                const int create_status = cmeta_thread_create(
                    &request_thread, coalescing_request, &request);
                check_equal(create_status, 0);
                if (create_status != 0) abort();
            }
            check_true(runtime_wait_until_at_least(
                &scheduler_state.posts, 1));
            {
                const int create_status = cmeta_thread_create(
                    &close_thread, coalescing_close, &close);
                check_equal(create_status, 0);
                if (create_status != 0) abort();
            }
            check_true(runtime_wait_until_at_least(&close.started, 1));
            atomic_store(&scheduler_state.release, true);

            {
                const bool request_returned =
                    runtime_wait_until_true(&request.returned);
                const bool close_returned = runtime_wait_until_at_least(
                    &close.returned, 1);
                check_true(request_returned);
                check_true(close_returned);
                if (!request_returned || !close_returned) abort();
            }
            check_equal(cmeta_thread_join(&request_thread), 0);
            check_equal(cmeta_thread_join(&close_thread), 0);
            check_true(atomic_load(&request.returned));
            check_equal(atomic_load(&close.returned), 1);
            check_null(run.impl);
            check_true(atomic_load(&source_state.destroyed));
            check_equal(atomic_load(&sink_state.errors), 0);
            check_false(atomic_load(&scheduler_state.timed_out));

            cflow_graph_destroy(&normalized);
            cflow_graph_destroy(&surface);
        }
    }

    it("takes ownership of managed coordination children") {
        owned_source_state state = {false};
        cflow_resumable children[] = {{
            "owned_machine",
            &cflow_test_owned_value_type,
            &owned_machine_ops,
            &state
        }};
        cflow_resumable coordination = {0};
        const bool initialized = cflow_resumable_from_coordination(
            &coordination, CFLOW_COORD_ALL, children, 1u);

        check_true(initialized);
        check_not_null(coordination.ops);
        check_null(children[0].ops);
        check_false(state.destroyed);
        if (initialized)
            coordination.ops->destroy(coordination.state);
        else
            children[0].ops->destroy(children[0].state);
        check_true(state.destroyed);
    }

    it("retains independently owned managed coordination values") {
        lifecycle_test_value values[] = {
            lifecycle_test_make(6), lifecycle_test_make(9)
        };
        cflow_resumable children[2] = {{0}};
        cflow_resumable coordination = {0};
        cflow_publish_context context = {0};
        cflow_coord_event event = {0};
        const void *retained = NULL;
        cflow_step step;

        lifecycle_test_reset();
        check_true(cflow_resumable_from_value(
            &children[0], &lifecycle_test_type, &values[0]));
        check_true(cflow_resumable_from_value(
            &children[1], &lifecycle_test_type, &values[1]));
        check_true(cflow_resumable_from_coordination(
            &coordination, CFLOW_COORD_ALL, children, 2u));
        step = coordination.ops->resume(
            coordination.state, &context, &event);
        check_true(step.kind == CFLOW_STEP_VALUE_AND_DONE);
        check_equal(event.child_index, SIZE_MAX);
        check_true(cflow_coord_value(
            &coordination, 0u, NULL, &retained));
        check_equal(*((const lifecycle_test_value *)retained)->resource, 6);
        check_true(cflow_coord_value(
            &coordination, 1u, NULL, &retained));
        check_equal(*((const lifecycle_test_value *)retained)->resource, 9);

        coordination.ops->destroy(coordination.state);
        lifecycle_test_destroy(&values[0]);
        lifecycle_test_destroy(&values[1]);
        check_equal(lifecycle_test_copies, (size_t)4u);
        check_equal(lifecycle_test_moves, (size_t)2u);
        check_equal(lifecycle_test_destroys, (size_t)8u);
    }

    it("copies managed Subflow input before ownership transfer") {
        const cflow_test_owned_value value = {0};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_resumable machine = {0};
        bool initialized;

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cflow_test_owned_value_type);
        check_true(cflow_graph_normalize(&normalized, &surface));

        initialized = cflow_resumable_from_subgraph(
            &machine, &normalized, normalized.root, &value);

        check_true(initialized);
        check_not_null(machine.ops);
        if (initialized)
            machine.ops->destroy(machine.state);

        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("rejects an owning source before run ownership transfer") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        owned_source_state state = {false};
        cflow_publisher source = owned_source_as_cflow_publisher(&state);
        cflow_subscription run = {0};
        bool opened;

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cflow_test_owned_value_type);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));

        opened = cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL);

        check_false(opened);
        check_null(run.impl);
        check_not_null(source.self);
        check_false(state.destroyed);
        if (opened)
            cflow_subscription_close(&run);
        else
            cflow_publisher_destroy(&source);
        check_true(state.destroyed);

        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("admits managed graphs when an operator enters the path") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_subgraph *root;

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &lifecycle_test_type);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_value_runtime_graph_supported(&normalized));

        root = &normalized.subgraphs[normalized.root];
        root->nodes[root->entry].op = CFLOW_OP_FILTER;
        check_true(cflow_value_runtime_graph_supported(&normalized));

        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("executes managed filter map and reduce slots") {
        int input[] = {1, 2, 3, 4};
        cmeta_type_desc managed_int = cmeta_type_int;
        cmeta_type_desc managed_long = cmeta_type_long;
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_graph optimized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        tracked_long_sink_state state = {0};
        cflow_subscriber_callbacks callbacks = {
            tracked_long_sink_value,
            tracked_long_sink_error,
            tracked_long_sink_done,
            &state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        managed_int.traits = &tracked_int_traits;
        managed_long.traits = &tracked_long_traits;
        normalized.root = CMETA_INVALID_ID;
        optimized.root = CMETA_INVALID_ID;
        tracked_int_copies = tracked_int_moves = tracked_int_destroys = 0u;
        tracked_long_copies = tracked_long_moves = tracked_long_destroys = 0u;

        cflow_graph_init(&surface, &managed_int);
        check_true(cflow_graph_add(
            &surface, CFLOW_OP_FILTER, cflow_test_even.fn, NULL));
        check_true(cflow_graph_add(
            &surface, CFLOW_OP_MAP, cflow_test_square.fn, NULL));
        check_true(cflow_graph_add(
            &surface, CFLOW_OP_REDUCE, runtime_sum_long.fn, NULL));
        {
            cflow_subgraph *root = &surface.subgraphs[surface.root];
            root->input_type = &managed_int;
            root->output_type = &managed_long;
            for (size_t i = 0u; i < root->node_count; ++i) {
                cflow_node *node = &root->nodes[i];
                if (node->op == CFLOW_OP_INPUT) {
                    node->input_type = &managed_int;
                    node->output_type = &managed_int;
                } else if (node->op == CFLOW_OP_FILTER) {
                    node->input_type = &managed_int;
                    node->output_type = &managed_int;
                } else if (node->op == CFLOW_OP_MAP) {
                    node->input_type = &managed_int;
                    node->output_type = &managed_long;
                } else if (node->op == CFLOW_OP_REDUCE) {
                    node->input_type = &managed_long;
                    node->output_type = &managed_long;
                }
            }
        }
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_graph_optimize(
            &optimized, &normalized,
            (cflow_opt_options){CMETA_OPT_DEFAULT}, NULL));
        check_true(cflow_subgraph_output_type(
            &optimized, optimized.root) == &managed_long);
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &source, &managed_int, input, 4u));
        check_true(cflow_subscribe(
            &run, &optimized, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_null(state.error);
        check_true(state.done);
        check_equal(state.values, (size_t)1u);
        check_equal(state.value, 20L);
        cflow_subscription_close(&run);
        check_equal(tracked_int_copies, (size_t)8u);
        check_equal(tracked_int_moves, (size_t)0u);
        check_equal(tracked_int_destroys, (size_t)8u);
        check_equal(tracked_long_copies, (size_t)1u);
        check_equal(tracked_long_moves, (size_t)2u);
        check_equal(tracked_long_destroys, (size_t)6u);

        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&optimized);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("creates a constructing array source for managed values") {
        lifecycle_test_value value = lifecycle_test_make(41);
        lifecycle_test_value output = {0};
        cflow_publisher source = {0};
        cflow_step step;

        lifecycle_test_reset();
        check_not_null(value.resource);
        check_true(cflow_publisher_from_array(
            &source, &lifecycle_test_type, &value, 1u));
        check_true(cflow_publisher_has(
            &source, CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES));
        step = cflow_publisher_resume(&source, NULL, &output);
        check_true(step.kind == CFLOW_STEP_VALUE_AND_DONE);
        check_not_null(output.resource);
        check_true(output.resource != value.resource);
        check_equal(*output.resource, 41);

        lifecycle_test_destroy(&output);
        lifecycle_test_destroy(&value);
        cflow_publisher_destroy(&source);
        check_equal(lifecycle_test_copies, (size_t)1u);
        check_equal(lifecycle_test_destroys, (size_t)2u);
    }

    it("rejects range sources whose values require lifecycle callbacks") {
        const cflow_test_owned_value value = {0};
        const cmeta_range range = {
            .object = &value,
            .element_type = &cflow_test_owned_value_type,
            .flags = CMETA_RANGE_SIZED,
            .size = owned_range_size,
            .next = owned_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_publisher source = {0};
        const bool initialized = cflow_publisher_from_range(&source, range);

        check_false(initialized);
        check_null(source.self);
        if (initialized)
            cflow_publisher_destroy(&source);
    }

    it("creates a managed source from a constructing range") {
        lifecycle_test_value value = lifecycle_test_make(43);
        lifecycle_test_value output = {0};
        const cmeta_range range = {
            .object = &value,
            .element_type = &lifecycle_test_type,
            .flags = CMETA_RANGE_SIZED | CMETA_RANGE_CONSTRUCTS_VALUES,
            .size = owned_range_size,
            .next = lifecycle_constructing_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_publisher source = {0};
        cflow_step step;

        lifecycle_test_reset();
        check_not_null(value.resource);
        check_true(cflow_publisher_from_range(&source, range));
        check_true(cflow_publisher_has(
            &source, CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES));
        step = cflow_publisher_resume(&source, NULL, &output);
        check_true(step.kind == CFLOW_STEP_VALUE_AND_DONE);
        check_not_null(output.resource);
        check_true(output.resource != value.resource);
        check_equal(*output.resource, 43);

        lifecycle_test_destroy(&output);
        lifecycle_test_destroy(&value);
        cflow_publisher_destroy(&source);
        check_equal(lifecycle_test_copies, (size_t)1u);
        check_equal(lifecycle_test_destroys, (size_t)2u);
    }

    it("runs a managed array through a source-only graph") {
        lifecycle_test_value input[] = {
            lifecycle_test_make(5), lifecycle_test_make(8)
        };
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        lifecycle_sink_state state = {0};
        cflow_subscriber_callbacks callbacks = {
            lifecycle_sink_value,
            lifecycle_sink_error,
            lifecycle_sink_done,
            &state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        bool opened;

        lifecycle_test_reset();
        normalized.root = CMETA_INVALID_ID;
        check_not_null(input[0].resource);
        check_not_null(input[1].resource);
        cflow_graph_init(&surface, &lifecycle_test_type);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &source, &lifecycle_test_type, input, 2u));
        opened = cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink);
        check_true(opened);
        if (opened) {
            check_true(cflow_subscription_request(&run, 2u));
            (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
            check_true(cflow_subscription_is_done(&run));
            cflow_subscription_close(&run);
        } else {
            cflow_publisher_destroy(&source);
        }

        check_equal(state.values, (size_t)2u);
        check_equal(state.sum, 13);
        check_true(state.done);
        check_false(state.failed);
        lifecycle_test_destroy(&input[0]);
        lifecycle_test_destroy(&input[1]);
        check_equal(lifecycle_test_copies, (size_t)2u);
        check_equal(lifecycle_test_destroys, (size_t)4u);

        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("destroys a managed value when the sink rejects it") {
        lifecycle_test_value input = lifecycle_test_make(11);
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        lifecycle_sink_state state = {.reject = true};
        cflow_subscriber_callbacks callbacks = {
            lifecycle_sink_value,
            lifecycle_sink_error,
            lifecycle_sink_done,
            &state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        lifecycle_test_reset();
        normalized.root = CMETA_INVALID_ID;
        check_not_null(input.resource);
        cflow_graph_init(&surface, &lifecycle_test_type);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &source, &lifecycle_test_type, &input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(state.values, (size_t)1u);
        check_true(state.failed);
        check_equal(cflow_subscription_error(&run), "observer rejected value");
        check_equal(lifecycle_test_destroys, (size_t)1u);
        cflow_subscription_close(&run);
        lifecycle_test_destroy(&input);
        check_equal(lifecycle_test_copies, (size_t)1u);
        check_equal(lifecycle_test_destroys, (size_t)2u);

        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("cleans a managed slot after a sink closes its run") {
        lifecycle_test_value input = lifecycle_test_make(19);
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        lifecycle_close_sink_state state = {&run, 0u, false};
        cflow_subscriber_callbacks callbacks = {
            lifecycle_close_sink_value,
            lifecycle_close_sink_error,
            lifecycle_close_sink_done,
            &state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);

        lifecycle_test_reset();
        normalized.root = CMETA_INVALID_ID;
        check_not_null(input.resource);
        cflow_graph_init(&surface, &lifecycle_test_type);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &source, &lifecycle_test_type, &input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(state.values, (size_t)1u);
        check_true(state.close_returned);
        check_null(run.impl);
        check_equal(lifecycle_test_destroys, (size_t)1u);
        lifecycle_test_destroy(&input);
        check_equal(lifecycle_test_copies, (size_t)1u);
        check_equal(lifecycle_test_destroys, (size_t)2u);

        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("collects a managed range with independent ownership") {
        lifecycle_range_owner owner = {{
            lifecycle_test_make(3), lifecycle_test_make(7)
        }, 2u};
        lifecycle_collect_output output = {0};
        lifecycle_collector_state collector_state = {0};
        cmeta_collector collector =
            lifecycle_collector(&collector_state, &output);
        cmeta_range range = {
            .object = &owner,
            .element_type = &lifecycle_test_type,
            .flags = CMETA_RANGE_SIZED | CMETA_RANGE_CONSTRUCTS_VALUES,
            .size = lifecycle_range_size,
            .next = lifecycle_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_stream stream = {0};
        cflow_collect_result result;
        const char *error = NULL;

        lifecycle_test_reset();
        check_not_null(owner.values[0].resource);
        check_not_null(owner.values[1].resource);
        check_not_null(cflow_stream_from_range(&stream, range));
        result = cflow_eval_collect_result(&stream, &collector, &error);
        check_true(cflow_collect_result_is_ok(result));
        check_equal(result.status, CFLOW_STATUS_OK);
        check_equal(result.collector_status, CMETA_OK);
        check_equal(result.collector_state, CMETA_COLLECTOR_COMMITTED);
        check_equal(result.count, (size_t)2u);
        check_null(error);
        check_true(collector.state == CMETA_COLLECTOR_COMMITTED);
        check_equal(output.count, (size_t)2u);
        check_not_null(output.values[0].resource);
        check_not_null(output.values[1].resource);
        check_true(output.values[0].resource != owner.values[0].resource);
        check_true(output.values[1].resource != owner.values[1].resource);
        check_equal(*output.values[0].resource, 3);
        check_equal(*output.values[1].resource, 7);
        check_equal(collector_state.aborts, (size_t)0u);
        check_equal(lifecycle_test_copies, (size_t)4u);
        check_equal(lifecycle_test_destroys, (size_t)2u);

        lifecycle_test_destroy(&owner.values[0]);
        lifecycle_test_destroy(&owner.values[1]);
        lifecycle_collect_output_destroy(&output);
        check_equal(lifecycle_test_destroys, (size_t)6u);
        cflow_stream_destroy(&stream);
    }

    it("aborts a managed collector when its copy construction fails") {
        lifecycle_range_owner owner = {{
            lifecycle_test_make(13), lifecycle_test_make(17)
        }, 2u};
        lifecycle_collect_output output = {0};
        lifecycle_collector_state collector_state = {0};
        cmeta_collector collector =
            lifecycle_collector(&collector_state, &output);
        cmeta_range range = {
            .object = &owner,
            .element_type = &lifecycle_test_type,
            .flags = CMETA_RANGE_SIZED | CMETA_RANGE_CONSTRUCTS_VALUES,
            .size = lifecycle_range_size,
            .next = lifecycle_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_stream stream = {0};
        cflow_collect_result result;
        const char *error = NULL;

        lifecycle_test_reset();
        lifecycle_test_copy_fail_at = 4u;
        check_not_null(owner.values[0].resource);
        check_not_null(owner.values[1].resource);
        check_not_null(cflow_stream_from_range(&stream, range));
        result = cflow_eval_collect_result(&stream, &collector, &error);
        check_false(cflow_collect_result_is_ok(result));
        check_equal(result.status, CFLOW_STATUS_ALLOCATION_FAILED);
        check_equal(result.collector_status, CMETA_OUT_OF_MEMORY);
        check_equal(result.collector_state, CMETA_COLLECTOR_ABORTED);
        check_equal(result.count, (size_t)1u);
        check_not_null(error);
        check_true(collector.state == CMETA_COLLECTOR_ABORTED);
        check_true(collector.status == CMETA_OUT_OF_MEMORY);
        check_equal(collector_state.aborts, (size_t)1u);
        check_equal(output.count, (size_t)0u);
        check_equal(lifecycle_test_copies, (size_t)4u);
        check_equal(lifecycle_test_destroys, (size_t)3u);

        lifecycle_test_destroy(&owner.values[0]);
        lifecycle_test_destroy(&owner.values[1]);
        check_equal(lifecycle_test_destroys, (size_t)5u);
        cflow_stream_destroy(&stream);
    }

    it("rejects a legacy managed range before starting collection") {
        lifecycle_range_owner owner = {{
            lifecycle_test_make(23), {0}
        }, 1u};
        lifecycle_collect_output output = {0};
        lifecycle_collector_state collector_state = {0};
        cmeta_collector collector =
            lifecycle_collector(&collector_state, &output);
        cmeta_range range = {
            .object = &owner,
            .element_type = &lifecycle_test_type,
            .flags = CMETA_RANGE_SIZED,
            .size = lifecycle_range_size,
            .next = lifecycle_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_stream stream = {0};
        const char *error = NULL;

        lifecycle_test_reset();
        check_not_null(owner.values[0].resource);
        check_not_null(cflow_stream_from_range(&stream, range));
        check_false(cflow_eval_collect(&stream, &collector, &error));
        check_equal(error, "managed range must construct values");
        check_true(collector.state == CMETA_COLLECTOR_ABORTED);
        check_true(collector.status == CMETA_INVALID_ARGUMENT);
        check_equal(collector_state.aborts, (size_t)0u);
        check_equal(output.count, (size_t)0u);
        check_equal(lifecycle_test_copies, (size_t)0u);

        lifecycle_test_destroy(&owner.values[0]);
        cflow_stream_destroy(&stream);
        check_equal(lifecycle_test_destroys, (size_t)1u);
    }

    it("reports missing range traits before starting collection") {
        lifecycle_range_owner owner = {0};
        cmeta_type_desc missing_traits_type = lifecycle_test_type;
        lifecycle_collect_output output = {0};
        lifecycle_collector_state collector_state = {0};
        cmeta_collector collector =
            lifecycle_collector(&collector_state, &output);
        cmeta_range range = {
            .object = &owner,
            .element_type = &missing_traits_type,
            .flags = CMETA_RANGE_SIZED,
            .size = lifecycle_range_size,
            .next = lifecycle_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_stream stream = {0};
        cflow_collect_result result;
        const char *error = NULL;

        missing_traits_type.traits = NULL;
        collector.input_type = &missing_traits_type;
        check_not_null(cflow_stream_from_range(&stream, range));
        result = cflow_eval_collect_result(&stream, &collector, &error);
        check_false(cflow_collect_result_is_ok(result));
        check_equal(result.status, CFLOW_STATUS_UNSUPPORTED);
        check_equal(result.collector_status, CMETA_TRAIT_MISSING);
        check_equal(result.collector_state, CMETA_COLLECTOR_ABORTED);
        check_equal(result.count, (size_t)0u);
        check_equal(error,
                    "range element type lacks required lifecycle traits");
        check_true(collector.state == CMETA_COLLECTOR_ABORTED);
        check_true(collector.status == CMETA_TRAIT_MISSING);
        check_equal(collector_state.aborts, (size_t)0u);
        check_equal(output.count, (size_t)0u);

        cflow_stream_destroy(&stream);
    }

    it("zeroes byte results when managed range admission fails") {
        lifecycle_range_owner owner = {{
            lifecycle_test_make(31), {0}
        }, 1u};
        cmeta_range range = {
            .object = &owner,
            .element_type = &lifecycle_test_type,
            .flags = CMETA_RANGE_SIZED,
            .size = lifecycle_range_size,
            .next = lifecycle_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_stream stream = {0};
        cflow_result result = {&owner, 1u, &lifecycle_test_type};
        cflow_result limited = {&owner, 1u, &lifecycle_test_type};
        cflow_status_result status;
        cflow_status_result limited_status;

        lifecycle_test_reset();
        check_not_null(owner.values[0].resource);
        check_not_null(cflow_stream_from_range(&stream, range));
        status = cflow_eval_stream_result(&stream, &result);
        check_equal(status.status, CFLOW_STATUS_INVALID_ARGUMENT);
        check_null(result.data);
        check_equal(result.count, (size_t)0u);
        check_null(result.type);
        limited_status =
            cflow_eval_stream_limit_result(&stream, 1u, &limited);
        check_equal(limited_status.status, CFLOW_STATUS_INVALID_ARGUMENT);
        check_null(limited.data);
        check_equal(limited.count, (size_t)0u);
        check_null(limited.type);
        check_equal(lifecycle_test_copies, (size_t)0u);

        lifecycle_test_destroy(&owner.values[0]);
        if (!result.data)
            cflow_result_destroy(&result);
        if (!limited.data)
            cflow_result_destroy(&limited);
        cflow_stream_destroy(&stream);
        check_equal(lifecycle_test_destroys, (size_t)1u);
    }

    it("keeps byte results fail-fast for managed streams") {
        lifecycle_range_owner owner = {{
            lifecycle_test_make(29), {0}
        }, 1u};
        cmeta_range range = {
            .object = &owner,
            .element_type = &lifecycle_test_type,
            .flags = CMETA_RANGE_SIZED | CMETA_RANGE_CONSTRUCTS_VALUES,
            .size = lifecycle_range_size,
            .next = lifecycle_range_next,
            .version = 0u,
            .current_version = NULL
        };
        cflow_stream stream = {0};
        cflow_result result = {0};
        cflow_status_result status;

        lifecycle_test_reset();
        check_not_null(owner.values[0].resource);
        check_not_null(cflow_stream_from_range(&stream, range));
        status = cflow_eval_stream_result(&stream, &result);
        check_equal(status.status, CFLOW_STATUS_UNSUPPORTED);
        check_null(result.data);
        check_equal(result.count, (size_t)0u);
        check_null(result.type);
        check_equal(lifecycle_test_copies, (size_t)0u);

        lifecycle_test_destroy(&owner.values[0]);
        cflow_result_destroy(&result);
        cflow_stream_destroy(&stream);
        check_equal(lifecycle_test_destroys, (size_t)1u);
    }

    it("rejects channels whose values require lifecycle callbacks") {
        cflow_channel channel = {0};
        const bool initialized = cflow_channel_init(
            &channel, &cflow_test_owned_value_type, 1u);

        check_false(initialized);
        if (initialized)
            cflow_channel_destroy(&channel);
    }

    it("rejects readiness sources whose values require lifecycle callbacks") {
        cflow_publisher source = {0};
        const bool initialized = cflow_publisher_from_readiness(
            &source,
            "owned_readiness",
            &cflow_test_owned_value_type,
            destroy_reentrant_read,
            destroy_reentrant_arm,
            NULL,
            NULL,
            NULL);

        check_false(initialized);
        check_null(source.self);
        if (initialized)
            cflow_publisher_destroy(&source);
    }

    it("rejects channel storage size overflow") {
        static const cmeta_type_traits trivial_traits = {
            .flags = CMETA_TRAIT_TRIVIAL_COPY |
                     CMETA_TRAIT_TRIVIAL_DESTROY
        };
        const cmeta_type_desc three_byte_type = {
            .name = "three_byte",
            .size = 3u,
            .align = 1u,
            .kind = CMETA_T_OBJECT,
            .pointee = NULL,
            .traits = &trivial_traits,
            .identity = NULL
        };
        cflow_channel channel = {0};
        const size_t overflowing_capacity = SIZE_MAX / three_byte_type.size + 1u;
        const bool initialized = cflow_channel_init(
            &channel, &three_byte_type, overflowing_capacity);

        check_false(initialized);
        if (initialized)
            cflow_channel_destroy(&channel);
    }

    it("preserves a live Source on rejected second construction") {
        const int value = 17;
        cflow_publisher source = {0};
        cflow_channel channel = {0};
        cflow_publish_context resume_context = {0};
        cflow_step step;
        cmeta_range range = {
            .object = &value,
            .element_type = &cmeta_type_int,
            .flags = CMETA_RANGE_SIZED,
            .size = owned_range_size,
            .next = owned_range_next
        };
        void *source_owner;
        int output = 0;

        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, &value, 1u));
        source_owner = source.self;
        check_false(cflow_publisher_from_array(
            &source, &cmeta_type_int, &value, 1u));
        check_equal(source.self, source_owner);
        check_false(cflow_publisher_from_range(&source, range));
        check_equal(source.self, source_owner);
        check_false(cflow_publisher_from_timer(&source, 1u, 1u));
        check_equal(source.self, source_owner);
        check_true(cflow_channel_init(&channel, &cmeta_type_int, 2u));
        check_false(cflow_publisher_from_channel(&source, &channel));
        check_equal(source.self, source_owner);
        check_false(cflow_publisher_from_readiness(
            &source, "occupied", &cmeta_type_int,
            destroy_reentrant_read, destroy_reentrant_arm,
            destroy_reentrant_cancel, NULL, NULL));
        check_equal(source.self, source_owner);
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, value);

        cflow_publisher_destroy(&source);
        cflow_channel_destroy(&channel);
    }

    it("preserves a live Channel on rejected second initialization") {
        cflow_channel channel = {0};
        cflow_channel_stats stats = {0};
        const int value = 23;
        void *channel_owner;

        check_true(cflow_channel_init(&channel, &cmeta_type_int, 2u));
        channel_owner = channel.impl;
        check_false(cflow_channel_init(&channel, &cmeta_type_int, 3u));
        check_equal(channel.impl, channel_owner);
        check_equal(cflow_channel_try_push(&channel, &value),
                    CFLOW_CHANNEL_OK);
        check_true(cflow_channel_get_stats(&channel, &stats));
        check_equal(stats.capacity, (size_t)2u);
        check_equal(stats.pending, (size_t)1u);

        cflow_channel_destroy(&channel);
    }

    it("reports exact bounded Channel admission and statistics") {
        cflow_channel channel = {0};
        cflow_channel_stats stats = {0};
        cflow_publisher source = {0};
        cflow_publish_context resume_context = {0};
        cflow_step step;
        int value = 1;
        int output = 0;

        check_equal(cflow_channel_try_push(NULL, &value),
                    CFLOW_CHANNEL_INVALID_ARGUMENT);
        check_true(cflow_channel_init(&channel, &cmeta_type_int, 1u));
        check_equal(cflow_channel_try_push(&channel, &value),
                    CFLOW_CHANNEL_OK);
        value = 2;
        check_equal(cflow_channel_try_push(&channel, &value),
                    CFLOW_CHANNEL_FULL);
        check_false(cflow_channel_push(&channel, &value));
        check_true(cflow_channel_get_stats(&channel, &stats));
        check_equal(stats.capacity, (size_t)1u);
        check_equal(stats.pending, (size_t)1u);
        check_equal(stats.peak_pending, (size_t)1u);
        check_equal(stats.accepted, UINT64_C(1));
        check_equal(stats.received, UINT64_C(0));
        check_equal(stats.rejected_full, UINT64_C(2));
        check_equal(stats.rejected_closed, UINT64_C(0));

        check_true(cflow_publisher_from_channel(&source, &channel));
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE);
        check_equal(output, 1);
        check_true(cflow_channel_get_stats(&channel, &stats));
        check_equal(stats.pending, (size_t)0u);
        check_equal(stats.received, UINT64_C(1));

        cflow_channel_close(&channel);
        check_equal(cflow_channel_try_push(&channel, &value),
                    CFLOW_CHANNEL_CLOSED);
        check_true(cflow_channel_get_stats(&channel, &stats));
        check_equal(stats.rejected_closed, UINT64_C(1));
        cflow_publisher_destroy(&source);
        cflow_channel_destroy(&channel);
    }

    it("requires readiness cancellation and orders it before resource close") {
        readiness_order_state state = {0};
        cflow_publisher source = {0};

        check_false(cflow_publisher_from_readiness(
            &source, "missing_cancel", &cmeta_type_int,
            destroy_reentrant_read, destroy_reentrant_arm,
            NULL, readiness_order_close, &state));
        check_false(cflow_publisher_valid(&source));
        check_true(cflow_publisher_from_readiness(
            &source, "ordered", &cmeta_type_int,
            destroy_reentrant_read, destroy_reentrant_arm,
            readiness_order_cancel, readiness_order_close, &state));
        cflow_publisher_cancel(&source);
        cflow_publisher_destroy(&source);

        check_equal(state.cancel_calls, (size_t)2u);
        check_equal(state.cancel_order, (size_t)2u);
        check_equal(state.close_order, (size_t)3u);
    }

    it("waits for an executing Timer wake before destroy returns") {
        pending_foreign_scheduler_state scheduler_state = {0};
        cflow_scheduler scheduler;
        cflow_publisher source = {0};
        cflow_publish_context resume_context;
        cflow_step step;
        timer_wake_gate gate = {0};
        pending_foreign_run_context run_context = {&scheduler_state};
        source_destroy_context destroy_context = {&source};
        cmeta_thread_t run_thread = 0;
        cmeta_thread_t destroy_thread = 0;
        size_t output = 0u;

        cmeta_mutex_init(&scheduler_state.mutex);
        cmeta_cond_init(&scheduler_state.changed);
        cmeta_mutex_init(&gate.mutex);
        cmeta_cond_init(&gate.changed);
        scheduler = pending_foreign_scheduler_as_cflow_scheduler(
            &scheduler_state);
        resume_context = (cflow_publish_context){&scheduler};
        check_true(cflow_publisher_from_timer(&source, 1u, 1u));
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){timer_blocking_wake, &gate}));
        check_equal(cmeta_thread_create(
            &run_thread, pending_foreign_run_one_thread, &run_context), 0);
        cmeta_mutex_lock(&gate.mutex);
        while (!gate.entered)
            cmeta_cond_wait(&gate.changed, &gate.mutex);
        cmeta_mutex_unlock(&gate.mutex);
        check_equal(cmeta_thread_create(
            &destroy_thread, source_destroy_thread, &destroy_context), 0);
        if (!runtime_wait_until_at_least(&destroy_context.started, 1)) abort();
        cmeta_sleep_ms(20u);
        check_equal(atomic_load(&destroy_context.returned), 0);

        cmeta_mutex_lock(&gate.mutex);
        gate.release = true;
        cmeta_cond_broadcast(&gate.changed);
        cmeta_mutex_unlock(&gate.mutex);
        check_equal(cmeta_thread_join(&run_thread), 0);
        check_equal(cmeta_thread_join(&destroy_thread), 0);
        check_equal(atomic_load(&destroy_context.returned), 1);
        check_false(cflow_publisher_valid(&source));

        cflow_scheduler_destroy(&scheduler);
        cmeta_cond_destroy(&gate.changed);
        cmeta_mutex_destroy(&gate.mutex);
        cmeta_cond_destroy(&scheduler_state.changed);
        cmeta_mutex_destroy(&scheduler_state.mutex);
    }

    it("survives inline Timer wake destruction during scheduler admission") {
        pending_foreign_scheduler_state scheduler_state = {0};
        cflow_scheduler scheduler;
        cflow_publisher source = {0};
        cflow_publish_context resume_context;
        cflow_step step;
        reentrant_timer_wake_state wake_state = {&source, false};
        size_t output = 0u;

        cmeta_mutex_init(&scheduler_state.mutex);
        cmeta_cond_init(&scheduler_state.changed);
        scheduler_state.run_inline = true;
        scheduler = pending_foreign_scheduler_as_cflow_scheduler(
            &scheduler_state);
        resume_context = (cflow_publish_context){&scheduler};
        check_true(cflow_publisher_from_timer(&source, 1u, 1u));
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable,
            (cflow_waker){timer_reentrant_destroy_wake, &wake_state}));
        check_true(wake_state.returned);
        check_false(cflow_publisher_valid(&source));

        cflow_scheduler_destroy(&scheduler);
        cmeta_cond_destroy(&scheduler_state.changed);
        cmeta_mutex_destroy(&scheduler_state.mutex);
    }

    it("rearms a Timer only after the prior callback settles") {
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_publish_context resume_context;
        cflow_step step;
        timer_wake_probe probe = {0};
        size_t output = SIZE_MAX;

        check_true(cflow_scheduler_test_init(&scheduler));
        resume_context = (cflow_publish_context){&scheduler};
        check_true(cflow_publisher_from_timer(&source, 2u, 1u));
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){timer_count_wake, &probe}));
        check_equal(cflow_scheduler_advance(&scheduler, 1u), (size_t)1u);
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE);
        check_equal(output, (size_t)0u);

        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){timer_count_wake, &probe}));
        check_equal(cflow_scheduler_advance(&scheduler, 1u), (size_t)1u);
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, (size_t)1u);
        check_equal(probe.wakes, (size_t)2u);

        cflow_publisher_destroy(&source);
        cflow_scheduler_destroy(&scheduler);
    }

    it("rejects array byte extent overflow") {
        unsigned char value = 0u;
        cflow_publisher source = {0};

        check_false(cflow_publisher_from_array(
            &source, &lifecycle_overaligned_type, &value,
            SIZE_MAX / lifecycle_overaligned_type.size + 1u));
        check_null(source.self);
    }

    it("allows a sink callback to close its run") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        close_from_sink_state state = {&run, 0u, false};
        cflow_subscriber_callbacks callbacks = {
            close_from_sink_value,
            close_from_sink_error,
            close_from_sink_done,
            &state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        const int input = 7;

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, &input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));

        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(state.values, (size_t)1u);
        check_true(state.close_returned);
        check_null(run.impl);

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("serializes callback and external close callers") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        concurrent_close_state state = {0};
        cflow_subscriber_callbacks callbacks = {
            concurrent_close_value,
            close_from_sink_error,
            close_from_sink_done,
            &state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        cmeta_thread_t external_thread;
        const int input = 11;

        state.run = &run;
        normalized.root = CMETA_INVALID_ID;
        cmeta_mutex_init(&state.lock);
        cmeta_cond_init(&state.changed);
        check_not_null(state.lock);
        check_not_null(state.changed);
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_worker_init(&scheduler, 1u));
        check_true(cflow_publisher_from_array(
            &source, &cmeta_type_int, &input, 1u));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        check_true(cflow_subscription_request(&run, 1u));

        cmeta_mutex_lock(&state.lock);
        while (!state.callback_entered)
            cmeta_cond_wait(&state.changed, &state.lock);
        cmeta_mutex_unlock(&state.lock);
        check_equal(cmeta_thread_create(
            &external_thread, concurrent_external_close, &state), 0);
        check_equal(cmeta_thread_join(&external_thread), 0);
        check_true(cflow_scheduler_wait_idle(&scheduler));

        check_true(state.callback_returned);
        check_true(state.external_returned);
        check_null(run.impl);

        cflow_subscription_close(&run);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
        cmeta_cond_destroy(&state.changed);
        cmeta_mutex_destroy(&state.lock);
    }

    it("allows a source destroy callback to close the same run") {
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_publisher source = {0};
        cflow_subscription run = {0};
        destroy_reentrant_close_state state = {&run, false};

        normalized.root = CMETA_INVALID_ID;
        cflow_graph_init(&surface, &cmeta_type_int);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_publisher_from_readiness(
            &source,
            "destroy_reentrant",
            &cmeta_type_int,
            destroy_reentrant_read,
            destroy_reentrant_arm,
            destroy_reentrant_cancel,
            destroy_reentrant_close,
            &state));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, NULL));

        cflow_subscription_close(&run);

        check_true(state.close_returned);
        check_null(run.impl);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }

    it("drives a Machine Source through Run demand") {
        const cflow_machine_state states[] = {
            {10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {20u, &cmeta_type_long, CFLOW_MACHINE_STATE_DONE}
        };
        const cflow_event_type events[] = {
            {100u, &cmeta_type_bool}
        };
        const cflow_machine_action actions[] = {
            {300u, &cmeta_type_int, 100u, &cmeta_type_bool,
             &cmeta_type_long, CMETA_EFFECT_PURE,
             CMETA_PROP_DETERMINISTIC | CMETA_PROP_TOTAL |
                 CMETA_PROP_NO_ALIAS,
             CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_long, 0u}
        };
        const cflow_machine_transition transitions[] = {
            {10u, 100u, 0u, 300u, 20u, 1u}
        };
        const cflow_machine_definition definition = {
            states, 2u, 10u, events, 1u, NULL, 0u,
            actions, 1u, transitions, 1u
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, machine_run_action, NULL}
        };
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_publisher source = {0};
        cflow_graph surface = {0};
        cflow_graph normalized = {0};
        cflow_scheduler scheduler = {0};
        cflow_subscription run = {0};
        machine_run_sink_state sink_state = {0};
        cflow_subscriber_callbacks callbacks = {
            machine_run_sink_value,
            machine_run_sink_error,
            machine_run_sink_done,
            &sink_state
        };
        cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
        const int initial = 5;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            NULL, 0u, action_bindings, 1u, 4u, &executor
        };

        normalized.root = CMETA_INVALID_ID;
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_publisher(&instance, &source));
        cflow_graph_init(&surface, &cmeta_type_long);
        check_true(cflow_graph_normalize(&normalized, &surface));
        check_true(cflow_scheduler_test_init(&scheduler));
        check_true(cflow_subscribe(
            &run, &normalized, &source, &scheduler, &sink));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_subscription_request(&run, 1u));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
        check_true(cflow_executor_wait_idle(&executor));
        (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

        check_equal(sink_state.values, (size_t)1u);
        check_equal(sink_state.value, 42L);
        check_equal(sink_state.dones, (size_t)1u);
        check_null(sink_state.error);
        check_true(cflow_subscription_is_done(&run));

        cflow_subscription_close(&run);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
        cflow_scheduler_destroy(&scheduler);
        cflow_graph_destroy(&normalized);
        cflow_graph_destroy(&surface);
    }
}
