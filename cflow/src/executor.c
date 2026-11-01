#include <cflow/executor.h>
#include "executor_internal.h"
#include <error_codes.h>
#include <thread_pool.h>

#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static bool cflow_task_fn_equal(const void *left_, const void *right_) {
    const cflow_task_fn *left = (const cflow_task_fn *)left_;
    const cflow_task_fn *right = (const cflow_task_fn *)right_;
    return left != NULL && right != NULL && *left == *right;
}

static const cmeta_type_traits cflow_task_fn_traits = {
    .flags = CMETA_TRAIT_EQUAL |
             CMETA_TRAIT_TRIVIAL_COPY |
             CMETA_TRAIT_TRIVIAL_DESTROY,
    .equal = cflow_task_fn_equal
};

static const cmeta_type_traits cflow_executor_trivial_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY |
             CMETA_TRAIT_TRIVIAL_DESTROY
};

const cmeta_type_desc cflow_type_task_fn = {
    .name = "cflow_task_fn",
    .size = sizeof(cflow_task_fn),
    .align = _Alignof(cflow_task_fn),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &cflow_task_fn_traits,
    .identity = NULL
};

#define CFLOW_EXECUTOR_VALUE_DESC(symbol_, type_, kind_) \
const cmeta_type_desc symbol_ = { \
    .name = #type_, .size = sizeof(type_), .align = _Alignof(type_), \
    .kind = (kind_), .pointee = NULL, \
    .traits = &cflow_executor_trivial_traits, .identity = NULL \
}

CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_shutdown_policy,
    cflow_executor_shutdown_policy, CMETA_T_INTEGER);
CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_post_status,
    cflow_executor_post_status, CMETA_T_INTEGER);
CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_wait_status,
    cflow_executor_wait_status, CMETA_T_INTEGER);
CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_stats,
    cflow_executor_stats, CMETA_T_OBJECT);
CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_protocol_stats,
    cflow_executor_protocol_stats, CMETA_T_OBJECT);
CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_task,
    cflow_executor_task, CMETA_T_OBJECT);
CFLOW_EXECUTOR_VALUE_DESC(
    cflow_type_executor_control,
    cflow_executor_control, CMETA_T_OBJECT);
#undef CFLOW_EXECUTOR_VALUE_DESC

const cmeta_type_desc cflow_type_executor_task_ptr = {
    .name = "cflow_executor_task *",
    .size = sizeof(cflow_executor_task *),
    .align = _Alignof(cflow_executor_task *),
    .kind = CMETA_T_POINTER,
    .pointee = &cflow_type_executor_task,
    .traits = NULL,
    .identity = NULL
};

const cmeta_type_desc cflow_type_executor_control_ptr = {
    .name = "cflow_executor_control *",
    .size = sizeof(cflow_executor_control *),
    .align = _Alignof(cflow_executor_control *),
    .kind = CMETA_T_POINTER,
    .pointee = &cflow_type_executor_control,
    .traits = NULL,
    .identity = NULL
};

const cmeta_type_desc cflow_type_executor_stats_ptr = {
    .name = "cflow_executor_stats *",
    .size = sizeof(cflow_executor_stats *),
    .align = _Alignof(cflow_executor_stats *),
    .kind = CMETA_T_POINTER,
    .pointee = &cflow_type_executor_stats,
    .traits = NULL,
    .identity = NULL
};

const cmeta_type_desc cflow_type_executor_protocol_stats_ptr = {
    .name = "cflow_executor_protocol_stats *",
    .size = sizeof(cflow_executor_protocol_stats *),
    .align = _Alignof(cflow_executor_protocol_stats *),
    .kind = CMETA_T_POINTER,
    .pointee = &cflow_type_executor_protocol_stats,
    .traits = NULL,
    .identity = NULL
};

typedef struct cflow_manual_executor_state {
    cflow_executor_task *tasks;
    size_t count;
    size_t settling;
    size_t capacity;
    size_t peak_pending;
    size_t accepted;
    size_t completed;
    size_t cancelled;
    size_t rejected_full;
    size_t rejected_closed;
    size_t rejected_would_block;
    cflow_executor_lifecycle lifecycle;
    cflow_executor_shutdown_policy shutdown_policy;
    bool shutdown_policy_selected;
    bool running;
} cflow_manual_executor_state;

