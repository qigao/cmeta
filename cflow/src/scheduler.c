#include <cflow/clock.h>
#include <cflow/executor.h>
#include <cflow/scheduler.h>
#include "scheduler_internal.h"
#include "timer_queue.h"
#include <thread.h>

#include <stdlib.h>
#include <string.h>

static const cmeta_type_traits cflow_scheduler_trivial_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY |
             CMETA_TRAIT_TRIVIAL_DESTROY
};

const cmeta_type_desc cflow_type_scheduler_stats = {
    .name = "cflow_scheduler_stats",
    .size = sizeof(cflow_scheduler_stats),
    .align = _Alignof(cflow_scheduler_stats),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &cflow_scheduler_trivial_traits,
    .identity = NULL
};

const cmeta_type_desc cflow_type_scheduler_stats_ptr = {
    .name = "cflow_scheduler_stats *",
    .size = sizeof(cflow_scheduler_stats *),
    .align = _Alignof(cflow_scheduler_stats *),
    .kind = CMETA_T_POINTER,
    .pointee = &cflow_type_scheduler_stats,
    .traits = NULL,
    .identity = NULL
};

typedef struct cflow_test_loop_state {
    cmeta_mutex_t mutex;
    cflow_clock clock;
    cflow_executor executor;
    cflow_timer_queue timers;
    size_t ready_capacity;
    size_t timer_capacity;
    size_t peak_pending;
    size_t rejected_full;
    size_t rejected_closed;
    size_t cancelled_on_shutdown;
    bool stopping;
} cflow_test_loop_state;

typedef struct cflow_inline_scheduler_state {
    cflow_task_id next_id;
    size_t dispatching;
    size_t peak_dispatching;
    size_t rejected_closed;
    bool closed;
} cflow_inline_scheduler_state;

typedef struct cflow_manual_scheduler_state {
    cflow_executor executor;
    cflow_task_id next_id;
} cflow_manual_scheduler_state;

static cflow_schedule_result inline_try_post_task_after(
    cflow_inline_scheduler_state *state, uint64_t delay_ticks,
    const cflow_executor_task *task) {
    cflow_task_id task_id;

    if (state == NULL || task == NULL || task->run == NULL ||
        delay_ticks != 0u)
        return (cflow_schedule_result){
            CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    if (state->closed) {
        ++state->rejected_closed;
        return (cflow_schedule_result){CFLOW_ADMISSION_CLOSED, 0u};
    }
    if (state->next_id == UINT64_MAX)
        return (cflow_schedule_result){
            CFLOW_ADMISSION_ALLOCATION_FAILED, 0u};

    task_id = ++state->next_id;
    ++state->dispatching;
    if (state->dispatching > state->peak_dispatching)
        state->peak_dispatching = state->dispatching;
    task->run(task->user);
    if (task->finalize != NULL)
        task->finalize(task->user);
    --state->dispatching;
    return (cflow_schedule_result){CFLOW_ADMISSION_ACCEPTED, task_id};
}

static cflow_schedule_result inline_try_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return inline_try_post_task_after(
        (cflow_inline_scheduler_state *)self, delay_ticks, &task);
}

static cflow_task_id inline_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    return inline_try_post_after(self, delay_ticks, fn, user).task_id;
}

static bool inline_cancel(void *self, cflow_task_id task_id) {
    (void)self;
    (void)task_id;
    return false;
}

static bool inline_run_one(void *self) {
    (void)self;
    return false;
}

static size_t inline_run_ready(void *self) {
    (void)self;
    return 0u;
}

static size_t inline_advance(void *self, uint64_t ticks) {
    (void)self;
    (void)ticks;
    return 0u;
}

static size_t inline_run_until_idle(void *self, size_t max_steps) {
    (void)self;
    (void)max_steps;
    return 0u;
}

static bool inline_wait_idle(void *self) {
    const cflow_inline_scheduler_state *state =
        (const cflow_inline_scheduler_state *)self;
    return state != NULL && state->dispatching == 0u;
}

static uint64_t inline_now(void *self) {
    (void)self;
    return 0u;
}

static size_t inline_pending(void *self) {
    (void)self;
    return 0u;
}

static bool inline_shutdown(void *self) {
    cflow_inline_scheduler_state *state =
        (cflow_inline_scheduler_state *)self;
    if (state == NULL)
        return false;
    state->closed = true;
    return true;
}

static bool inline_get_stats(void *self, cflow_scheduler_stats *out) {
    const cflow_inline_scheduler_state *state =
        (const cflow_inline_scheduler_state *)self;
    if (state == NULL || out == NULL)
        return false;
    *out = (cflow_scheduler_stats){
        .dispatching = state->dispatching,
        .peak_pending = state->peak_dispatching,
        .rejected_closed = state->rejected_closed
    };
    return true;
}

