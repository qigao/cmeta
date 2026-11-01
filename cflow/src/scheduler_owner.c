#include <cflow/scheduler.h>
#include "scheduler_internal.h"
#include "executor_internal.h"

#include <thread.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Host-owned, concurrent-admission Scheduler over one borrowed Owner Executor.
 * Scheduler does NOT create another queue, executor, worker, clock, or I/O loop.
 * Each accepted task holds a preallocated slot until the underlying Executor
 * runs (or cancels) its one descriptor. A successful synchronous cancel
 * removes the user's borrowed callback ownership, leaving a bounded tombstone
 * that the actual owner reclaims at the queued descriptor's terminal.
 */
typedef enum owner_schedule_phase {
    OWNER_SCHEDULE_FREE = 0,
    OWNER_SCHEDULE_PENDING,
    OWNER_SCHEDULE_RUNNING,
    OWNER_SCHEDULE_CANCEL_SETTLING,
    OWNER_SCHEDULE_CANCELLED
} owner_schedule_phase;

typedef struct owner_schedule_state owner_schedule_state;

typedef struct owner_schedule_slot {
    owner_schedule_state *owner;
    cflow_executor_task task;
    cflow_task_id id;
    size_t index;
    owner_schedule_phase phase;
    bool owner_consumed; /* Executor terminal already observed during cancel. */
} owner_schedule_slot;

struct owner_schedule_state {
    cmeta_mutex_t lock;
    cflow_executor *executor; /* Borrowed; outlives this Scheduler. */
    owner_schedule_slot *slots;
    size_t *free_indices;
    size_t capacity;
    size_t free_count;
    size_t pending;     /* Includes cancellation tombstones. */
    size_t settling;    /* Synchronous cancel/finalize still executing. */
    size_t dispatching; /* Currently executing user callback on owner. */
    size_t peak_pending;
    size_t rejected_full;
    size_t rejected_closed;
    cflow_task_id next_id;
    bool stopping;
};

static bool scheduler_on_owner(const owner_schedule_state *s) {
    return s && cflow_executor_owner_is_thread_internal(s->executor);
}

static void slot_return_locked(owner_schedule_slot *slot) {
    owner_schedule_state *s = slot->owner;
    assert(slot->phase != OWNER_SCHEDULE_FREE &&
           s->free_count < s->capacity && s->pending > 0u);
    slot->phase = OWNER_SCHEDULE_FREE;
    slot->owner_consumed = false;
    slot->id = 0u;
    slot->task = (cflow_executor_task){0};
    s->free_indices[s->free_count++] = slot->index;
    --s->pending;
}

/* The descriptor is executed/cancelled only by the borrowed Executor owner.
 * User callbacks run without the Scheduler lock to permit reentry. */
static void owner_slot_settle(void *user, bool cancelled_by_executor) {
    owner_schedule_slot *slot = (owner_schedule_slot *)user;
    owner_schedule_state *s;
    cflow_executor_task task = {0};
    bool have_task = false;

    if (!slot || !slot->owner) abort();
    s = slot->owner;
    cmeta_mutex_lock(&s->lock);
    if (slot->phase == OWNER_SCHEDULE_PENDING) {
        task = slot->task;
        slot->task = (cflow_executor_task){0};
        slot->phase = OWNER_SCHEDULE_RUNNING;
        have_task = true;
        if (cancelled_by_executor) ++s->settling;
        else ++s->dispatching;
    } else if (slot->phase == OWNER_SCHEDULE_CANCEL_SETTLING) {
        /* A foreign cancel/finalize still owns this slot's task lease.
         * Mark the Executor side terminal, but do not recycle the record
         * until that caller finishes its synchronous callbacks. */
        slot->owner_consumed = true;
        cmeta_mutex_unlock(&s->lock);
        return;
    } else if (slot->phase == OWNER_SCHEDULE_CANCELLED) {
        /* Cancel/finalize already returned; this Executor terminal completes
         * the other half of the slot's bounded obligation. */
        slot_return_locked(slot);
        cmeta_mutex_unlock(&s->lock);
        return;
    } else {
        cmeta_mutex_unlock(&s->lock);
        abort(); /* Duplicate settlement or unexpected slot reuse. */
    }
    cmeta_mutex_unlock(&s->lock);

    if (have_task) {
        if (cancelled_by_executor) {
            if (task.cancel) task.cancel(task.user);
        } else {
            task.run(task.user);
        }
        if (task.finalize) task.finalize(task.user);
    }

    cmeta_mutex_lock(&s->lock);
    if (have_task) {
        if (cancelled_by_executor) --s->settling;
        else --s->dispatching;
    }
    slot_return_locked(slot);
    cmeta_mutex_unlock(&s->lock);
}