typedef struct cflow_pool_executor_state {
    cmeta_threadpool_t *pool;
    _Atomic size_t rejected_full;
    _Atomic size_t rejected_closed;
    _Atomic size_t rejected_would_block;
    atomic_int lifecycle;
} cflow_pool_executor_state;

static _Thread_local cflow_manual_executor_state *manual_current = NULL;

static void manual_maybe_close(cflow_manual_executor_state *state) {
    if (state->lifecycle == CFLOW_EXECUTOR_CLOSING &&
        state->count == 0u && state->settling == 0u && !state->running)
        state->lifecycle = CFLOW_EXECUTOR_CLOSED;
}

static cflow_admission_status manual_try_post_task_state(
    cflow_manual_executor_state *state, const cflow_executor_task *task) {
    if (!state || !task || !task->run)
        return CFLOW_ADMISSION_INVALID_ARGUMENT;
    if (state->lifecycle != CFLOW_EXECUTOR_OPEN) {
        ++state->rejected_closed;
        return CFLOW_ADMISSION_CLOSED;
    }
    if (state->count >= state->capacity) {
        ++state->rejected_full;
        return CFLOW_ADMISSION_FULL;
    }
    state->tasks[state->count++] = *task;
    ++state->accepted;
    if (state->count > state->peak_pending)
        state->peak_pending = state->count;
    return CFLOW_ADMISSION_ACCEPTED;
}

static cflow_admission_status manual_try_post(void *self, cflow_task_fn fn,
                                              void *user) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return manual_try_post_task_state(state, &task);
}

static cflow_executor_post_status manual_control_post_task_state(
    cflow_manual_executor_state *state, const cflow_executor_task *task) {
    cflow_admission_status admitted;
    if (!state || !task || !task->run)
        return CFLOW_EXECUTOR_POST_INVALID_ARGUMENT;
    if (state->lifecycle != CFLOW_EXECUTOR_OPEN) {
        ++state->rejected_closed;
        return CFLOW_EXECUTOR_POST_CLOSED;
    }
    if (state->count >= state->capacity && manual_current == state) {
        ++state->rejected_would_block;
        return CFLOW_EXECUTOR_POST_WOULD_BLOCK;
    }
    admitted = manual_try_post_task_state(state, task);
    switch (admitted) {
    case CFLOW_ADMISSION_ACCEPTED: return CFLOW_EXECUTOR_POST_ACCEPTED;
    case CFLOW_ADMISSION_FULL: return CFLOW_EXECUTOR_POST_FULL;
    case CFLOW_ADMISSION_CLOSED: return CFLOW_EXECUTOR_POST_CLOSED;
    default: return CFLOW_EXECUTOR_POST_INVALID_ARGUMENT;
    }
}

static cflow_executor_post_status manual_control_post(
    void *self, cflow_task_fn fn, void *user) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return manual_control_post_task_state(state, &task);
}

static cflow_executor_wait_status manual_control_wait_idle(void *self) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    if (!state) return CFLOW_EXECUTOR_WAIT_INVALID_ARGUMENT;
    if (manual_current == state) {
        ++state->rejected_would_block;
        return CFLOW_EXECUTOR_WAIT_WOULD_BLOCK;
    }
    manual_maybe_close(state);
    return state->count == 0u && state->settling == 0u && !state->running
               ? CFLOW_EXECUTOR_WAIT_IDLE : CFLOW_EXECUTOR_WAIT_PENDING;
}

static void manual_cancel_pending(cflow_manual_executor_state *state) {
    cflow_manual_executor_state *previous;
    size_t task_count;
    if (!state || state->count == 0u) return;

    task_count = state->count;
    state->count = 0u;
    state->settling = task_count;
    previous = manual_current;
    manual_current = state;
    for (size_t i = 0u; i < task_count; ++i) {
        cflow_executor_task *task = &state->tasks[i];
        if (task->cancel) task->cancel(task->user);
        if (task->finalize) task->finalize(task->user);
        --state->settling;
        ++state->cancelled;
    }
    manual_current = previous;
}