static void inline_destroy(void *self) {
    free(self);
}

CMETA_IMPLEMENTS(cflow_scheduler, inline_scheduler,
    CMETA_SCHED_CAP_CALLER_DRIVEN_ZERO_DELAY,
    .try_post_after = inline_try_post_after,
    .post_after = inline_post_after,
    .cancel = inline_cancel,
    .run_one = inline_run_one,
    .run_ready = inline_run_ready,
    .advance = inline_advance,
    .run_until_idle = inline_run_until_idle,
    .wait_idle = inline_wait_idle,
    .now = inline_now,
    .pending = inline_pending,
    .shutdown = inline_shutdown,
    .get_stats = inline_get_stats,
    .destroy = inline_destroy
);

static cflow_schedule_result manual_scheduler_try_post_task_after(
    cflow_manual_scheduler_state *state, uint64_t delay_ticks,
    const cflow_executor_task *task) {
    cflow_admission_status admitted;
    cflow_task_id task_id;

    if (state == NULL || task == NULL || task->run == NULL ||
        delay_ticks != 0u)
        return (cflow_schedule_result){
            CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    if (state->next_id == UINT64_MAX)
        return (cflow_schedule_result){
            CFLOW_ADMISSION_ALLOCATION_FAILED, 0u};
    admitted = cflow_executor_try_post_task(&state->executor, task);
    if (admitted != CFLOW_ADMISSION_ACCEPTED)
        return (cflow_schedule_result){admitted, 0u};
    task_id = ++state->next_id;
    return (cflow_schedule_result){CFLOW_ADMISSION_ACCEPTED, task_id};
}

static cflow_schedule_result manual_scheduler_try_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return manual_scheduler_try_post_task_after(
        (cflow_manual_scheduler_state *)self, delay_ticks, &task);
}

static cflow_task_id manual_scheduler_post_after(
    void *self, uint64_t delay_ticks, cflow_task_fn fn, void *user) {
    return manual_scheduler_try_post_after(
        self, delay_ticks, fn, user).task_id;
}

static bool manual_scheduler_cancel(void *self, cflow_task_id task_id) {
    (void)self;
    (void)task_id;
    return false;
}

static bool manual_scheduler_run_one(void *self) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    return state != NULL && cflow_executor_run_one(&state->executor);
}

static size_t manual_scheduler_run_ready(void *self) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    return state != NULL ? cflow_executor_run_ready(&state->executor) : 0u;
}

static size_t manual_scheduler_advance(void *self, uint64_t ticks) {
    (void)self;
    (void)ticks;
    return 0u;
}

static size_t manual_scheduler_run_until_idle(
    void *self, size_t max_steps) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    size_t count = 0u;

    if (state == NULL)
        return 0u;
    while ((max_steps == 0u || count < max_steps) &&
           cflow_executor_run_one(&state->executor))
        ++count;
    return count;
}

static bool manual_scheduler_wait_idle(void *self) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    return state != NULL && cflow_executor_wait_idle(&state->executor);
}

static uint64_t manual_scheduler_now(void *self) {
    (void)self;
    return 0u;
}

static size_t manual_scheduler_pending(void *self) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    return state != NULL ? cflow_executor_pending(&state->executor) : 0u;
}

static bool manual_scheduler_shutdown(void *self) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    return state != NULL && cflow_executor_shutdown(&state->executor);
}

static bool manual_scheduler_get_stats(
    void *self, cflow_scheduler_stats *out) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    cflow_executor_stats executor_stats = {0};

    if (state == NULL || out == NULL ||
        !cflow_executor_get_stats(&state->executor, &executor_stats))
        return false;
    *out = (cflow_scheduler_stats){
        .ready_capacity = executor_stats.capacity,
        .ready_pending = executor_stats.pending,
        .peak_pending = executor_stats.peak_pending,
        .rejected_full = executor_stats.rejected_full,
        .rejected_closed = executor_stats.rejected_closed
    };
    return true;
}

static void manual_scheduler_destroy(void *self) {
    cflow_manual_scheduler_state *state =
        (cflow_manual_scheduler_state *)self;
    if (state == NULL)
        return;
    cflow_executor_destroy(&state->executor);
    free(state);
}

