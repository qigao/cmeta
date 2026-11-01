#include <cflow/executor.h>
#include <cflow/event.h>
#include <thread.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Owner-bound Executor: a concrete CFlow Executor provider, NOT an additional
 * thread pool or a generic routing runtime. Reuse the existing bounded typed
 * CFlow Mailbox for concurrent producer -> single owner task admission.
 *
 * The host owns the OS thread and drives run_one with its own fairness budget.
 * The optional wake is a post-publication signal, never a recursive drive.
 */
typedef struct cflow_owner_executor_state {
    cflow_mailbox mailbox;
    cmeta_mutex_t gate;
    const void *owner_thread;
    cflow_task_fn wake;
    void *wake_user;
    size_t capacity;
    size_t queued;
    size_t accepted;
    size_t completed;
    size_t cancelled;
    size_t peak_pending;
    size_t rejected_full;
    size_t rejected_closed;
    size_t rejected_would_block;
    cflow_executor_lifecycle lifecycle;
    cflow_executor_shutdown_policy policy;
    bool policy_selected;
    bool running;
} cflow_owner_executor_state;

static _Thread_local cflow_owner_executor_state *owner_current;

static bool owner_on_thread(const cflow_owner_executor_state *s) {
    return s && s->owner_thread == cmeta_thread_current_token();
}

static void owner_maybe_close(cflow_owner_executor_state *s) {
    if (s->lifecycle == CFLOW_EXECUTOR_CLOSING &&
        s->queued == 0u && !s->running)
        s->lifecycle = CFLOW_EXECUTOR_CLOSED;
}

static cflow_admission_status owner_task_admit_state(
    cflow_owner_executor_state *s, const cflow_executor_task *task) {
    cflow_mailbox_status status;
    cflow_event_view event;
    cflow_task_fn wake = NULL;
    void *wake_user = NULL;
    size_t pending;

    if (!s || !task || !task->run) return CFLOW_ADMISSION_INVALID_ARGUMENT;
    event = (cflow_event_view){1u, &cflow_type_executor_task, task};

    cmeta_mutex_lock(&s->gate);
    if (s->lifecycle != CFLOW_EXECUTOR_OPEN) {
        ++s->rejected_closed;
        cmeta_mutex_unlock(&s->gate);
        return CFLOW_ADMISSION_CLOSED;
    }

    /* Mailbox is the one authoritative bounded queue; do not mirror slots. */
    status = cflow_mailbox_try_send(&s->mailbox, &event);
    if (status == CFLOW_MAILBOX_OK) {
        if (s->queued == 0u && !s->running) {
            wake = s->wake;
            wake_user = s->wake_user;
        }
        ++s->queued;
        ++s->accepted;
        pending = s->queued + (s->running ? 1u : 0u);
        if (pending > s->peak_pending) s->peak_pending = pending;
    } else if (status == CFLOW_MAILBOX_FULL) {
        ++s->rejected_full;
    }
    cmeta_mutex_unlock(&s->gate);

    /* A committed post stays accepted even if host wake delivery is delayed.
     * The host must quiesce posting threads (including wake tails) on destroy. */
    if (wake) wake(wake_user);

    switch (status) {
    case CFLOW_MAILBOX_OK: return CFLOW_ADMISSION_ACCEPTED;
    case CFLOW_MAILBOX_FULL: return CFLOW_ADMISSION_FULL;
    default: return CFLOW_ADMISSION_INVALID_ARGUMENT;
    }
}

static cflow_admission_status owner_task_admit(
    void *self, const cflow_executor_task *task) {
    return owner_task_admit_state((cflow_owner_executor_state *)self, task);
}

static cflow_admission_status owner_try_post(
    void *self, cflow_task_fn fn, void *user) {
    const cflow_executor_task task = {fn, NULL, NULL, user};
    return owner_task_admit(self, &task);
}

static cflow_executor_post_status owner_control_task_post(
    void *self, const cflow_executor_task *task) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    cflow_admission_status admitted = owner_task_admit_state(s, task);
    switch (admitted) {
    case CFLOW_ADMISSION_ACCEPTED: return CFLOW_EXECUTOR_POST_ACCEPTED;
    case CFLOW_ADMISSION_CLOSED: return CFLOW_EXECUTOR_POST_CLOSED;
    case CFLOW_ADMISSION_FULL:
        if (owner_current == s) {
            cmeta_mutex_lock(&s->gate);
            ++s->rejected_would_block;
            cmeta_mutex_unlock(&s->gate);
            return CFLOW_EXECUTOR_POST_WOULD_BLOCK;
        }
        return CFLOW_EXECUTOR_POST_FULL;
    default: return CFLOW_EXECUTOR_POST_INVALID_ARGUMENT;
    }
}