static bool manual_control_shutdown(
    void *self, cflow_executor_shutdown_policy policy) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    if (!state || (policy != CFLOW_EXECUTOR_SHUTDOWN_DRAIN &&
                   policy != CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING))
        return false;
    if (state->shutdown_policy_selected) {
        if (state->shutdown_policy != policy) return false;
    } else {
        state->shutdown_policy = policy;
        state->shutdown_policy_selected = true;
        state->lifecycle = CFLOW_EXECUTOR_CLOSING;
        if (policy == CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING)
            manual_cancel_pending(state);
    }
    manual_maybe_close(state);
    return true;
}

static bool manual_control_get_stats(void *self,
                                     cflow_executor_protocol_stats *out) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    if (!state || !out) return false;
    manual_maybe_close(state);
    *out = (cflow_executor_protocol_stats){
        .capacity = state->capacity,
        .accepted = state->accepted,
        .queued = state->count + state->settling,
        .running = state->running ? 1u : 0u,
        .completed = state->completed,
        .cancelled = state->cancelled,
        .rejected_full = state->rejected_full,
        .rejected_closed = state->rejected_closed,
        .rejected_would_block = state->rejected_would_block,
        .lifecycle = state->lifecycle
    };
    return true;
}

static bool manual_post(void *self, cflow_task_fn fn, void *user) {
    return manual_control_post(self, fn, user) ==
           CFLOW_EXECUTOR_POST_ACCEPTED;
}

static bool manual_run_one(void *self) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    cflow_manual_executor_state *previous;
    cflow_executor_task task;

    if (!state || state->count == 0u || state->running || state->settling > 0u)
        return false;
    task = state->tasks[0];
    if (state->count > 1u) {
        memmove(&state->tasks[0], &state->tasks[1],
                (state->count - 1u) * sizeof(state->tasks[0]));
    }
    --state->count;
    state->running = true;
    previous = manual_current;
    manual_current = state;
    task.run(task.user);
    if (task.finalize) task.finalize(task.user);
    manual_current = previous;
    state->running = false;
    ++state->completed;
    manual_maybe_close(state);
    return true;
}

static size_t manual_run_ready(void *self) {
    size_t count = 0u;
    while (manual_run_one(self)) ++count;
    return count;
}

static bool manual_wait_idle(void *self) {
    return manual_control_wait_idle(self) == CFLOW_EXECUTOR_WAIT_IDLE;
}

static size_t manual_pending(void *self) {
    const cflow_manual_executor_state *state =
        (const cflow_manual_executor_state *)self;
    return state ? state->count + state->settling +
                       (state->running ? 1u : 0u) : 0u;
}

static bool manual_shutdown(void *self) {
    return manual_control_shutdown(self, CFLOW_EXECUTOR_SHUTDOWN_DRAIN);
}

static bool manual_get_stats(void *self, cflow_executor_stats *out) {
    const cflow_manual_executor_state *state =
        (const cflow_manual_executor_state *)self;
    if (!state || !out) return false;
    *out = (cflow_executor_stats){
        .capacity = state->capacity,
        .pending = state->count + state->settling +
                   (state->running ? 1u : 0u),
        .peak_pending = state->peak_pending,
        .rejected_full = state->rejected_full,
        .rejected_closed = state->rejected_closed
    };
    return true;
}

static void manual_destroy(void *self) {
    cflow_manual_executor_state *state = (cflow_manual_executor_state *)self;
    if (!state) return;
    state->lifecycle = CFLOW_EXECUTOR_CLOSING;
    manual_cancel_pending(state);
    state->lifecycle = CFLOW_EXECUTOR_CLOSED;
    free(state->tasks);
    free(state);
}

static cflow_admission_status manual_task_admit(
    void *self, const cflow_executor_task *task) {
    return manual_try_post_task_state(
        (cflow_manual_executor_state *)self, task);
}

static cflow_executor_post_status manual_control_task_post(
    void *self, const cflow_executor_task *task) {
    return manual_control_post_task_state(
        (cflow_manual_executor_state *)self, task);
}

static bool manual_project_control(
    void *self, struct cflow_executor_control *out);