static void owner_slot_run(void *user) {
    owner_slot_settle(user, false);
}

static void owner_slot_cancel(void *user) {
    owner_slot_settle(user, true);
}

static cflow_schedule_result owner_try_post_task_after(
    owner_schedule_state *s, uint64_t delay_ticks,
    const cflow_executor_task *task) {
    size_t index;
    owner_schedule_slot *slot;
    const cflow_executor_task descriptor = {
        owner_slot_run, owner_slot_cancel, NULL, NULL
    };
    cflow_executor_task posted = descriptor;
    cflow_admission_status admitted;
    cflow_task_id id;

    if (!s || !task || !task->run || delay_ticks != 0u)
        return (cflow_schedule_result){CFLOW_ADMISSION_INVALID_ARGUMENT, 0u};

    cmeta_mutex_lock(&s->lock);
    if (s->stopping) {
        ++s->rejected_closed;
        cmeta_mutex_unlock(&s->lock);
        return (cflow_schedule_result){CFLOW_ADMISSION_CLOSED, 0u};
    }
    if (s->next_id == UINT64_MAX) {
        cmeta_mutex_unlock(&s->lock);
        return (cflow_schedule_result){CFLOW_ADMISSION_ALLOCATION_FAILED, 0u};
    }
    if (s->free_count == 0u) {
        ++s->rejected_full;
        cmeta_mutex_unlock(&s->lock);
        return (cflow_schedule_result){CFLOW_ADMISSION_FULL, 0u};
    }

    index = s->free_indices[--s->free_count];
    slot = &s->slots[index];
    assert(slot->phase == OWNER_SCHEDULE_FREE);
    id = s->next_id + 1u;
    slot->task = *task;
    slot->id = id;
    slot->phase = OWNER_SCHEDULE_PENDING;
    slot->owner_consumed = false;
    posted.user = slot;

    /* The Executor owns the ONLY task queue. The Scheduler lock serializes
     * cancellation/id publication with admission. Its wake callback must
     * only signal, not reenter Scheduler/Executor driving. */
    admitted = cflow_executor_try_post_task(s->executor, &posted);
    if (admitted != CFLOW_ADMISSION_ACCEPTED) {
        slot->phase = OWNER_SCHEDULE_FREE;
        slot->id = 0u;
        slot->task = (cflow_executor_task){0};
        s->free_indices[s->free_count++] = index;
        if (admitted == CFLOW_ADMISSION_FULL) ++s->rejected_full;
        if (admitted == CFLOW_ADMISSION_CLOSED) ++s->rejected_closed;
        cmeta_mutex_unlock(&s->lock);
        return (cflow_schedule_result){admitted, 0u};
    }

    s->next_id = id;
    ++s->pending;
    if (s->pending > s->peak_pending) s->peak_pending = s->pending;
    cmeta_mutex_unlock(&s->lock);
    return (cflow_schedule_result){CFLOW_ADMISSION_ACCEPTED, id};
}

static cflow_schedule_result owner_try_post_after(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    const cflow_executor_task task = {fn, NULL, NULL, user};
    return owner_try_post_task_after((owner_schedule_state *)self,
                                     delay, &task);
}

static cflow_task_id owner_post_after(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    return owner_try_post_after(self, delay, fn, user).task_id;
}

