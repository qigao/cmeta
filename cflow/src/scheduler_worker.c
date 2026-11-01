#include <cflow/clock.h>
#include <cflow/executor.h>
#include <cflow/scheduler.h>
#include "scheduler_internal.h"
#include "timer_queue.h"
#include <thread.h>

#include <stdlib.h>
#include <string.h>

typedef struct worker_state {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    cmeta_thread_t timer_thread;
    cflow_clock clock;
    cflow_executor executor;
    cflow_executor_control executor_control;
    cflow_timer_queue timers;
    size_t ready_capacity;
    size_t timer_capacity;
    size_t dispatching;
    size_t accepted;
    size_t settled_before_executor;
    size_t peak_pending;
    size_t rejected_full;
    size_t rejected_closed;
    size_t cancelled_on_shutdown;
    bool stopping;
} worker_state;

static size_t worker_logical_pending_locked(worker_state *state) {
    cflow_executor_protocol_stats stats;
    size_t settled;

    if (!cflow_executor_control_get_stats(&state->executor_control, &stats))
        return 0u;
    if (stats.completed > SIZE_MAX - stats.cancelled)
        return 0u;
    settled = stats.completed + stats.cancelled;
    if (settled > SIZE_MAX - state->settled_before_executor)
        return 0u;
    settled += state->settled_before_executor;
    return settled < state->accepted ? state->accepted - settled : 0u;
}

static void worker_update_peak_locked(worker_state *state) {
    size_t pending = worker_logical_pending_locked(state);
    if (pending > state->peak_pending) state->peak_pending = pending;
}

static void worker_timer_main(void *user) {
    worker_state *state = (worker_state *)user;

    if (!state) return;
    cmeta_mutex_lock(&state->mutex);
    for (;;) {
        cflow_deadline deadline;
        cflow_instant now;
        cflow_timer_task task;
        cflow_executor_task descriptor;
        cflow_executor_post_status admitted;

        if (state->stopping) break;
        if (!cflow_timer_queue_next_deadline(&state->timers, &deadline)) {
            cmeta_cond_wait(&state->changed, &state->mutex);
            continue;
        }

        now = cflow_clock_now(&state->clock);
        if (deadline.ns > now.ns) {
            cflow_duration remaining = cflow_deadline_remaining(deadline, now);
            (void)cmeta_cond_timedwait(&state->changed, &state->mutex,
                                       remaining.ns);
            continue;
        }

        if (!cflow_timer_queue_take_ready(&state->timers, now, &task))
            continue;

        ++state->dispatching;
        cmeta_cond_broadcast(&state->changed);
        cmeta_mutex_unlock(&state->mutex);

        descriptor = cflow_timer_task_descriptor(&task);
        admitted = cflow_executor_control_post_task(
            &state->executor_control, &descriptor);
        if (admitted != CFLOW_EXECUTOR_POST_ACCEPTED)
            cflow_scheduler_settle_cancelled_task_internal(&descriptor);

        cmeta_mutex_lock(&state->mutex);
        --state->dispatching;
        if (admitted != CFLOW_EXECUTOR_POST_ACCEPTED) {
            ++state->settled_before_executor;
            ++state->cancelled_on_shutdown;
            ++state->rejected_closed;
        }
        cmeta_cond_broadcast(&state->changed);
    }
    cmeta_cond_broadcast(&state->changed);
    cmeta_mutex_unlock(&state->mutex);
}