static bool manual_is_current(void *self) {
    return self != NULL && self == manual_current;
}

CMETA_IMPLEMENTS(cflow_executor, manual_executor,
    CMETA_EXEC_CAP_MANUAL | CMETA_EXEC_CAP_SERIAL,
    .try_post = manual_try_post,
    .post = manual_post,
    .run_one = manual_run_one,
    .run_ready = manual_run_ready,
    .wait_idle = manual_wait_idle,
    .pending = manual_pending,
    .shutdown = manual_shutdown,
    .get_stats = manual_get_stats,
    .destroy = manual_destroy,
    .task_admit = manual_task_admit,
    .project_control = manual_project_control,
    .is_current = manual_is_current
);

CMETA_IMPLEMENTS(cflow_executor_control, manual_control,
    CMETA_EXEC_CAP_MANUAL | CMETA_EXEC_CAP_SERIAL,
    .post = manual_control_post,
    .wait_idle = manual_control_wait_idle,
    .shutdown = manual_control_shutdown,
    .get_stats = manual_control_get_stats,
    .task_post = manual_control_task_post
);

static bool manual_project_control(
    void *self, struct cflow_executor_control *out) {
    if (!self || !out || out->self || out->vtable) return false;
    *out = manual_control_as_cflow_executor_control(self);
    return true;
}

static cflow_admission_status pool_admission_status(int status) {
    switch (status) {
    case SALTS_OK: return CFLOW_ADMISSION_ACCEPTED;
    case SALTS_EINVAL: return CFLOW_ADMISSION_INVALID_ARGUMENT;
    case SALTS_ENOBUFS: return CFLOW_ADMISSION_FULL;
    case SALTS_ESHUTDOWN: return CFLOW_ADMISSION_CLOSED;
    default: return CFLOW_ADMISSION_CLOSED;
    }
}

static cflow_executor_post_status pool_post_status(int status) {
    switch (status) {
    case SALTS_OK: return CFLOW_EXECUTOR_POST_ACCEPTED;
    case SALTS_EINVAL: return CFLOW_EXECUTOR_POST_INVALID_ARGUMENT;
    case SALTS_ENOBUFS: return CFLOW_EXECUTOR_POST_FULL;
    case SALTS_ESHUTDOWN: return CFLOW_EXECUTOR_POST_CLOSED;
    case SALTS_EBUSY: return CFLOW_EXECUTOR_POST_WOULD_BLOCK;
    default: return CFLOW_EXECUTOR_POST_CLOSED;
    }
}

static size_t pool_counter_size(int64_t value) {
    if (value <= 0) return 0u;
    if ((uint64_t)value > (uint64_t)SIZE_MAX) return SIZE_MAX;
    return (size_t)value;
}

static void pool_refresh_lifecycle(cflow_pool_executor_state *state) {
    int expected = CFLOW_EXECUTOR_CLOSING;
    if (state && state->pool && cmeta_threadpool_pending(state->pool) == 0)
        (void)atomic_compare_exchange_strong(
            &state->lifecycle, &expected, CFLOW_EXECUTOR_CLOSED);
}

static cmeta_threadpool_task_t pool_task_descriptor(
    const cflow_executor_task *task) {
    return (cmeta_threadpool_task_t){
        .run = task->run,
        .cancel = task->cancel,
        .finalize = task->finalize,
        .arg = task->user
    };
}

static cflow_admission_status pool_try_post_task_state(
    cflow_pool_executor_state *state, const cflow_executor_task *task) {
    cflow_admission_status status;
    cmeta_threadpool_task_t pool_task;
    if (!state || !state->pool || !task || !task->run)
        return CFLOW_ADMISSION_INVALID_ARGUMENT;
    pool_task = pool_task_descriptor(task);
    status = pool_admission_status(
        cmeta_threadpool_try_submit_task(state->pool, &pool_task));
    if (status == CFLOW_ADMISSION_FULL)
        atomic_fetch_add(&state->rejected_full, 1u);
    else if (status == CFLOW_ADMISSION_CLOSED)
        atomic_fetch_add(&state->rejected_closed, 1u);
    return status;
}

static cflow_admission_status pool_task_admit(
    void *self, const cflow_executor_task *task) {
    return pool_try_post_task_state((cflow_pool_executor_state *)self, task);
}

