#include <cflow/temporal.h>

#include "value_storage.h"

#include <thread.h>

#include <stdlib.h>
#include <string.h>

#define CFLOW_TEMPORAL_RESUME_QUANTUM 64u
#define CFLOW_TEMPORAL_TIMEOUT_ERROR "temporal source timed out"

typedef enum temporal_kind {
    TEMPORAL_DELAY = 0,
    TEMPORAL_DEBOUNCE,
    TEMPORAL_TIMEOUT
} temporal_kind;

typedef enum temporal_ready_cause {
    TEMPORAL_READY_NONE = 0,
    TEMPORAL_READY_INNER,
    TEMPORAL_READY_TIMER
} temporal_ready_cause;

typedef struct temporal_state {
    temporal_kind kind;
    cflow_publisher inner;
    const cmeta_type_desc *type;
    cflow_value_slot pending;
    cflow_value_slot scratch;
    uint64_t duration_ticks;
    cflow_scheduler *scheduler;
    cflow_task_id timer_task;
    cflow_waitable inner_waitable;
    cflow_waker waiter;
    cflow_waker terminal_waker;
    cmeta_mutex_t lock;
    cmeta_cond_t timer_changed;
    temporal_ready_cause ready_cause;
    bool timer_arming;
    bool timer_outstanding;
    bool timer_stale;
    bool inner_armed;
    bool need_inner;
    bool need_timer;
    bool yielding;
    bool inner_done;
    bool done;
    bool cancelled;
    const char *error;
} temporal_state;

static cflow_step temporal_error_step(const char *error) {
    return (cflow_step){
        CFLOW_STEP_ERROR, {0}, error != NULL ? error : "temporal source error"
    };
}

static void invoke_waker(cflow_waker waker) {
    if (waker.wake != NULL) waker.wake(waker.user);
}

static uint64_t duration_to_scheduler_ticks(cflow_duration duration) {
    const uint64_t ns_per_tick = 1000000u;
    return duration.ns == 0u
        ? 0u : (duration.ns - 1u) / ns_per_tick + 1u;
}

static bool temporal_emit(cflow_value_slot *slot, void *out_value) {
    if (slot == NULL || !slot->live || out_value == NULL) return false;
    if (cflow_value_storage_type_supported(slot->type)) {
        memcpy(out_value, slot->storage, slot->type->size);
    } else {
        slot->type->traits->move_construct(out_value, slot->storage);
        slot->type->traits->destroy(slot->storage);
    }
    slot->live = false;
    return true;
}

static void temporal_timer_fire(void *user) {
    temporal_state *state = (temporal_state *)user;
    cflow_waker waker = {0};
    if (state == NULL) return;
    cmeta_mutex_lock(&state->lock);
    {
        const bool stale = state->timer_stale;
        state->timer_stale = false;
        state->timer_outstanding = false;
        state->timer_task = 0u;
        cmeta_cond_broadcast(&state->timer_changed);
        if (!state->cancelled && !state->done && state->error == NULL &&
            (state->timer_arming || state->need_timer) &&
            state->ready_cause == TEMPORAL_READY_NONE) {
            state->timer_arming = false;
            state->ready_cause = stale
                ? TEMPORAL_READY_INNER : TEMPORAL_READY_TIMER;
            waker = state->waiter;
            state->waiter = (cflow_waker){0};
        }
    }
    cmeta_mutex_unlock(&state->lock);
    invoke_waker(waker);
}

static bool temporal_cancel_scheduled(temporal_state *state,
                                      cflow_scheduler *scheduler,
                                      cflow_task_id timer_task) {
    if (timer_task != 0u && scheduler != NULL &&
        cflow_scheduler_cancel(scheduler, timer_task)) {
        cmeta_mutex_lock(&state->lock);
        state->timer_outstanding = false;
        cmeta_cond_broadcast(&state->timer_changed);
        cmeta_mutex_unlock(&state->lock);
        return true;
    }
    return timer_task == 0u;
}

static void temporal_cancel_active_wait(temporal_state *state) {
    cflow_scheduler *scheduler;
    cflow_task_id timer_task;
    cflow_waitable inner_waitable = {0};
    bool inner_armed;
    cmeta_mutex_lock(&state->lock);
    scheduler = state->scheduler;
    timer_task = state->timer_task;
    state->timer_task = 0u;
    if (timer_task == 0u && state->timer_arming) {
        state->timer_outstanding = false;
        cmeta_cond_broadcast(&state->timer_changed);
    }
    state->timer_arming = false;
    inner_armed = state->inner_armed;
    state->inner_armed = false;
    if (inner_armed) inner_waitable = state->inner_waitable;
    state->waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&state->lock);
    (void)temporal_cancel_scheduled(state, scheduler, timer_task);
    if (inner_armed && cflow_waitable_valid(&inner_waitable))
        cflow_waitable_cancel(&inner_waitable);
}