/* A true cancel is synchronous: fn/user are no longer borrowed after return.
 * The task descriptor is cleared under the lock, then cancel/finalize run
 * outside the lock on the cancelling thread. The owner later consumes the
 * tombstone and returns its one bounded slot. */
static bool owner_cancel(void *self, cflow_task_id id) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    cflow_executor_task task = {0};
    owner_schedule_slot *cancelled_slot = NULL;
    if (!s || id == 0u) return false;

    cmeta_mutex_lock(&s->lock);
    for (size_t i = 0u; i < s->capacity; ++i) {
        owner_schedule_slot *slot = &s->slots[i];
        if (slot->phase != OWNER_SCHEDULE_PENDING || slot->id != id)
            continue;
        task = slot->task;
        slot->task = (cflow_executor_task){0};
        slot->phase = OWNER_SCHEDULE_CANCEL_SETTLING;
        ++s->settling;
        cancelled_slot = slot;
        break;
    }
    cmeta_mutex_unlock(&s->lock);
    if (!cancelled_slot) return false;

    if (task.cancel) task.cancel(task.user);
    if (task.finalize) task.finalize(task.user);

    cmeta_mutex_lock(&s->lock);
    --s->settling;
    if (cancelled_slot->phase != OWNER_SCHEDULE_CANCEL_SETTLING) {
        cmeta_mutex_unlock(&s->lock);
        abort(); /* No slot recycling while cancel/finalize is in flight. */
    }
    if (cancelled_slot->owner_consumed) {
        slot_return_locked(cancelled_slot);
    } else {
        cancelled_slot->phase = OWNER_SCHEDULE_CANCELLED;
    }
    cmeta_mutex_unlock(&s->lock);
    return true;
}

static bool owner_run_one(void *self) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    return scheduler_on_owner(s) && cflow_executor_run_one(s->executor);
}

static size_t owner_run_ready(void *self) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    return scheduler_on_owner(s) ? cflow_executor_run_ready(s->executor) : 0u;
}

static size_t owner_run_until_idle(void *self, size_t max_steps) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    size_t ran = 0u;
    if (!scheduler_on_owner(s)) return 0u;
    while ((max_steps == 0u || ran < max_steps) &&
           cflow_executor_run_one(s->executor))
        ++ran;
    return ran;
}

static size_t owner_advance(void *self, uint64_t ticks) {
    (void)self;
    (void)ticks;
    return 0u;
}

static bool owner_wait_idle(void *self) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    bool idle;
    if (!s) return false;
    cmeta_mutex_lock(&s->lock);
    idle = s->pending == 0u && s->settling == 0u;
    cmeta_mutex_unlock(&s->lock);
    return idle;
}

static uint64_t owner_now(void *self) {
    (void)self;
    return 0u; /* zero-delay schedule: no independent timer/clock. */
}

static size_t owner_pending(void *self) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    size_t pending;
    if (!s) return 0u;
    cmeta_mutex_lock(&s->lock);
    pending = s->pending;
    cmeta_mutex_unlock(&s->lock);
    return pending;
}

static bool owner_shutdown(void *self) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    if (!s) return false;
    cmeta_mutex_lock(&s->lock);
    s->stopping = true;
    cmeta_mutex_unlock(&s->lock);
    /* The Executor is borrowed/shared with Machine and must remain open.
     * Accepted work is still driven by its owner. */
    return true;
}

static bool owner_get_stats(void *self, cflow_scheduler_stats *out) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    if (!s || !out) return false;
    cmeta_mutex_lock(&s->lock);
    *out = (cflow_scheduler_stats){
        .ready_capacity = s->capacity,
        .ready_pending = s->pending,
        .dispatching = s->dispatching,
        .peak_pending = s->peak_pending,
        .rejected_full = s->rejected_full,
        .rejected_closed = s->rejected_closed
    };
    cmeta_mutex_unlock(&s->lock);
    return true;
}