static cflow_admission_status pool_try_post(void *self, cflow_task_fn fn,
                                            void *user) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return pool_try_post_task_state(state, &task);
}

static cflow_executor_post_status pool_control_post_task_state(
    cflow_pool_executor_state *state, const cflow_executor_task *task) {
    cflow_executor_post_status status;
    cmeta_threadpool_task_t pool_task;
    if (!state || !state->pool || !task || !task->run)
        return CFLOW_EXECUTOR_POST_INVALID_ARGUMENT;
    pool_task = pool_task_descriptor(task);
    status = pool_post_status(
        cmeta_threadpool_submit_task(state->pool, &pool_task));
    if (status == CFLOW_EXECUTOR_POST_FULL)
        atomic_fetch_add(&state->rejected_full, 1u);
    else if (status == CFLOW_EXECUTOR_POST_CLOSED)
        atomic_fetch_add(&state->rejected_closed, 1u);
    else if (status == CFLOW_EXECUTOR_POST_WOULD_BLOCK)
        atomic_fetch_add(&state->rejected_would_block, 1u);
    return status;
}

static cflow_executor_post_status pool_control_task_post(
    void *self, const cflow_executor_task *task) {
    return pool_control_post_task_state(
        (cflow_pool_executor_state *)self, task);
}

static cflow_executor_post_status pool_control_post(
    void *self, cflow_task_fn fn, void *user) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    const cflow_executor_task task = {
        .run = fn,
        .cancel = NULL,
        .finalize = NULL,
        .user = user
    };
    return pool_control_post_task_state(state, &task);
}

static bool pool_post(void *self, cflow_task_fn fn, void *user) {
    return pool_control_post(self, fn, user) == CFLOW_EXECUTOR_POST_ACCEPTED;
}

static bool pool_run_one(void *self) {
    (void)self;
    return false;
}

static size_t pool_run_ready(void *self) {
    (void)self;
    return 0u;
}

static cflow_executor_wait_status pool_control_wait_idle(void *self) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    int status;
    if (!state || !state->pool) return CFLOW_EXECUTOR_WAIT_INVALID_ARGUMENT;
    status = cmeta_threadpool_wait_status(state->pool);
    if (status == SALTS_EBUSY) {
        atomic_fetch_add(&state->rejected_would_block, 1u);
        return CFLOW_EXECUTOR_WAIT_WOULD_BLOCK;
    }
    if (status != SALTS_OK) return CFLOW_EXECUTOR_WAIT_INVALID_ARGUMENT;
    pool_refresh_lifecycle(state);
    return CFLOW_EXECUTOR_WAIT_IDLE;
}

static bool pool_wait_idle(void *self) {
    return pool_control_wait_idle(self) == CFLOW_EXECUTOR_WAIT_IDLE;
}

static size_t pool_pending(void *self) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    int pending;
    if (!state || !state->pool) return 0u;
    pending = cmeta_threadpool_pending(state->pool);
    return pending > 0 ? (size_t)pending : 0u;
}

static bool pool_control_shutdown(
    void *self, cflow_executor_shutdown_policy policy) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    cmeta_threadpool_shutdown_policy_t pool_policy;
    int status;
    if (!state || !state->pool ||
        (policy != CFLOW_EXECUTOR_SHUTDOWN_DRAIN &&
         policy != CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING))
        return false;
    pool_policy = policy == CFLOW_EXECUTOR_SHUTDOWN_DRAIN
                      ? SALTS_THREADPOOL_SHUTDOWN_DRAIN
                      : SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING;
    status = cmeta_threadpool_shutdown_with_policy(state->pool, pool_policy);
    if (status != SALTS_OK) return false;
    atomic_store(&state->lifecycle, CFLOW_EXECUTOR_CLOSING);
    pool_refresh_lifecycle(state);
    return true;
}

static bool pool_shutdown(void *self) {
    return pool_control_shutdown(self, CFLOW_EXECUTOR_SHUTDOWN_DRAIN);
}