static void temporal_restart_timer(temporal_state *state) {
    cflow_scheduler *scheduler;
    cflow_task_id timer_task;
    cmeta_mutex_lock(&state->lock);
    scheduler = state->scheduler;
    timer_task = state->timer_task;
    state->timer_task = 0u;
    cmeta_mutex_unlock(&state->lock);
    if (!temporal_cancel_scheduled(state, scheduler, timer_task)) {
        cmeta_mutex_lock(&state->lock);
        if (state->timer_outstanding) state->timer_stale = true;
        cmeta_mutex_unlock(&state->lock);
    }
}

static bool temporal_wait_arm(void *user, cflow_waker waker) {
    temporal_state *state = (temporal_state *)user;
    cflow_waitable inner_waitable = {0};
    cflow_scheduler *scheduler;
    cflow_schedule_result scheduled = {
        CFLOW_ADMISSION_INVALID_ARGUMENT, 0u
    };
    bool need_inner;
    bool need_timer;
    uint64_t ticks;
    bool cancel_scheduled = false;

    if (state == NULL || waker.wake == NULL) return false;
    cmeta_mutex_lock(&state->lock);
    if (state->waiter.wake != NULL) {
        cmeta_mutex_unlock(&state->lock);
        return false;
    }
    if (state->cancelled || state->done || state->error != NULL ||
        state->ready_cause != TEMPORAL_READY_NONE) {
        cmeta_mutex_unlock(&state->lock);
        invoke_waker(waker);
        return true;
    }
    state->waiter = waker;
    scheduler = state->scheduler;
    need_inner = state->need_inner;
    need_timer = state->need_timer && !state->timer_stale &&
                 !state->timer_outstanding;
    ticks = state->yielding ? 0u : state->duration_ticks;
    if (need_inner) inner_waitable = state->inner_waitable;
    state->timer_arming = need_timer;
    state->timer_outstanding = need_timer;
    cmeta_mutex_unlock(&state->lock);

    if (need_inner) {
        cmeta_mutex_lock(&state->lock);
        state->inner_armed = true;
        cmeta_mutex_unlock(&state->lock);
        if (!cflow_waitable_valid(&inner_waitable) ||
            !cflow_waitable_arm(&inner_waitable, waker)) {
            temporal_cancel_active_wait(state);
            return false;
        }
    }
    if (need_timer) {
        if (scheduler == NULL) {
            temporal_cancel_active_wait(state);
            return false;
        }
        scheduled = cflow_scheduler_try_post_after(
            scheduler, ticks, temporal_timer_fire, state);
        if (scheduled.status != CFLOW_ADMISSION_ACCEPTED) {
            cmeta_mutex_lock(&state->lock);
            state->timer_outstanding = false;
            state->timer_arming = false;
            cmeta_cond_broadcast(&state->timer_changed);
            cmeta_mutex_unlock(&state->lock);
            temporal_cancel_active_wait(state);
            return false;
        }
        cmeta_mutex_lock(&state->lock);
        if (state->ready_cause != TEMPORAL_READY_NONE ||
            state->cancelled || state->done || state->error != NULL) {
            cancel_scheduled = true;
        } else {
            state->timer_task = scheduled.task_id;
        }
        state->timer_arming = false;
        cmeta_cond_broadcast(&state->timer_changed);
        cmeta_mutex_unlock(&state->lock);
        if (cancel_scheduled)
            (void)temporal_cancel_scheduled(
                state, scheduler, scheduled.task_id);
    }
    return true;
}

static void temporal_wait_cancel(void *user) {
    temporal_state *state = (temporal_state *)user;
    if (state != NULL) temporal_cancel_active_wait(state);
}

CMETA_IMPLEMENTS(cflow_waitable, temporal_waitable, 0,
    .arm = temporal_wait_arm,
    .cancel = temporal_wait_cancel
);