CMETA_IMPLEMENTS(cflow_scheduler, manual_scheduler,
    CMETA_SCHED_CAP_CALLER_DRIVEN_ZERO_DELAY,
    .try_post_after = manual_scheduler_try_post_after,
    .post_after = manual_scheduler_post_after,
    .cancel = manual_scheduler_cancel,
    .run_one = manual_scheduler_run_one,
    .run_ready = manual_scheduler_run_ready,
    .advance = manual_scheduler_advance,
    .run_until_idle = manual_scheduler_run_until_idle,
    .wait_idle = manual_scheduler_wait_idle,
    .now = manual_scheduler_now,
    .pending = manual_scheduler_pending,
    .shutdown = manual_scheduler_shutdown,
    .get_stats = manual_scheduler_get_stats,
    .destroy = manual_scheduler_destroy
);

static void test_update_peak_locked(cflow_test_loop_state *state) {
    size_t pending = cflow_timer_queue_pending(&state->timers);
    if (pending > state->peak_pending) state->peak_pending = pending;
}

static bool test_take_and_run_one(cflow_test_loop_state *state,
                                  bool advance_to_next) {
    cflow_timer_task task;
    cflow_executor_task descriptor;
    cflow_instant now;
    cflow_admission_status admitted;

    if (!state) return false;
    cmeta_mutex_lock(&state->mutex);
    now = cflow_clock_now(&state->clock);
    if (advance_to_next) {
        cflow_deadline deadline;
        if (!cflow_timer_queue_next_deadline(&state->timers, &deadline)) {
            cmeta_mutex_unlock(&state->mutex);
            return false;
        }
        if (deadline.ns > now.ns &&
            !cflow_clock_advance(&state->clock,
                                 cflow_deadline_remaining(deadline, now))) {
            cmeta_mutex_unlock(&state->mutex);
            return false;
        }
        now = cflow_clock_now(&state->clock);
    }
    if (!cflow_timer_queue_take_ready(&state->timers, now, &task)) {
        cmeta_mutex_unlock(&state->mutex);
        return false;
    }
    cmeta_mutex_unlock(&state->mutex);
    descriptor = cflow_timer_task_descriptor(&task);
    admitted = cflow_executor_try_post_task(&state->executor, &descriptor);
    if (admitted != CFLOW_ADMISSION_ACCEPTED) {
        cflow_scheduler_settle_cancelled_task_internal(&descriptor);
        return false;
    }

    /* The task can re-enter the scheduler, so callbacks run without its lock. */
    return cflow_executor_run_one(&state->executor);
}

static cflow_schedule_result test_try_post_task_after(
    cflow_test_loop_state *state, uint64_t delay_ms,
    const cflow_executor_task *task) {
    cflow_instant now;
    cflow_deadline deadline;
    cflow_schedule_result result;

    if (!state || !task || !task->run)
        return (cflow_schedule_result){CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};
    cmeta_mutex_lock(&state->mutex);
    if (state->stopping) {
        ++state->rejected_closed;
        cmeta_mutex_unlock(&state->mutex);
        return (cflow_schedule_result){CFLOW_ADMISSION_CLOSED, 0u};
    }
    now = cflow_clock_now(&state->clock);
    deadline = cflow_deadline_after(now, cflow_duration_from_ms(delay_ms));
    result = cflow_timer_queue_try_schedule_task(
        &state->timers, deadline, task);
    if (result.status == CFLOW_ADMISSION_ACCEPTED)
        test_update_peak_locked(state);
    else if (result.status == CFLOW_ADMISSION_FULL)
        ++state->rejected_full;
    cmeta_mutex_unlock(&state->mutex);
    return result;
}

static cflow_schedule_result test_try_post_after(void *self,
                                                 uint64_t delay_ms,
                                                 cflow_task_fn fn,
                                                 void *user) {
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return test_try_post_task_after(
        (cflow_test_loop_state *)self, delay_ms, &task);
}

static cflow_task_id test_post_after(void *self,
                                     uint64_t delay_ms,
                                     cflow_task_fn fn,
                                     void *user) {
    return test_try_post_after(self, delay_ms, fn, user).task_id;
}

static bool test_cancel(void *self, cflow_task_id id) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    cflow_timer_task timer_task = {0};
    cflow_executor_task task;
    bool cancelled;

    if (!state || id == 0u) return false;
    cmeta_mutex_lock(&state->mutex);
    cancelled = !state->stopping &&
                cflow_timer_queue_take(&state->timers, id, &timer_task);
    cmeta_mutex_unlock(&state->mutex);
    if (cancelled) {
        task = cflow_timer_task_descriptor(&timer_task);
        cflow_scheduler_settle_cancelled_task_internal(&task);
    }
    return cancelled;
}

static bool test_run_one(void *self) {
    return test_take_and_run_one((cflow_test_loop_state *)self, false);
}