static void owner_destroy(void *self) {
    owner_schedule_state *s = (owner_schedule_state *)self;
    if (!s) return;
    if (!scheduler_on_owner(s) ||
        cflow_executor_is_current_internal(s->executor)) {
        assert(!"owner-bound Scheduler destroy requires non-callback owner");
        abort();
    }
    (void)owner_shutdown(s);
    /* The caller has already quiesced all external producers, cancellation
     * callbacks and subscriptions. Never free slots still queued in Executor. */
    while (owner_pending(s) != 0u) {
        if (!cflow_executor_run_one(s->executor)) {
            assert(!"owner Scheduler destroy has unprogressable work");
            abort();
        }
    }
    if (!owner_wait_idle(s)) {
        assert(!"owner Scheduler destroy raced an external cancellation");
        abort();
    }
    cmeta_mutex_destroy(&s->lock);
    free(s->free_indices);
    free(s->slots);
    free(s);
}

CMETA_IMPLEMENTS(cflow_scheduler, owner_scheduler,
    CMETA_SCHED_CAP_CONCURRENT | CMETA_SCHED_CAP_CALLER_DRIVEN_ZERO_DELAY,
    .try_post_after = owner_try_post_after,
    .post_after = owner_post_after,
    .cancel = owner_cancel,
    .run_one = owner_run_one,
    .run_ready = owner_run_ready,
    .advance = owner_advance,
    .run_until_idle = owner_run_until_idle,
    .wait_idle = owner_wait_idle,
    .now = owner_now,
    .pending = owner_pending,
    .shutdown = owner_shutdown,
    .get_stats = owner_get_stats,
    .destroy = owner_destroy
);

bool cflow_scheduler_owner_try_post_task_after_internal(
    cflow_scheduler *scheduler, uint64_t delay,
    const cflow_executor_task *task, cflow_schedule_result *out) {
    if (!scheduler || scheduler->vtable != &owner_scheduler_vtable ||
        !task || !task->run || !out)
        return false;
    *out = owner_try_post_task_after(
        (owner_schedule_state *)scheduler->self, delay, task);
    return true;
}

bool cflow_scheduler_owner_is_thread_internal(
    const cflow_scheduler *scheduler) {
    return scheduler != NULL &&
           scheduler->vtable == &owner_scheduler_vtable &&
           scheduler_on_owner((const owner_schedule_state *)scheduler->self);
}

bool cflow_scheduler_owner_bind(cflow_scheduler *scheduler,
                                cflow_executor *owner_executor,
                                size_t ready_capacity) {
    owner_schedule_state *s;
    cflow_executor_stats exec_stats = {0};
    if (!scheduler || scheduler->self || scheduler->vtable ||
        !owner_executor || !cflow_executor_valid(owner_executor) ||
        !cflow_executor_has(owner_executor, CMETA_EXEC_CAP_OWNER_AFFINE) ||
        !cflow_executor_owner_is_thread_internal(owner_executor) ||
        cflow_executor_is_current_internal(owner_executor) ||
        ready_capacity == 0u ||
        !cflow_executor_get_stats(owner_executor, &exec_stats) ||
        ready_capacity > exec_stats.capacity ||
        ready_capacity > SIZE_MAX / sizeof(owner_schedule_slot) ||
        ready_capacity > SIZE_MAX / sizeof(size_t))
        return false;

    s = (owner_schedule_state *)calloc(1u, sizeof(*s));
    if (!s) return false;
    s->slots = (owner_schedule_slot *)calloc(ready_capacity, sizeof(*s->slots));
    s->free_indices = (size_t *)malloc(ready_capacity *
                                      sizeof(*s->free_indices));
    cmeta_mutex_init(&s->lock);
    if (!s->slots || !s->free_indices || !s->lock) {
        cmeta_mutex_destroy(&s->lock);
        free(s->free_indices);
        free(s->slots);
        free(s);
        return false;
    }
    s->executor = owner_executor;
    s->capacity = ready_capacity;
    s->free_count = ready_capacity;
    for (size_t i = 0u; i < ready_capacity; ++i) {
        s->slots[i].owner = s;
        s->slots[i].index = i;
        s->free_indices[i] = ready_capacity - 1u - i;
    }
    *scheduler = owner_scheduler_as_cflow_scheduler(s);
    return true;
}