static cflow_executor_post_status owner_control_post(
    void *self, cflow_task_fn fn, void *user) {
    const cflow_executor_task task = {fn, NULL, NULL, user};
    return owner_control_task_post(self, &task);
}

static bool owner_post(void *self, cflow_task_fn fn, void *user) {
    return owner_control_post(self, fn, user) == CFLOW_EXECUTOR_POST_ACCEPTED;
}

static bool owner_run_one(void *self) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    cflow_owner_executor_state *previous;
    cflow_executor_task task = {0};
    cflow_event_id event_id = 0u;
    const cmeta_type_desc *type = NULL;
    cflow_mailbox_status received;
    bool cancel;

    if (!owner_on_thread(s)) return false;
    cmeta_mutex_lock(&s->gate);
    if (s->running || s->queued == 0u) {
        cmeta_mutex_unlock(&s->gate);
        return false;
    }
    received = cflow_mailbox_try_receive(&s->mailbox, &event_id, &type,
                                          &task, sizeof(task));
    /* Internal invariant: only valid executor tasks enter this Mailbox. */
    assert(received == CFLOW_MAILBOX_OK && event_id == 1u &&
           type == &cflow_type_executor_task);
    if (received != CFLOW_MAILBOX_OK) {
        cmeta_mutex_unlock(&s->gate);
        return false;
    }
    --s->queued;
    s->running = true;
    cancel = s->policy_selected &&
             s->policy == CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING;
    cmeta_mutex_unlock(&s->gate);

    previous = owner_current;
    owner_current = s;
    if (cancel) {
        if (task.cancel) task.cancel(task.user);
    } else {
        task.run(task.user);
    }
    if (task.finalize) task.finalize(task.user);
    owner_current = previous;

    cmeta_mutex_lock(&s->gate);
    s->running = false;
    if (cancel) ++s->cancelled;
    else ++s->completed;
    owner_maybe_close(s);
    cmeta_mutex_unlock(&s->gate);
    return true;
}

static size_t owner_run_ready(void *self) {
    size_t count = 0u;
    while (owner_run_one(self)) ++count;
    return count;
}

static cflow_executor_wait_status owner_control_wait_idle(void *self) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    cflow_executor_wait_status result;
    if (!s) return CFLOW_EXECUTOR_WAIT_INVALID_ARGUMENT;
    cmeta_mutex_lock(&s->gate);
    if (owner_current == s) {
        ++s->rejected_would_block;
        result = CFLOW_EXECUTOR_WAIT_WOULD_BLOCK;
    } else {
        owner_maybe_close(s);
        result = s->queued == 0u && !s->running
            ? CFLOW_EXECUTOR_WAIT_IDLE : CFLOW_EXECUTOR_WAIT_PENDING;
    }
    cmeta_mutex_unlock(&s->gate);
    return result;
}

static bool owner_wait_idle(void *self) {
    return owner_control_wait_idle(self) == CFLOW_EXECUTOR_WAIT_IDLE;
}

static size_t owner_pending(void *self) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    size_t pending;
    if (!s) return 0u;
    cmeta_mutex_lock(&s->gate);
    pending = s->queued + (s->running ? 1u : 0u);
    cmeta_mutex_unlock(&s->gate);
    return pending;
}

static bool owner_control_shutdown(
    void *self, cflow_executor_shutdown_policy policy) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    cflow_task_fn wake = NULL;
    void *wake_user = NULL;
    if (!s || (policy != CFLOW_EXECUTOR_SHUTDOWN_DRAIN &&
               policy != CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING))
        return false;
    cmeta_mutex_lock(&s->gate);
    if (s->policy_selected && s->policy != policy) {
        cmeta_mutex_unlock(&s->gate);
        return false;
    }
    if (!s->policy_selected) {
        s->policy_selected = true;
        s->policy = policy;
        s->lifecycle = CFLOW_EXECUTOR_CLOSING;
        if (s->queued != 0u && !s->running) {
            wake = s->wake;
            wake_user = s->wake_user;
        }
    }
    owner_maybe_close(s);
    cmeta_mutex_unlock(&s->gate);
    if (wake) wake(wake_user);
    return true;
}

static bool owner_shutdown(void *self) {
    return owner_control_shutdown(self, CFLOW_EXECUTOR_SHUTDOWN_DRAIN);
}