static size_t test_run_ready(void *self) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    size_t count = 0u;
    while (test_take_and_run_one(state, false)) ++count;
    return count;
}

static size_t test_advance(void *self, uint64_t ticks_ms) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    bool advanced;

    if (!state) return 0u;
    cmeta_mutex_lock(&state->mutex);
    advanced = cflow_clock_advance(&state->clock,
                                   cflow_duration_from_ms(ticks_ms));
    cmeta_mutex_unlock(&state->mutex);
    if (!advanced) return 0u;
    return test_run_ready(state);
}

static size_t test_run_until_idle(void *self, size_t max_steps) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    size_t ran = 0u;

    if (!state) return 0u;
    while (max_steps == 0u || ran < max_steps) {
        if (!test_take_and_run_one(state, true)) break;
        ++ran;
    }
    return ran;
}

static bool test_wait_idle(void *self) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    bool timers_idle;

    if (!state) return false;
    (void)test_run_until_idle(state, 0u);
    cmeta_mutex_lock(&state->mutex);
    timers_idle = cflow_timer_queue_pending(&state->timers) == 0u;
    cmeta_mutex_unlock(&state->mutex);
    return timers_idle && cflow_executor_wait_idle(&state->executor);
}

static uint64_t test_now(void *self) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    uint64_t now;

    if (!state) return 0u;
    cmeta_mutex_lock(&state->mutex);
    now = cflow_instant_to_ms(cflow_clock_now(&state->clock));
    cmeta_mutex_unlock(&state->mutex);
    return now;
}

static size_t test_pending(void *self) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    size_t pending;

    if (!state) return 0u;
    cmeta_mutex_lock(&state->mutex);
    pending = cflow_timer_queue_pending(&state->timers) +
              cflow_executor_pending(&state->executor);
    cmeta_mutex_unlock(&state->mutex);
    return pending;
}

static bool test_shutdown(void *self) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    cflow_timer_task timer_task;
    bool should_shutdown = false;

    if (!state) return false;
    cmeta_mutex_lock(&state->mutex);
    if (!state->stopping) {
        state->stopping = true;
        should_shutdown = true;
    }
    while (cflow_timer_queue_take_any(&state->timers, &timer_task)) {
        const cflow_executor_task task =
            cflow_timer_task_descriptor(&timer_task);
        ++state->cancelled_on_shutdown;
        cmeta_mutex_unlock(&state->mutex);
        cflow_scheduler_settle_cancelled_task_internal(&task);
        cmeta_mutex_lock(&state->mutex);
    }
    cmeta_mutex_unlock(&state->mutex);
    return !should_shutdown || cflow_executor_shutdown(&state->executor);
}

static bool test_get_stats(void *self, cflow_scheduler_stats *out) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    if (!state || !out) return false;
    cmeta_mutex_lock(&state->mutex);
    *out = (cflow_scheduler_stats){
        .ready_capacity = state->ready_capacity,
        .timer_capacity = state->timer_capacity,
        .ready_pending = cflow_executor_pending(&state->executor),
        .timer_pending = cflow_timer_queue_pending(&state->timers),
        .dispatching = 0u,
        .peak_pending = state->peak_pending,
        .rejected_full = state->rejected_full,
        .rejected_closed = state->rejected_closed,
        .cancelled_on_shutdown = state->cancelled_on_shutdown
    };
    cmeta_mutex_unlock(&state->mutex);
    return true;
}

static void test_destroy(void *self) {
    cflow_test_loop_state *state = (cflow_test_loop_state *)self;
    if (!state) return;
    (void)test_shutdown(state);
    cflow_timer_queue_destroy(&state->timers);
    cflow_executor_destroy(&state->executor);
    cflow_clock_destroy(&state->clock);
    cmeta_mutex_destroy(&state->mutex);
    free(state);
}

CMETA_IMPLEMENTS(cflow_scheduler, test_loop,
    CMETA_SCHED_CAP_DELAYED | CMETA_SCHED_CAP_MANUAL_CLOCK,
    .try_post_after = test_try_post_after,
    .post_after = test_post_after,
    .cancel = test_cancel,
    .run_one = test_run_one,
    .run_ready = test_run_ready,
    .advance = test_advance,
    .run_until_idle = test_run_until_idle,
    .wait_idle = test_wait_idle,
    .now = test_now,
    .pending = test_pending,
    .shutdown = test_shutdown,
    .get_stats = test_get_stats,
    .destroy = test_destroy
);