static bool pool_get_stats(void *self, cflow_executor_stats *out) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    cmeta_threadpool_stats_t pool_stats;
    if (!state || !state->pool || !out) return false;
    cmeta_threadpool_get_stats(state->pool, &pool_stats);
    *out = (cflow_executor_stats){
        .capacity = pool_stats.queue_capacity,
        .pending = pool_counter_size(pool_stats.pending_tasks),
        .peak_pending = pool_counter_size(pool_stats.peak_pending_tasks),
        .rejected_full = atomic_load(&state->rejected_full),
        .rejected_closed = atomic_load(&state->rejected_closed)
    };
    return true;
}

static bool pool_control_get_stats(void *self,
                                   cflow_executor_protocol_stats *out) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    cmeta_threadpool_stats_t stats;
    size_t queued;
    size_t running;
    size_t completed;
    size_t cancelled;
    if (!state || !state->pool || !out) return false;
    cmeta_threadpool_get_stats(state->pool, &stats);
    pool_refresh_lifecycle(state);
    queued = pool_counter_size(stats.queued_tasks);
    running = pool_counter_size(stats.active_tasks);
    completed = pool_counter_size(stats.completed_tasks);
    cancelled = pool_counter_size(cmeta_threadpool_cancelled(state->pool));
    *out = (cflow_executor_protocol_stats){
        .capacity = stats.queue_capacity,
        .accepted = pool_counter_size(stats.submitted_tasks),
        .queued = queued,
        .running = running,
        .completed = completed,
        .cancelled = cancelled,
        .rejected_full = atomic_load(&state->rejected_full),
        .rejected_closed = atomic_load(&state->rejected_closed),
        .rejected_would_block =
            atomic_load(&state->rejected_would_block),
        .lifecycle = (cflow_executor_lifecycle)
            atomic_load(&state->lifecycle)
    };
    return true;
}

static void pool_destroy(void *self) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    if (!state) return;
    cmeta_threadpool_destroy(state->pool);
    atomic_store(&state->lifecycle, CFLOW_EXECUTOR_CLOSED);
    free(state);
}

static bool serial_project_control(
    void *self, struct cflow_executor_control *out);
static bool worker_project_control(
    void *self, struct cflow_executor_control *out);

static bool pool_is_current(void *self) {
    cflow_pool_executor_state *state = (cflow_pool_executor_state *)self;
    return state && state->pool &&
           cmeta_threadpool_is_current_internal(state->pool) != 0;
}

CMETA_IMPLEMENTS(cflow_executor, serial_executor,
    CMETA_EXEC_CAP_SERIAL,
    .try_post = pool_try_post,
    .post = pool_post,
    .run_one = pool_run_one,
    .run_ready = pool_run_ready,
    .wait_idle = pool_wait_idle,
    .pending = pool_pending,
    .shutdown = pool_shutdown,
    .get_stats = pool_get_stats,
    .destroy = pool_destroy,
    .task_admit = pool_task_admit,
    .project_control = serial_project_control,
    .is_current = pool_is_current
);

CMETA_IMPLEMENTS(cflow_executor, worker_executor,
    CMETA_EXEC_CAP_CONCURRENT,
    .try_post = pool_try_post,
    .post = pool_post,
    .run_one = pool_run_one,
    .run_ready = pool_run_ready,
    .wait_idle = pool_wait_idle,
    .pending = pool_pending,
    .shutdown = pool_shutdown,
    .get_stats = pool_get_stats,
    .destroy = pool_destroy,
    .task_admit = pool_task_admit,
    .project_control = worker_project_control,
    .is_current = pool_is_current
);

CMETA_IMPLEMENTS(cflow_executor_control, serial_control,
    CMETA_EXEC_CAP_SERIAL,
    .post = pool_control_post,
    .wait_idle = pool_control_wait_idle,
    .shutdown = pool_control_shutdown,
    .get_stats = pool_control_get_stats,
    .task_post = pool_control_task_post
);

CMETA_IMPLEMENTS(cflow_executor_control, worker_control,
    CMETA_EXEC_CAP_CONCURRENT,
    .post = pool_control_post,
    .wait_idle = pool_control_wait_idle,
    .shutdown = pool_control_shutdown,
    .get_stats = pool_control_get_stats,
    .task_post = pool_control_task_post
);