static cflow_schedule_result worker_try_post_task_after(
    worker_state *state, uint64_t delay_ms,
    const cflow_executor_task *task) {
    cflow_instant now;
    cflow_deadline deadline;
    cflow_schedule_result result;
    bool immediate;

    if (!state || !task || !task->run)
        return (cflow_schedule_result){CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    cmeta_mutex_lock(&state->mutex);
    if (state->stopping) {
        ++state->rejected_closed;
        cmeta_mutex_unlock(&state->mutex);
        return (cflow_schedule_result){CFLOW_ADMISSION_CLOSED, 0u};
    }

    /* Keep the bounded timer admission/ID contract. With no older timer or
     * dispatcher ahead, zero-delay work can transfer straight to the existing
     * worker queue, avoiding a timer-thread round trip for every Run wake.
     * If that queue is full, the accepted timer retains progress exactly as
     * before; no caller waits for worker capacity. */
    immediate = delay_ms == 0u && state->dispatching == 0u &&
        cflow_timer_queue_pending(&state->timers) == 0u;
    now = cflow_clock_now(&state->clock);
    deadline = cflow_deadline_after(now, cflow_duration_from_ms(delay_ms));
    result = cflow_timer_queue_try_schedule_task(
        &state->timers, deadline, task);
    if (result.status == CFLOW_ADMISSION_ACCEPTED) {
        if (immediate && cflow_executor_try_post_task(&state->executor, task) ==
                         CFLOW_ADMISSION_ACCEPTED) {
            cflow_timer_task transferred;
            (void)cflow_timer_queue_take(&state->timers, result.task_id, &transferred);
        }
        ++state->accepted;
        worker_update_peak_locked(state);
        cmeta_cond_broadcast(&state->changed);
    } else if (result.status == CFLOW_ADMISSION_FULL) {
        ++state->rejected_full;
    }
    cmeta_mutex_unlock(&state->mutex);
    return result;
}

static cflow_schedule_result worker_try_post_after(void *self,
                                                   uint64_t delay_ms,
                                                   cflow_task_fn fn,
                                                   void *user) {
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return worker_try_post_task_after(
        (worker_state *)self, delay_ms, &task);
}

static cflow_task_id worker_post_after(void *self,
                                       uint64_t delay_ms,
                                       cflow_task_fn fn,
                                       void *user) {
    return worker_try_post_after(self, delay_ms, fn, user).task_id;
}

static bool worker_cancel(void *self, cflow_task_id id) {
    worker_state *state = (worker_state *)self;
    cflow_timer_task timer_task = {0};
    cflow_executor_task task;
    bool cancelled;

    if (!state || id == 0u) return false;
    cmeta_mutex_lock(&state->mutex);
    cancelled = !state->stopping &&
                cflow_timer_queue_take(&state->timers, id, &timer_task);
    if (cancelled) {
        ++state->settled_before_executor;
        cmeta_cond_broadcast(&state->changed);
    }
    cmeta_mutex_unlock(&state->mutex);
    if (cancelled) {
        task = cflow_timer_task_descriptor(&timer_task);
        cflow_scheduler_settle_cancelled_task_internal(&task);
    }
    return cancelled;
}

static bool worker_run_one(void *self) {
    (void)self;
    return false;
}

static size_t worker_run_ready(void *self) {
    (void)self;
    return 0u;
}

static size_t worker_advance(void *self, uint64_t ticks) {
    (void)self;
    (void)ticks;
    return 0u;
}

static size_t worker_run_until_idle(void *self, size_t max_steps) {
    (void)self;
    (void)max_steps;
    return 0u;
}

static bool worker_wait_idle(void *self) {
    worker_state *state = (worker_state *)self;

    if (!state) return false;
    for (;;) {
        bool stopping;
        bool delayed_idle;

        cmeta_mutex_lock(&state->mutex);
        while (!state->stopping &&
               (cflow_timer_queue_pending(&state->timers) != 0u ||
                state->dispatching != 0u)) {
            cmeta_cond_wait(&state->changed, &state->mutex);
        }
        stopping = state->stopping;
        delayed_idle = cflow_timer_queue_pending(&state->timers) == 0u &&
                       state->dispatching == 0u;
        cmeta_mutex_unlock(&state->mutex);

        if (stopping) return false;
        if (!delayed_idle || !cflow_executor_wait_idle(&state->executor))
            return false;

        cmeta_mutex_lock(&state->mutex);
        delayed_idle = !state->stopping &&
                       cflow_timer_queue_pending(&state->timers) == 0u &&
                       state->dispatching == 0u;
        cmeta_mutex_unlock(&state->mutex);
        if (delayed_idle && cflow_executor_pending(&state->executor) == 0u)
            return true;
    }
}

static uint64_t worker_now(void *self) {
    worker_state *state = (worker_state *)self;
    return state ? cflow_instant_to_ms(cflow_clock_now(&state->clock)) : 0u;
}

static size_t worker_pending(void *self) {
    worker_state *state = (worker_state *)self;
    size_t pending;

    if (!state) return 0u;
    cmeta_mutex_lock(&state->mutex);
    pending = worker_logical_pending_locked(state);
    cmeta_mutex_unlock(&state->mutex);
    return pending;
}

static bool worker_shutdown(void *self) {
    worker_state *state = (worker_state *)self;
    cflow_timer_task timer_task;
    bool join_timer;

    if (!state) return false;
    cmeta_mutex_lock(&state->mutex);
    if (!state->stopping) {
        state->stopping = true;
        cmeta_cond_broadcast(&state->changed);
    }
    while (cflow_timer_queue_take_any(&state->timers, &timer_task)) {
        const cflow_executor_task task =
            cflow_timer_task_descriptor(&timer_task);
        ++state->settled_before_executor;
        ++state->cancelled_on_shutdown;
        cmeta_mutex_unlock(&state->mutex);
        cflow_scheduler_settle_cancelled_task_internal(&task);
        cmeta_mutex_lock(&state->mutex);
    }
    join_timer = state->timer_thread != 0;
    cmeta_mutex_unlock(&state->mutex);

    if (!cflow_executor_shutdown(&state->executor)) return false;
    if (join_timer) {
        (void)cmeta_thread_join(&state->timer_thread);
        state->timer_thread = 0;
    }
    return true;
}

static bool worker_get_stats(void *self, cflow_scheduler_stats *out) {
    worker_state *state = (worker_state *)self;
    cflow_executor_stats executor_stats;

    if (!state || !out) return false;
    cmeta_mutex_lock(&state->mutex);
    if (!cflow_executor_get_stats(&state->executor, &executor_stats)) {
        cmeta_mutex_unlock(&state->mutex);
        return false;
    }
    *out = (cflow_scheduler_stats){
        .ready_capacity = state->ready_capacity,
        .timer_capacity = state->timer_capacity,
        .ready_pending = executor_stats.pending,
        .timer_pending = cflow_timer_queue_pending(&state->timers),
        .dispatching = state->dispatching,
        .peak_pending = state->peak_pending,
        .rejected_full = state->rejected_full,
        .rejected_closed = state->rejected_closed,
        .cancelled_on_shutdown = state->cancelled_on_shutdown
    };
    cmeta_mutex_unlock(&state->mutex);
    return true;
}

static void worker_destroy(void *self) {
    worker_state *state = (worker_state *)self;

    if (!state) return;
    (void)worker_shutdown(state);
    cflow_timer_queue_destroy(&state->timers);
    cflow_executor_destroy(&state->executor);
    cflow_clock_destroy(&state->clock);
    cmeta_cond_destroy(&state->changed);
    cmeta_mutex_destroy(&state->mutex);
    free(state);
}

CMETA_IMPLEMENTS(cflow_scheduler, worker_scheduler,
    CMETA_SCHED_CAP_DELAYED | CMETA_SCHED_CAP_CONCURRENT,
    .try_post_after = worker_try_post_after,
    .post_after = worker_post_after,
    .cancel = worker_cancel,
    .run_one = worker_run_one,
    .run_ready = worker_run_ready,
    .advance = worker_advance,
    .run_until_idle = worker_run_until_idle,
    .wait_idle = worker_wait_idle,
    .now = worker_now,
    .pending = worker_pending,
    .shutdown = worker_shutdown,
    .get_stats = worker_get_stats,
    .destroy = worker_destroy
);

bool cflow_scheduler_worker_init(cflow_scheduler *scheduler, size_t workers) {
    return cflow_scheduler_worker_init_with_capacity(
        scheduler, workers, CFLOW_EXECUTOR_DEFAULT_CAPACITY,
        CFLOW_TIMER_DEFAULT_CAPACITY);
}

bool cflow_scheduler_worker_init_with_capacity(cflow_scheduler *scheduler,
                                               size_t workers,
                                               size_t ready_capacity,
                                               size_t timer_capacity) {
    worker_state *state;

    if (!scheduler || scheduler->self || scheduler->vtable) return false;
    if (workers == 0u || ready_capacity == 0u || timer_capacity == 0u)
        return false;
    state = (worker_state *)calloc(1, sizeof(*state));
    if (!state) return false;

    cmeta_mutex_init(&state->mutex);
    cmeta_cond_init(&state->changed);
    if (!state->mutex || !state->changed ||
        !cflow_clock_system_init(&state->clock) ||
        !cflow_executor_worker_init_with_capacity(&state->executor, workers,
                                                  ready_capacity) ||
        !cflow_executor_as_control(&state->executor,
                                   &state->executor_control) ||
        !cflow_timer_queue_init_with_capacity(&state->timers,
                                              timer_capacity) ||
        cmeta_thread_create(&state->timer_thread, worker_timer_main, state) != 0) {
        if (state->timer_thread) (void)cmeta_thread_join(&state->timer_thread);
        cflow_timer_queue_destroy(&state->timers);
        if (cflow_executor_valid(&state->executor))
            cflow_executor_destroy(&state->executor);
        if (cflow_clock_valid(&state->clock)) cflow_clock_destroy(&state->clock);
        cmeta_cond_destroy(&state->changed);
        cmeta_mutex_destroy(&state->mutex);
        free(state);
        return false;
    }

    state->ready_capacity = ready_capacity;
    state->timer_capacity = timer_capacity;
    *scheduler = worker_scheduler_as_cflow_scheduler(state);
    return true;
}

bool cflow_scheduler_worker_try_post_task_after_internal(
    cflow_scheduler *scheduler, uint64_t delay_ms,
    const cflow_executor_task *task, cflow_schedule_result *out) {
    if (!scheduler || scheduler->vtable != &worker_scheduler_vtable ||
        !task || !task->run || !out)
        return false;
    *out = worker_try_post_task_after(
        (worker_state *)scheduler->self, delay_ms, task);
    return true;
}