bool cflow_scheduler_test_init(cflow_scheduler *scheduler) {
    return cflow_scheduler_test_init_with_capacity(
        scheduler, CFLOW_EXECUTOR_DEFAULT_CAPACITY,
        CFLOW_TIMER_DEFAULT_CAPACITY);
}

bool cflow_scheduler_inline_init(cflow_scheduler *scheduler) {
    cflow_inline_scheduler_state *state;

    if (scheduler == NULL || scheduler->self != NULL ||
        scheduler->vtable != NULL)
        return false;
    state = (cflow_inline_scheduler_state *)calloc(1u, sizeof(*state));
    if (state == NULL)
        return false;
    *scheduler = inline_scheduler_as_cflow_scheduler(state);
    return true;
}

bool cflow_scheduler_manual_init(cflow_scheduler *scheduler) {
    return cflow_scheduler_manual_init_with_capacity(
        scheduler, CFLOW_EXECUTOR_DEFAULT_CAPACITY);
}

bool cflow_scheduler_manual_init_with_capacity(
    cflow_scheduler *scheduler, size_t ready_capacity) {
    cflow_manual_scheduler_state *state;

    if (scheduler == NULL || scheduler->self != NULL ||
        scheduler->vtable != NULL || ready_capacity == 0u)
        return false;
    state = (cflow_manual_scheduler_state *)calloc(1u, sizeof(*state));
    if (state == NULL)
        return false;
    if (!cflow_executor_manual_init_with_capacity(
            &state->executor, ready_capacity)) {
        free(state);
        return false;
    }
    *scheduler = manual_scheduler_as_cflow_scheduler(state);
    return true;
}

bool cflow_scheduler_test_init_with_capacity(cflow_scheduler *scheduler,
                                             size_t ready_capacity,
                                             size_t timer_capacity) {
    cflow_test_loop_state *state;

    if (!scheduler || scheduler->self || scheduler->vtable) return false;
    if (ready_capacity == 0u || timer_capacity == 0u) return false;
    state = (cflow_test_loop_state *)calloc(1, sizeof(*state));
    if (!state) return false;

    cmeta_mutex_init(&state->mutex);
    if (!state->mutex ||
        !cflow_clock_virtual_init(&state->clock, (cflow_instant){0u}) ||
        !cflow_executor_manual_init_with_capacity(&state->executor,
                                                  ready_capacity) ||
        !cflow_timer_queue_init_with_capacity(&state->timers,
                                              timer_capacity)) {
        if (cflow_clock_valid(&state->clock)) cflow_clock_destroy(&state->clock);
        if (cflow_executor_valid(&state->executor))
            cflow_executor_destroy(&state->executor);
        cflow_timer_queue_destroy(&state->timers);
        cmeta_mutex_destroy(&state->mutex);
        free(state);
        return false;
    }

    state->ready_capacity = ready_capacity;
    state->timer_capacity = timer_capacity;
    *scheduler = test_loop_as_cflow_scheduler(state);
    return true;
}

cflow_task_id cflow_scheduler_post(cflow_scheduler *scheduler,
                                   cflow_task_fn fn,
                                   void *user) {
    return cflow_scheduler_post_after(scheduler, 0u, fn, user);
}

const char *cflow_scheduler_name(const cflow_scheduler *scheduler) {
    return cflow_scheduler_implementation(scheduler);
}

bool cflow_scheduler_try_post_task_after_internal(
    cflow_scheduler *scheduler, uint64_t delay_ms,
    const cflow_executor_task *task, cflow_schedule_result *out) {
    if (!scheduler || !task || !task->run || !out) return false;
    if (scheduler->vtable == &manual_scheduler_vtable) {
        *out = manual_scheduler_try_post_task_after(
            (cflow_manual_scheduler_state *)scheduler->self,
            delay_ms, task);
        return true;
    }
    if (scheduler->vtable == &inline_scheduler_vtable) {
        *out = inline_try_post_task_after(
            (cflow_inline_scheduler_state *)scheduler->self,
            delay_ms, task);
        return true;
    }
    if (scheduler->vtable == &test_loop_vtable) {
        *out = test_try_post_task_after(
            (cflow_test_loop_state *)scheduler->self, delay_ms, task);
        return true;
    }
    if (cflow_scheduler_owner_try_post_task_after_internal(
            scheduler, delay_ms, task, out))
        return true;
    return cflow_scheduler_worker_try_post_task_after_internal(
        scheduler, delay_ms, task, out);
}

bool cflow_scheduler_is_manual_internal(
    const cflow_scheduler *scheduler) {
    return scheduler != NULL && scheduler->self != NULL &&
           scheduler->vtable == &manual_scheduler_vtable;
}