static temporal_ready_cause temporal_take_ready(temporal_state *state) {
    temporal_ready_cause cause;
    cflow_waitable inner_waitable = {0};
    bool inner_armed;
    cmeta_mutex_lock(&state->lock);
    cause = state->ready_cause;
    state->ready_cause = TEMPORAL_READY_NONE;
    inner_armed = state->inner_armed;
    state->inner_armed = false;
    if (inner_armed) inner_waitable = state->inner_waitable;
    state->waiter = (cflow_waker){0};
    state->need_inner = false;
    state->need_timer = false;
    cmeta_mutex_unlock(&state->lock);
    if (inner_armed && cflow_waitable_valid(&inner_waitable))
        cflow_waitable_cancel(&inner_waitable);
    return cause;
}

static cflow_step temporal_wait(temporal_state *state,
                                bool need_inner,
                                bool need_timer) {
    cmeta_mutex_lock(&state->lock);
    state->need_inner = need_inner;
    state->need_timer = need_timer;
    cmeta_mutex_unlock(&state->lock);
    return (cflow_step){
        CFLOW_STEP_WAIT,
        temporal_waitable_as_cflow_waitable(state),
        NULL
    };
}

static void temporal_mark_done(temporal_state *state) {
    cflow_waker waker;
    cmeta_mutex_lock(&state->lock);
    state->done = true;
    waker = state->terminal_waker;
    state->terminal_waker = (cflow_waker){0};
    cmeta_mutex_unlock(&state->lock);
    invoke_waker(waker);
}

static void temporal_mark_error(temporal_state *state, const char *error) {
    cflow_waker waker;
    cmeta_mutex_lock(&state->lock);
    state->error = error != NULL ? error : "temporal source error";
    waker = state->terminal_waker;
    state->terminal_waker = (cflow_waker){0};
    cmeta_mutex_unlock(&state->lock);
    invoke_waker(waker);
}

static cflow_step delay_resume(temporal_state *state,
                               cflow_publish_context *context,
                               void *out_value,
                               temporal_ready_cause cause) {
    cflow_step step;
    if (state->pending.live) {
        if (cause != TEMPORAL_READY_TIMER)
            return temporal_wait(state, false, true);
        if (!temporal_emit(&state->pending, out_value))
            return temporal_error_step("delay value move failed");
        if (state->inner_done) {
            temporal_mark_done(state);
            return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
        }
        return (cflow_step){CFLOW_STEP_VALUE, {0}, NULL};
    }
    step = cflow_publisher_resume(
        &state->inner, context, state->pending.storage);
    if (step.kind == CFLOW_STEP_VALUE ||
        step.kind == CFLOW_STEP_VALUE_AND_DONE) {
        state->pending.live = true;
        state->inner_done = step.kind == CFLOW_STEP_VALUE_AND_DONE;
        return temporal_wait(state, false, true);
    }
    if (step.kind == CFLOW_STEP_WAIT) {
        state->inner_waitable = step.waitable;
        return temporal_wait(state, true, false);
    }
    if (step.kind == CFLOW_STEP_DONE) temporal_mark_done(state);
    else if (step.kind == CFLOW_STEP_ERROR)
        temporal_mark_error(state, step.error);
    return step;
}

static cflow_step debounce_resume(temporal_state *state,
                                  cflow_publish_context *context,
                                  void *out_value,
                                  temporal_ready_cause cause) {
    size_t count;
    bool timer_expired = cause == TEMPORAL_READY_TIMER;
    bool timer_restart = false;
    if (cause == TEMPORAL_READY_TIMER && state->yielding) {
        state->yielding = false;
    }
    for (count = 0u; count < CFLOW_TEMPORAL_RESUME_QUANTUM; ++count) {
        cflow_step step = cflow_publisher_resume(
            &state->inner, context, state->scratch.storage);
        if (step.kind == CFLOW_STEP_VALUE ||
            step.kind == CFLOW_STEP_VALUE_AND_DONE) {
            state->scratch.live = true;
            cflow_value_slot_reset(&state->pending);
            if (!cflow_value_slot_move(&state->pending, &state->scratch)) {
                temporal_mark_error(state, "debounce value move failed");
                return temporal_error_step(state->error);
            }
            timer_expired = false;
            timer_restart = true;
            if (step.kind == CFLOW_STEP_VALUE_AND_DONE) {
                state->inner_done = true;
                if (!temporal_emit(&state->pending, out_value))
                    return temporal_error_step("debounce final move failed");
                temporal_mark_done(state);
                return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
            }
            continue;
        }
        if (step.kind == CFLOW_STEP_WAIT) {
            state->inner_waitable = step.waitable;
            if (timer_restart) temporal_restart_timer(state);
            if (timer_expired && state->pending.live) {
                if (!temporal_emit(&state->pending, out_value))
                    return temporal_error_step("debounce value move failed");
                return (cflow_step){CFLOW_STEP_VALUE, {0}, NULL};
            }
            return temporal_wait(state, true, state->pending.live);
        }
        if (step.kind == CFLOW_STEP_DONE) {
            if (state->pending.live) {
                if (!temporal_emit(&state->pending, out_value))
                    return temporal_error_step("debounce final move failed");
                temporal_mark_done(state);
                return (cflow_step){CFLOW_STEP_VALUE_AND_DONE, {0}, NULL};
            }
            temporal_mark_done(state);
            return step;
        }
        temporal_mark_error(state, step.error);
        cflow_value_slot_reset(&state->pending);
        return step;
    }
    state->yielding = true;
    return temporal_wait(state, false, true);
}