static bool serial_project_control(
    void *self, struct cflow_executor_control *out) {
    if (!self || !out || out->self || out->vtable) return false;
    *out = serial_control_as_cflow_executor_control(self);
    return true;
}

static bool worker_project_control(
    void *self, struct cflow_executor_control *out) {
    if (!self || !out || out->self || out->vtable) return false;
    *out = worker_control_as_cflow_executor_control(self);
    return true;
}

bool cflow_executor_as_control(cflow_executor *executor,
                               cflow_executor_control *out) {
    if (!cflow_executor_valid(executor) || !out || out->self || out->vtable)
        return false;
    return executor->vtable->project_control(executor->self, out);
}

cflow_admission_status cflow_executor_try_post_task(
    cflow_executor *executor, const cflow_executor_task *task) {
    if (!cflow_executor_valid(executor) || !task || !task->run)
        return CFLOW_ADMISSION_INVALID_ARGUMENT;
    return executor->vtable->task_admit(executor->self, task);
}

cflow_executor_post_status cflow_executor_control_post_task(
    cflow_executor_control *control, const cflow_executor_task *task) {
    if (!cflow_executor_control_valid(control) || !task || !task->run)
        return CFLOW_EXECUTOR_POST_INVALID_ARGUMENT;
    return control->vtable->task_post(control->self, task);
}

bool cflow_executor_is_current_internal(const cflow_executor *executor) {
    if (!cflow_executor_valid(executor)) return false;
    return executor->vtable->is_current(executor->self);
}

bool cflow_executor_manual_init_with_capacity(cflow_executor *executor,
                                              size_t capacity) {
    cflow_manual_executor_state *state;
    if (!executor || executor->self || executor->vtable) return false;
    if (capacity == 0u || capacity > SIZE_MAX / sizeof(cflow_executor_task))
        return false;
    state = (cflow_manual_executor_state *)calloc(1, sizeof(*state));
    if (!state) return false;
    state->tasks = (cflow_executor_task *)calloc(
        capacity, sizeof(*state->tasks));
    if (!state->tasks) {
        free(state);
        return false;
    }
    state->capacity = capacity;
    state->lifecycle = CFLOW_EXECUTOR_OPEN;
    *executor = manual_executor_as_cflow_executor(state);
    return true;
}

bool cflow_executor_manual_init(cflow_executor *executor) {
    return cflow_executor_manual_init_with_capacity(
        executor, CFLOW_EXECUTOR_DEFAULT_CAPACITY);
}

static bool pool_executor_init(cflow_executor *executor, size_t workers,
                               size_t capacity, bool serial) {
    cflow_pool_executor_state *state;
    cmeta_threadpool_config_t config;
    if (!executor || executor->self || executor->vtable) return false;
    if (workers == 0u || workers > (size_t)INT_MAX || capacity == 0u)
        return false;
    state = (cflow_pool_executor_state *)calloc(1, sizeof(*state));
    if (!state) return false;
    config.num_threads = (int)workers;
    config.queue_capacity = capacity;
    state->pool = cmeta_threadpool_create_with_config(&config);
    if (!state->pool) {
        free(state);
        return false;
    }
    atomic_init(&state->lifecycle, CFLOW_EXECUTOR_OPEN);
    *executor = serial ? serial_executor_as_cflow_executor(state)
                       : worker_executor_as_cflow_executor(state);
    return true;
}

bool cflow_executor_serial_init(cflow_executor *executor) {
    return cflow_executor_serial_init_with_capacity(
        executor, CFLOW_EXECUTOR_DEFAULT_CAPACITY);
}

bool cflow_executor_serial_init_with_capacity(cflow_executor *executor,
                                              size_t capacity) {
    return pool_executor_init(executor, 1u, capacity, true);
}

bool cflow_executor_worker_init(cflow_executor *executor, size_t workers) {
    return cflow_executor_worker_init_with_capacity(
        executor, workers, CFLOW_EXECUTOR_DEFAULT_CAPACITY);
}

bool cflow_executor_worker_init_with_capacity(cflow_executor *executor,
                                              size_t workers,
                                              size_t capacity) {
    return pool_executor_init(executor, workers, capacity, false);
}