static bool owner_control_get_stats(
    void *self, cflow_executor_protocol_stats *out) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    if (!s || !out) return false;
    cmeta_mutex_lock(&s->gate);
    owner_maybe_close(s);
    *out = (cflow_executor_protocol_stats){
        .capacity = s->capacity,
        .accepted = s->accepted,
        .queued = s->queued,
        .running = s->running ? 1u : 0u,
        .completed = s->completed,
        .cancelled = s->cancelled,
        .rejected_full = s->rejected_full,
        .rejected_closed = s->rejected_closed,
        .rejected_would_block = s->rejected_would_block,
        .lifecycle = s->lifecycle
    };
    cmeta_mutex_unlock(&s->gate);
    return true;
}

static bool owner_get_stats(void *self, cflow_executor_stats *out) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    if (!s || !out) return false;
    cmeta_mutex_lock(&s->gate);
    *out = (cflow_executor_stats){
        .capacity = s->capacity,
        .pending = s->queued + (s->running ? 1u : 0u),
        .peak_pending = s->peak_pending,
        .rejected_full = s->rejected_full,
        .rejected_closed = s->rejected_closed
    };
    cmeta_mutex_unlock(&s->gate);
    return true;
}

static bool owner_project_control(void *self, cflow_executor_control *out);

static bool owner_is_current(void *self) {
    return self != NULL && owner_current == self;
}

static void owner_destroy(void *self) {
    cflow_owner_executor_state *s = (cflow_owner_executor_state *)self;
    if (!s) return;

    /* Same contract as other Executors: all producers and callback/wake tails
     * must be quiescent before destroy. No callback may destroy its executor. */
    if (!owner_on_thread(s) || owner_current == s || s->running) {
        assert(!"owner-affine Executor destroy requires quiescent owner");
        abort(); /* release builds must not free borrowed pending task state */
    }
    if (s->lifecycle == CFLOW_EXECUTOR_OPEN)
        (void)owner_control_shutdown(s, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING);
    while (owner_run_one(s)) {}
    assert(owner_control_wait_idle(s) == CFLOW_EXECUTOR_WAIT_IDLE);

    cflow_mailbox_destroy(&s->mailbox);
    cmeta_mutex_destroy(&s->gate);
    free(s);
}

CMETA_IMPLEMENTS(cflow_executor, owner_affine_executor,
    CMETA_EXEC_CAP_SERIAL | CMETA_EXEC_CAP_MANUAL |
    CMETA_EXEC_CAP_OWNER_AFFINE,
    .try_post = owner_try_post,
    .post = owner_post,
    .run_one = owner_run_one,
    .run_ready = owner_run_ready,
    .wait_idle = owner_wait_idle,
    .pending = owner_pending,
    .shutdown = owner_shutdown,
    .get_stats = owner_get_stats,
    .destroy = owner_destroy,
    .task_admit = owner_task_admit,
    .project_control = owner_project_control,
    .is_current = owner_is_current
);

CMETA_IMPLEMENTS(cflow_executor_control, owner_affine_control,
    CMETA_EXEC_CAP_SERIAL | CMETA_EXEC_CAP_MANUAL |
    CMETA_EXEC_CAP_OWNER_AFFINE,
    .post = owner_control_post,
    .wait_idle = owner_control_wait_idle,
    .shutdown = owner_control_shutdown,
    .get_stats = owner_control_get_stats,
    .task_post = owner_control_task_post
);

static bool owner_project_control(void *self, cflow_executor_control *out) {
    if (!self || !out || out->self || out->vtable) return false;
    *out = owner_affine_control_as_cflow_executor_control(self);
    return true;
}

bool cflow_executor_owner_is_thread_internal(const cflow_executor *executor) {
    return executor != NULL &&
           executor->vtable == &owner_affine_executor_vtable &&
           owner_on_thread((const cflow_owner_executor_state *)executor->self);
}

bool cflow_executor_owner_init_with_capacity(
    cflow_executor *executor, size_t capacity,
    cflow_task_fn wake, void *wake_user) {
    cflow_owner_executor_state *s;
    const cflow_event_type schema = {1u, &cflow_type_executor_task};
    if (!executor || executor->self || executor->vtable || capacity == 0u)
        return false;
    s = (cflow_owner_executor_state *)calloc(1u, sizeof(*s));
    if (!s) return false;
    cmeta_mutex_init(&s->gate);
    if (!s->gate || cflow_mailbox_init(&s->mailbox, &schema, 1u, capacity) !=
                        CFLOW_MAILBOX_OK) {
        cflow_mailbox_destroy(&s->mailbox);
        cmeta_mutex_destroy(&s->gate);
        free(s);
        return false;
    }
    s->owner_thread = cmeta_thread_current_token();
    s->wake = wake;
    s->wake_user = wake_user;
    s->capacity = capacity;
    s->lifecycle = CFLOW_EXECUTOR_OPEN;
    *executor = owner_affine_executor_as_cflow_executor(s);
    return true;
}