static cflow_step timeout_resume(temporal_state *state,
                                 cflow_publish_context *context,
                                 void *out_value,
                                 temporal_ready_cause cause) {
    cflow_step step;
    step = cflow_publisher_resume(&state->inner, context, out_value);
    if (step.kind == CFLOW_STEP_WAIT) {
        if (cause == TEMPORAL_READY_TIMER) {
            temporal_mark_error(state, CFLOW_TEMPORAL_TIMEOUT_ERROR);
            return temporal_error_step(CFLOW_TEMPORAL_TIMEOUT_ERROR);
        }
        state->inner_waitable = step.waitable;
        return temporal_wait(state, true, true);
    }
    temporal_restart_timer(state);
    if (step.kind == CFLOW_STEP_VALUE_AND_DONE ||
        step.kind == CFLOW_STEP_DONE)
        temporal_mark_done(state);
    else if (step.kind == CFLOW_STEP_ERROR)
        temporal_mark_error(state, step.error);
    return step;
}

static cflow_step temporal_resume(void *user,
                                  cflow_publish_context *context,
                                  void *out_value) {
    temporal_state *state = (temporal_state *)user;
    temporal_ready_cause cause;
    if (state == NULL || context == NULL || context->scheduler == NULL ||
        out_value == NULL)
        return temporal_error_step("temporal source has no scheduler");
    cmeta_mutex_lock(&state->lock);
    if (state->scheduler != NULL && state->scheduler != context->scheduler) {
        cmeta_mutex_unlock(&state->lock);
        return temporal_error_step("temporal source scheduler changed");
    }
    state->scheduler = context->scheduler;
    if (state->error != NULL) {
        const char *error = state->error;
        cmeta_mutex_unlock(&state->lock);
        return temporal_error_step(error);
    }
    if (state->done || state->cancelled) {
        cmeta_mutex_unlock(&state->lock);
        return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    }
    cmeta_mutex_unlock(&state->lock);
    cause = temporal_take_ready(state);
    if (state->kind == TEMPORAL_DELAY)
        return delay_resume(state, context, out_value, cause);
    if (state->kind == TEMPORAL_DEBOUNCE)
        return debounce_resume(state, context, out_value, cause);
    return timeout_resume(state, context, out_value, cause);
}

static void temporal_cancel(void *user) {
    temporal_state *state = (temporal_state *)user;
    if (state == NULL) return;
    cmeta_mutex_lock(&state->lock);
    state->cancelled = true;
    cmeta_mutex_unlock(&state->lock);
    temporal_cancel_active_wait(state);
    cflow_publisher_cancel(&state->inner);
}

static void temporal_destroy(void *user) {
    temporal_state *state = (temporal_state *)user;
    if (state == NULL) return;
    temporal_cancel(state);
    cmeta_mutex_lock(&state->lock);
    while (state->timer_outstanding || state->timer_arming)
        cmeta_cond_wait(&state->timer_changed, &state->lock);
    cmeta_mutex_unlock(&state->lock);
    if (cflow_publisher_valid(&state->inner)) {
        cflow_publisher_bind_terminal_waker(&state->inner, (cflow_waker){0});
        cflow_publisher_destroy(&state->inner);
    }
    cflow_value_slot_destroy(&state->scratch);
    cflow_value_slot_destroy(&state->pending);
    cmeta_cond_destroy(&state->timer_changed);
    cmeta_mutex_destroy(&state->lock);
    free(state);
}

static const char *temporal_name(void *user) {
    temporal_state *state = (temporal_state *)user;
    if (state == NULL) return "temporal";
    if (state->kind == TEMPORAL_DELAY) return "delay";
    if (state->kind == TEMPORAL_DEBOUNCE) return "debounce";
    return "timeout";
}

static const cmeta_type_desc *temporal_type(void *user) {
    temporal_state *state = (temporal_state *)user;
    return state != NULL ? state->type : NULL;
}

static void temporal_bind_terminal(void *user, cflow_waker waker) {
    temporal_state *state = (temporal_state *)user;
    bool terminal;
    if (state == NULL) return;
    cmeta_mutex_lock(&state->lock);
    state->terminal_waker = waker;
    terminal = state->done || state->error != NULL;
    cmeta_mutex_unlock(&state->lock);
    cflow_publisher_bind_terminal_waker(&state->inner, waker);
    if (terminal) invoke_waker(waker);
}

static cflow_publisher_terminal temporal_poll_terminal(
    void *user, const char **out_error) {
    temporal_state *state = (temporal_state *)user;
    bool pending;
    const char *error;
    bool done;
    temporal_ready_cause cause;
    if (out_error != NULL) *out_error = NULL;
    if (state == NULL) return CFLOW_PUBLISHER_ERROR;
    cmeta_mutex_lock(&state->lock);
    error = state->error;
    done = state->done;
    pending = state->pending.live;
    cause = state->ready_cause;
    cmeta_mutex_unlock(&state->lock);
    if (error != NULL) {
        if (out_error != NULL) *out_error = error;
        return CFLOW_PUBLISHER_ERROR;
    }
    if (done) return CFLOW_PUBLISHER_DONE;
    if (state->kind == TEMPORAL_TIMEOUT &&
        cause == TEMPORAL_READY_TIMER)
        return CFLOW_PUBLISHER_OPEN;
    if (!pending)
        return cflow_publisher_poll_terminal(&state->inner, out_error);
    return CFLOW_PUBLISHER_OPEN;
}

CMETA_IMPLEMENTS(cflow_publisher, temporal_source,
    CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES,
    .name = temporal_name,
    .output_type = temporal_type,
    .resume = temporal_resume,
    .cancel = temporal_cancel,
    .destroy = temporal_destroy,
    .bind_terminal_waker = temporal_bind_terminal,
    .poll_terminal = temporal_poll_terminal
);

static bool temporal_source_init(cflow_publisher *out,
                                 cflow_publisher *inner,
                                 cflow_duration duration,
                                 temporal_kind kind) {
    temporal_state *state;
    const cmeta_type_desc *type;
    if (out == NULL || inner == NULL || cflow_publisher_valid(out) ||
        !cflow_publisher_valid(inner))
        return false;
    type = cflow_publisher_output_type(inner);
    if (!cflow_value_type_supported(type) ||
        (!cflow_value_storage_type_supported(type) &&
         !cflow_publisher_has(inner, CFLOW_PUBLISHER_CAP_CONSTRUCTS_VALUES)))
        return false;
    state = (temporal_state *)calloc(1u, sizeof(*state));
    if (state == NULL) return false;
    state->kind = kind;
    state->type = type;
    state->duration_ticks = duration_to_scheduler_ticks(duration);
    cmeta_mutex_init(&state->lock);
    cmeta_cond_init(&state->timer_changed);
    if (state->lock == NULL || state->timer_changed == NULL ||
        (kind != TEMPORAL_TIMEOUT &&
         !cflow_value_slot_init(&state->pending, type)) ||
        (kind == TEMPORAL_DEBOUNCE &&
         !cflow_value_slot_init(&state->scratch, type))) {
        cflow_value_slot_destroy(&state->scratch);
        cflow_value_slot_destroy(&state->pending);
        if (state->timer_changed != NULL)
            cmeta_cond_destroy(&state->timer_changed);
        if (state->lock != NULL) cmeta_mutex_destroy(&state->lock);
        free(state);
        return false;
    }
    state->inner = *inner;
    memset(inner, 0, sizeof(*inner));
    *out = temporal_source_as_cflow_publisher(state);
    return true;
}

bool cflow_publisher_delay(cflow_publisher *out,
                        cflow_publisher *inner,
                        cflow_duration delay) {
    return temporal_source_init(out, inner, delay, TEMPORAL_DELAY);
}

bool cflow_publisher_debounce(cflow_publisher *out,
                           cflow_publisher *inner,
                           cflow_duration quiet_period) {
    return temporal_source_init(
        out, inner, quiet_period, TEMPORAL_DEBOUNCE);
}

bool cflow_publisher_timeout(cflow_publisher *out,
                          cflow_publisher *inner,
                          cflow_duration timeout) {
    return temporal_source_init(out, inner, timeout, TEMPORAL_TIMEOUT);
}
