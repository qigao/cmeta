#include <cflow/machine_instance.h>

#include "machine_instance_internal.h"
#include "executor_internal.h"
#include "event_internal.h"

#include <thread.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef CFLOW_MACHINE_INSTANCE_QUANTUM
#define CFLOW_MACHINE_INSTANCE_QUANTUM 64u
#endif

typedef enum cflow_machine_ready_kind {
    CFLOW_MACHINE_READY_NONE = 0,
    CFLOW_MACHINE_READY_VALUE,
    CFLOW_MACHINE_READY_VALUE_AND_DONE,
    CFLOW_MACHINE_READY_DONE,
    CFLOW_MACHINE_READY_ERROR
} cflow_machine_ready_kind;

typedef enum cflow_machine_control_lifecycle {
    CFLOW_MACHINE_CONTROL_OPEN = 0,
    CFLOW_MACHINE_CONTROL_CLOSE_REQUESTED,
    CFLOW_MACHINE_CONTROL_CANCEL_REQUESTED,
    CFLOW_MACHINE_CONTROL_TERMINAL
} cflow_machine_control_lifecycle;

typedef enum cflow_machine_worker_phase {
    CFLOW_MACHINE_WORKER_IDLE = 0,
    CFLOW_MACHINE_WORKER_SCHEDULED,
    CFLOW_MACHINE_WORKER_EXECUTING,
    CFLOW_MACHINE_WORKER_COMMITTING
} cflow_machine_worker_phase;

typedef enum cflow_machine_commit_decision {
    CFLOW_MACHINE_COMMIT_ALLOWED = 0,
    CFLOW_MACHINE_COMMIT_CANCELLED,
    CFLOW_MACHINE_COMMIT_INVALID
} cflow_machine_commit_decision;

typedef struct cflow_machine_instance_impl {
    const cflow_machine *machine;
    cflow_executor *executor;
    cflow_mailbox mailbox;
    bool mailbox_initialized;
    cflow_machine_guard_binding *guards;
    size_t guard_count;
    cflow_machine_action_binding *actions;
    size_t action_count;
    const cmeta_type_desc *output_type;
    const cflow_machine_state *state;
    unsigned char *state_value;
    size_t state_capacity;
    unsigned char *target_value;
    unsigned char *event_value;
    unsigned char *observation_value;
    size_t event_capacity;
    size_t observation_capacity;
    cmeta_mutex_t lock;
    const char *error;
    bool error_owned;
    uint64_t completed;
    uint64_t failed;
    uint64_t cancelled_in_flight;
    uint64_t emitted_values;
    uint64_t emitted_events;
    size_t in_flight;
    bool closed;
    bool cancelled;
    bool done;
    bool adapter_attached;
    bool rerun;
    bool mailbox_armed;
    cflow_machine_control_lifecycle lifecycle;
    cflow_machine_worker_phase worker_phase;
    cflow_waitable mailbox_waitable;
    cflow_waker downstream_waiter;
    cflow_waker terminal_waiter;
    cflow_machine_ready_kind ready;
    cflow_machine_transition_commit_hook commit_hook;
    void *commit_user;
    cflow_machine_commit_boundary_hook boundary_hook;
    void *boundary_user;
} cflow_machine_instance_impl;

static void machine_executor_task(void *user);
static bool schedule_machine_task(cflow_machine_instance_impl *impl);

static bool runtime_alignment_valid(size_t alignment) {
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u &&
           alignment <= _Alignof(cmeta_capture_storage);
}

static bool runtime_type_supported(const cmeta_type_desc *type) {
    const cmeta_trait_flags required =
        CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY;
    return cmeta_type_desc_valid(type) && type->size != 0u &&
           runtime_alignment_valid(type->align) &&
           cmeta_type_require_traits(type, required) == CMETA_OK;
}

static const cflow_machine_state *find_state(
    const cflow_machine *machine, cflow_machine_state_id id) {
    size_t left = 0u;
    size_t right = cflow_machine_state_count(machine);
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_state *state =
            cflow_machine_state_at(machine, middle);
        if (state->id == id) return state;
        if (state->id < id)
            left = middle + 1u;
        else
            right = middle;
    }
    return NULL;
}

static int compare_guard_binding(const void *left, const void *right) {
    const cflow_machine_guard_binding *a =
        (const cflow_machine_guard_binding *)left;
    const cflow_machine_guard_binding *b =
        (const cflow_machine_guard_binding *)right;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int compare_action_binding(const void *left, const void *right) {
    const cflow_machine_action_binding *a =
        (const cflow_machine_action_binding *)left;
    const cflow_machine_action_binding *b =
        (const cflow_machine_action_binding *)right;
    return a->id < b->id ? -1 : a->id > b->id;
}

static void instance_impl_free(cflow_machine_instance_impl *impl) {
    if (impl == NULL) return;
    if (impl->mailbox_initialized) cflow_mailbox_destroy(&impl->mailbox);
    if (impl->lock != NULL) cmeta_mutex_destroy(&impl->lock);
    if (impl->error_owned) free((void *)impl->error);
    free(impl->observation_value);
    free(impl->event_value);
    free(impl->target_value);
    free(impl->state_value);
    free(impl->actions);
    free(impl->guards);
    free(impl);
}

static char *runtime_copy_error(const char *message) {
    const char *source = message != NULL && message[0] != '\0'
        ? message : "machine runtime error";
    const size_t length = strlen(source);
    char *copy;
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1u);
    if (copy != NULL) memcpy(copy, source, length + 1u);
    return copy;
}

static void invoke_waker(cflow_waker waker) {
    if (waker.wake != NULL) waker.wake(waker.user);
}

static cflow_waker take_downstream_waiter_locked(
    cflow_machine_instance_impl *impl) {
    cflow_waker waker = impl->downstream_waiter;
    impl->downstream_waiter = (cflow_waker){0};
    return waker;
}

static cflow_waker take_terminal_waiter_locked(
    cflow_machine_instance_impl *impl) {
    cflow_waker waker = impl->terminal_waiter;
    impl->terminal_waiter = (cflow_waker){0};
    return waker;
}

static void publish_control_terminal_locked(
    cflow_machine_instance_impl *impl) {
    impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
    impl->done = true;
    if (impl->ready == CFLOW_MACHINE_READY_VALUE)
        impl->ready = CFLOW_MACHINE_READY_VALUE_AND_DONE;
    else if (impl->ready == CFLOW_MACHINE_READY_NONE)
        impl->ready = CFLOW_MACHINE_READY_DONE;
}

static cflow_machine_commit_decision begin_commit_locked(
    cflow_machine_instance_impl *impl) {
    if (impl->lifecycle == CFLOW_MACHINE_CONTROL_CANCEL_REQUESTED)
        return CFLOW_MACHINE_COMMIT_CANCELLED;
    if (impl->worker_phase != CFLOW_MACHINE_WORKER_EXECUTING ||
        (impl->lifecycle != CFLOW_MACHINE_CONTROL_OPEN &&
         impl->lifecycle != CFLOW_MACHINE_CONTROL_CLOSE_REQUESTED))
        return CFLOW_MACHINE_COMMIT_INVALID;
    impl->worker_phase = CFLOW_MACHINE_WORKER_COMMITTING;
    return CFLOW_MACHINE_COMMIT_ALLOWED;
}

static void cancel_mailbox(cflow_machine_instance_impl *impl) {
    if (impl->mailbox_initialized)
        (void)cflow_mailbox_cancel(&impl->mailbox);
}

static void fail_runtime(cflow_machine_instance_impl *impl,
                         const char *message,
                         bool settle_current_event) {
    char *copy;
    cflow_waker waker = {0};
    cflow_waker terminal_waker = {0};
    bool cancel_won = false;
    if (impl == NULL) return;
    copy = runtime_copy_error(message);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, true);
    cmeta_mutex_lock(&impl->lock);
    cancel_won = impl->lifecycle == CFLOW_MACHINE_CONTROL_CANCEL_REQUESTED;
    if (!cancel_won && impl->error == NULL) {
        if (copy != NULL) {
            impl->error = copy;
            impl->error_owned = true;
            copy = NULL;
        } else {
            impl->error = "machine runtime could not preserve error text";
        }
    }
    if (settle_current_event) {
        if (impl->in_flight != 0u) --impl->in_flight;
        if (cancel_won) {
            if (impl->cancelled_in_flight != UINT64_MAX)
                ++impl->cancelled_in_flight;
        } else if (impl->failed != UINT64_MAX) {
            ++impl->failed;
        }
    }
    impl->worker_phase = CFLOW_MACHINE_WORKER_IDLE;
    impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
    impl->done = true;
    impl->ready = cancel_won
        ? CFLOW_MACHINE_READY_DONE : CFLOW_MACHINE_READY_ERROR;
    waker = take_downstream_waiter_locked(impl);
    terminal_waker = take_terminal_waiter_locked(impl);
    cmeta_mutex_unlock(&impl->lock);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, false);
    free(copy);
    cancel_mailbox(impl);
    invoke_waker(waker);
    invoke_waker(terminal_waker);
}

static const cflow_machine_guard_binding *find_guard_binding(
    const cflow_machine_instance_impl *impl,
    cflow_machine_guard_id id) {
    size_t left = 0u;
    size_t right = impl->guard_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_guard_binding *binding = &impl->guards[middle];
        if (binding->id == id) return binding;
        if (binding->id < id)
            left = middle + 1u;
        else
            right = middle;
    }
    return NULL;
}

static const cflow_machine_action_binding *find_action_binding(
    const cflow_machine_instance_impl *impl,
    cflow_machine_action_id id) {
    size_t left = 0u;
    size_t right = impl->action_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_action_binding *binding = &impl->actions[middle];
        if (binding->id == id) return binding;
        if (binding->id < id)
            left = middle + 1u;
        else
            right = middle;
    }
    return NULL;
}

static const cflow_machine_action *find_action_declaration(
    const cflow_machine_instance_impl *impl,
    cflow_machine_action_id id) {
    size_t left = 0u;
    size_t right = cflow_machine_action_count(impl->machine);
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_action *action =
            cflow_machine_action_at(impl->machine, middle);
        if (action->id == id) return action;
        if (action->id < id)
            left = middle + 1u;
        else
            right = middle;
    }
    return NULL;
}

static bool transition_enabled(
    cflow_machine_instance_impl *impl,
    const cflow_machine_transition *transition,
    const void *event_value,
    bool *enabled,
    const char **error) {
    const cflow_machine_guard_binding *binding;
    if (transition->guard == 0u) {
        *enabled = true;
        return true;
    }
    binding = find_guard_binding(impl, transition->guard);
    if (binding == NULL) {
        *error = "machine guard binding is missing";
        return false;
    }
    return binding->fn(binding->user, impl->state_value, event_value,
                       enabled, error);
}

static const cflow_machine_transition *select_transition(
    cflow_machine_instance_impl *impl,
    cflow_event_id event_id,
    const void *event_value,
    const char **error,
    size_t *out_index) {
    const size_t count = cflow_machine_transition_count(impl->machine);
    size_t left = 0u;
    size_t right = count;
    size_t index;

    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_transition *transition =
            cflow_machine_transition_at(impl->machine, middle);
        if (transition->source < impl->state->id ||
            (transition->source == impl->state->id &&
             transition->event < event_id))
            left = middle + 1u;
        else
            right = middle;
    }
    for (index = left; index < count; ++index) {
        const cflow_machine_transition *transition =
            cflow_machine_transition_at(impl->machine, index);
        bool enabled = false;
        if (transition->source != impl->state->id ||
            transition->event != event_id)
            break;
        if (!transition_enabled(impl, transition, event_value,
                                &enabled, error))
            return NULL;
        if (enabled) {
            *out_index = index;
            return transition;
        }
    }
    if (*error == NULL) *error = "no enabled transition";
    return NULL;
}

static bool run_action(cflow_machine_instance_impl *impl,
                       const cflow_machine_transition *transition,
                       const void *event_value,
                       const cflow_machine_action **out_action,
                       const char **error) {
    const cflow_machine_action *action;
    const cflow_machine_action_binding *binding;
    void *observation;
    if (transition->action == 0u) {
        memcpy(impl->target_value, impl->state_value,
               impl->state->value_type->size);
        *out_action = NULL;
        return true;
    }
    action = find_action_declaration(impl, transition->action);
    binding = find_action_binding(impl, transition->action);
    if (action == NULL || binding == NULL) {
        *error = "machine action binding is missing";
        return false;
    }
    observation = action->observation == CFLOW_MACHINE_ACTION_NONE
        ? NULL : impl->observation_value;
    if (!binding->fn(binding->user, impl->state_value, event_value,
                     impl->target_value, observation, error)) {
        if ((action->effects & CMETA_EFFECT_MAY_FAIL) == 0u)
            *error = "machine action contract violation";
        else if (*error == NULL)
            *error = "machine action failed";
        return false;
    }
    *out_action = action;
    return true;
}

static bool emit_action_event(cflow_machine_instance_impl *impl,
                              const cflow_machine_action *action,
                              const char **error) {
    const cflow_event_view event = {
        action->output_event_id,
        action->output_type,
        impl->observation_value
    };
    const cflow_mailbox_status status =
        cflow_mailbox_try_send(&impl->mailbox, &event);
    if (status != CFLOW_MAILBOX_OK) {
        *error = status == CFLOW_MAILBOX_FULL
            ? "machine emitted Event mailbox is full"
            : "machine emitted Event was rejected";
        return false;
    }
    return true;
}

static void publish_done(cflow_machine_instance_impl *impl,
                         cflow_machine_ready_kind ready) {
    cflow_waker waker;
    cflow_waker terminal_waker;
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, true);
    cmeta_mutex_lock(&impl->lock);
    impl->worker_phase = CFLOW_MACHINE_WORKER_IDLE;
    impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
    impl->done = true;
    impl->ready = ready;
    waker = take_downstream_waiter_locked(impl);
    terminal_waker = take_terminal_waiter_locked(impl);
    cmeta_mutex_unlock(&impl->lock);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, false);
    cancel_mailbox(impl);
    invoke_waker(waker);
    invoke_waker(terminal_waker);
}

static bool settle_cancelled_in_flight(cflow_machine_instance_impl *impl) {
    cflow_waker waker = {0};
    cflow_waker terminal_waker = {0};
    bool cancelled;
    cmeta_mutex_lock(&impl->lock);
    cancelled =
        impl->lifecycle == CFLOW_MACHINE_CONTROL_CANCEL_REQUESTED;
    if (cancelled) {
        if (impl->in_flight != 0u) --impl->in_flight;
        if (impl->cancelled_in_flight != UINT64_MAX)
            ++impl->cancelled_in_flight;
        impl->worker_phase = CFLOW_MACHINE_WORKER_IDLE;
        impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
        impl->done = true;
        impl->ready = CFLOW_MACHINE_READY_DONE;
        waker = take_downstream_waiter_locked(impl);
        terminal_waker = take_terminal_waiter_locked(impl);
    }
    cmeta_mutex_unlock(&impl->lock);
    if (cancelled) {
        invoke_waker(waker);
        invoke_waker(terminal_waker);
    }
    return cancelled;
}

static bool process_event(cflow_machine_instance_impl *impl,
                          cflow_event_id event_id,
                          const void *event_value) {
    const cflow_machine_transition *transition;
    const cflow_machine_action *action = NULL;
    const cflow_machine_state *target;
    const char *error = NULL;
    cflow_waker waker = {0};
    cflow_waker terminal_waker = {0};
    bool runtime_closed = false;
    bool cancel_won = false;
    bool invalid_commit = false;
    size_t transition_index = 0u;

    if (settle_cancelled_in_flight(impl)) return false;
    transition = select_transition(impl, event_id, event_value, &error,
                                   &transition_index);
    if (settle_cancelled_in_flight(impl)) return false;
    if (transition == NULL) {
        fail_runtime(impl, error, true);
        return false;
    }
    if (!run_action(impl, transition, event_value, &action, &error)) {
        if (settle_cancelled_in_flight(impl)) return false;
        fail_runtime(impl, error, true);
        return false;
    }
    if (settle_cancelled_in_flight(impl)) return false;
    target = find_state(impl->machine, transition->target);
    if (target == NULL) {
        fail_runtime(impl, "machine transition target is invalid", true);
        return false;
    }
    if (action != NULL && action->observation == CFLOW_MACHINE_ACTION_EVENT) {
        bool closed;
        cmeta_mutex_lock(&impl->lock);
        closed = impl->closed;
        cmeta_mutex_unlock(&impl->lock);
        if (!closed && !emit_action_event(impl, action, &error)) {
            fail_runtime(impl, error, true);
            return false;
        }
    }

    if (impl->boundary_hook != NULL)
        impl->boundary_hook(impl->boundary_user);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, transition_index, true);
    cmeta_mutex_lock(&impl->lock);
    switch (begin_commit_locked(impl)) {
    case CFLOW_MACHINE_COMMIT_CANCELLED:
        if (impl->in_flight != 0u) --impl->in_flight;
        if (impl->cancelled_in_flight != UINT64_MAX)
            ++impl->cancelled_in_flight;
        impl->worker_phase = CFLOW_MACHINE_WORKER_IDLE;
        impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
        impl->done = true;
        impl->ready = CFLOW_MACHINE_READY_DONE;
        waker = take_downstream_waiter_locked(impl);
        terminal_waker = take_terminal_waiter_locked(impl);
        cancel_won = true;
        break;
    case CFLOW_MACHINE_COMMIT_INVALID:
        invalid_commit = true;
        break;
    case CFLOW_MACHINE_COMMIT_ALLOWED:
        break;
    }
    if (cancel_won || invalid_commit) {
        cmeta_mutex_unlock(&impl->lock);
        if (impl->commit_hook != NULL)
            impl->commit_hook(impl->commit_user, SIZE_MAX, false);
        if (invalid_commit) {
            fail_runtime(impl, "machine commit phase is invalid", true);
            return false;
        }
        cancel_mailbox(impl);
        invoke_waker(waker);
        invoke_waker(terminal_waker);
        return false;
    }
    memcpy(impl->state_value, impl->target_value, target->value_type->size);
    impl->state = target;
    if (impl->in_flight != 0u) --impl->in_flight;
    if (target->kind == CFLOW_MACHINE_STATE_ERROR) {
        if (impl->failed != UINT64_MAX) ++impl->failed;
    } else if (impl->completed != UINT64_MAX) {
        ++impl->completed;
    }
    if (action != NULL && action->observation == CFLOW_MACHINE_ACTION_EVENT &&
        !impl->closed) {
        if (impl->emitted_events != UINT64_MAX) ++impl->emitted_events;
    } else if (action != NULL &&
               action->observation == CFLOW_MACHINE_ACTION_VALUE) {
        if (impl->emitted_values != UINT64_MAX) ++impl->emitted_values;
        impl->ready = target->kind == CFLOW_MACHINE_STATE_ACTIVE
            ? CFLOW_MACHINE_READY_VALUE
            : target->kind == CFLOW_MACHINE_STATE_DONE
                ? CFLOW_MACHINE_READY_VALUE_AND_DONE
                : CFLOW_MACHINE_READY_ERROR;
        if (target->kind != CFLOW_MACHINE_STATE_ACTIVE) impl->done = true;
        waker = take_downstream_waiter_locked(impl);
    } else if (target->kind == CFLOW_MACHINE_STATE_DONE) {
        impl->done = true;
        impl->ready = CFLOW_MACHINE_READY_DONE;
        waker = take_downstream_waiter_locked(impl);
    }
    runtime_closed =
        impl->lifecycle == CFLOW_MACHINE_CONTROL_CLOSE_REQUESTED;
    if (runtime_closed && target->kind == CFLOW_MACHINE_STATE_ACTIVE) {
        impl->done = true;
        impl->ready = action != NULL &&
                              action->observation == CFLOW_MACHINE_ACTION_VALUE
            ? CFLOW_MACHINE_READY_VALUE_AND_DONE
            : CFLOW_MACHINE_READY_DONE;
        waker = take_downstream_waiter_locked(impl);
    }
    impl->worker_phase = CFLOW_MACHINE_WORKER_SCHEDULED;
    if (impl->done || target->kind == CFLOW_MACHINE_STATE_ERROR)
        impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
    if (impl->done && target->kind != CFLOW_MACHINE_STATE_ERROR)
        terminal_waker = take_terminal_waiter_locked(impl);
    cmeta_mutex_unlock(&impl->lock);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, transition_index, false);
    if (target->kind == CFLOW_MACHINE_STATE_ERROR) {
        fail_runtime(impl, "entered error state", false);
        return false;
    }
    if (target->kind == CFLOW_MACHINE_STATE_DONE || runtime_closed)
        cancel_mailbox(impl);
    invoke_waker(waker);
    invoke_waker(terminal_waker);
    return target->kind == CFLOW_MACHINE_STATE_ACTIVE &&
           !runtime_closed &&
           (action == NULL ||
            action->observation != CFLOW_MACHINE_ACTION_VALUE);
}

static void machine_mailbox_wake(void *user) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)user;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    impl->mailbox_armed = false;
    cmeta_mutex_unlock(&impl->lock);
    (void)schedule_machine_task(impl);
}

static bool arm_mailbox(cflow_machine_instance_impl *impl) {
    cflow_waker waker = {machine_mailbox_wake, impl};
    cmeta_mutex_lock(&impl->lock);
    if (impl->done || impl->error != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return true;
    }
    impl->mailbox_armed = true;
    cmeta_mutex_unlock(&impl->lock);
    if (!cflow_waitable_arm(&impl->mailbox_waitable, waker)) {
        cmeta_mutex_lock(&impl->lock);
        impl->mailbox_armed = false;
        cmeta_mutex_unlock(&impl->lock);
        fail_runtime(impl, "machine mailbox waitable arm failed", false);
        return false;
    }
    return true;
}

static void machine_executor_task(void *user) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)user;
    unsigned iteration;
    bool repost = false;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    impl->rerun = false;
    cmeta_mutex_unlock(&impl->lock);

    for (iteration = 0u; iteration < CFLOW_MACHINE_INSTANCE_QUANTUM;
         ++iteration) {
        cflow_event_id event_id = 0u;
        const cmeta_type_desc *event_type = NULL;
        cflow_mailbox_status status;
        bool terminal;
        cmeta_mutex_lock(&impl->lock);
        terminal = impl->done || impl->error != NULL ||
                   impl->ready != CFLOW_MACHINE_READY_NONE;
        if (!terminal)
            impl->worker_phase = CFLOW_MACHINE_WORKER_EXECUTING;
        cmeta_mutex_unlock(&impl->lock);
        if (terminal) break;
        status = cflow_mailbox_try_receive(
            &impl->mailbox, &event_id, &event_type,
            impl->event_value, impl->event_capacity);
        if (status == CFLOW_MAILBOX_EMPTY) {
            bool control_requested;
            cmeta_mutex_lock(&impl->lock);
            impl->worker_phase = CFLOW_MACHINE_WORKER_SCHEDULED;
            control_requested =
                impl->lifecycle != CFLOW_MACHINE_CONTROL_OPEN;
            cmeta_mutex_unlock(&impl->lock);
            if (control_requested)
                publish_done(impl, CFLOW_MACHINE_READY_DONE);
            else
                (void)arm_mailbox(impl);
            break;
        }
        if (status == CFLOW_MAILBOX_CLOSED ||
            status == CFLOW_MAILBOX_CANCELLED) {
            publish_done(impl, CFLOW_MACHINE_READY_DONE);
            break;
        }
        if (status != CFLOW_MAILBOX_OK) {
            fail_runtime(impl, "machine mailbox receive failed", false);
            break;
        }
        cmeta_mutex_lock(&impl->lock);
        ++impl->in_flight;
        cmeta_mutex_unlock(&impl->lock);
        if (!process_event(impl, event_id, impl->event_value)) break;
    }

    cmeta_mutex_lock(&impl->lock);
    if (impl->worker_phase == CFLOW_MACHINE_WORKER_SCHEDULED)
        impl->worker_phase = CFLOW_MACHINE_WORKER_IDLE;
    repost = impl->rerun && !impl->done && impl->error == NULL &&
             impl->ready == CFLOW_MACHINE_READY_NONE;
    if (iteration == CFLOW_MACHINE_INSTANCE_QUANTUM &&
        !impl->done && impl->error == NULL &&
        impl->ready == CFLOW_MACHINE_READY_NONE)
        repost = true;
    cmeta_mutex_unlock(&impl->lock);
    if (repost) (void)schedule_machine_task(impl);
}

static bool schedule_machine_task(cflow_machine_instance_impl *impl) {
    cflow_admission_status status;
    if (impl == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    if (impl->done || impl->error != NULL ||
        impl->ready != CFLOW_MACHINE_READY_NONE) {
        cmeta_mutex_unlock(&impl->lock);
        return true;
    }
    if (impl->worker_phase == CFLOW_MACHINE_WORKER_EXECUTING ||
        impl->worker_phase == CFLOW_MACHINE_WORKER_COMMITTING) {
        impl->rerun = true;
        cmeta_mutex_unlock(&impl->lock);
        return true;
    }
    if (impl->worker_phase == CFLOW_MACHINE_WORKER_SCHEDULED) {
        impl->rerun = true;
        cmeta_mutex_unlock(&impl->lock);
        return true;
    }
    impl->worker_phase = CFLOW_MACHINE_WORKER_SCHEDULED;
    cmeta_mutex_unlock(&impl->lock);

    status = cflow_executor_try_post(
        impl->executor, machine_executor_task, impl);
    if (status != CFLOW_ADMISSION_ACCEPTED) {
        cmeta_mutex_lock(&impl->lock);
        impl->worker_phase = CFLOW_MACHINE_WORKER_IDLE;
        cmeta_mutex_unlock(&impl->lock);
        fail_runtime(impl,
                     status == CFLOW_ADMISSION_FULL
                         ? "machine SerialExecutor is full"
                         : "machine SerialExecutor is closed",
                     false);
        return false;
    }
    return true;
}

static bool machine_wait_arm(void *state, cflow_waker waker) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    bool ready;
    if (impl == NULL || waker.wake == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    ready = impl->ready != CFLOW_MACHINE_READY_NONE || impl->done ||
            impl->error != NULL;
    if (!ready && impl->downstream_waiter.wake != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    if (!ready) impl->downstream_waiter = waker;
    cmeta_mutex_unlock(&impl->lock);
    if (ready) invoke_waker(waker);
    return true;
}

static void machine_wait_cancel(void *state) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    impl->downstream_waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&impl->lock);
}

CMETA_IMPLEMENTS(cflow_waitable, cflow_machine_waitable, 0,
    .arm = machine_wait_arm,
    .cancel = machine_wait_cancel
);

static cflow_step machine_resume(void *state,
                                 cflow_publish_context *context,
                                 void *out_value) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    cflow_machine_ready_kind ready;
    const char *error;
    (void)context;
    if (impl == NULL || out_value == NULL)
        return (cflow_step){
            CFLOW_STEP_ERROR, {0}, "machine resumable is invalid"};
    cmeta_mutex_lock(&impl->lock);
    ready = impl->ready;
    error = impl->error;
    if (ready == CFLOW_MACHINE_READY_VALUE ||
        ready == CFLOW_MACHINE_READY_VALUE_AND_DONE) {
        memcpy(out_value, impl->observation_value, impl->output_type->size);
        impl->ready = ready == CFLOW_MACHINE_READY_VALUE
            ? CFLOW_MACHINE_READY_NONE : ready;
        cmeta_mutex_unlock(&impl->lock);
        return (cflow_step){
            ready == CFLOW_MACHINE_READY_VALUE
                ? CFLOW_STEP_VALUE : CFLOW_STEP_VALUE_AND_DONE,
            {0}, NULL};
    }
    if (ready == CFLOW_MACHINE_READY_ERROR || error != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return (cflow_step){CFLOW_STEP_ERROR, {0}, error};
    }
    if (ready == CFLOW_MACHINE_READY_DONE || impl->done) {
        cmeta_mutex_unlock(&impl->lock);
        return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    }
    cmeta_mutex_unlock(&impl->lock);
    if (!schedule_machine_task(impl)) {
        return (cflow_step){CFLOW_STEP_ERROR, {0}, impl->error};
    }
    return (cflow_step){
        CFLOW_STEP_WAIT,
        cflow_machine_waitable_as_cflow_waitable(impl),
        NULL};
}

static void request_cancel(cflow_machine_instance_impl *impl) {
    cflow_waker waker = {0};
    cflow_waker terminal_waker = {0};
    bool mailbox_armed;
    bool settle_now;
    if (impl == NULL) return;
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, true);
    cmeta_mutex_lock(&impl->lock);
    if (impl->done ||
        impl->lifecycle == CFLOW_MACHINE_CONTROL_TERMINAL) {
        cmeta_mutex_unlock(&impl->lock);
        if (impl->commit_hook != NULL)
            impl->commit_hook(impl->commit_user, SIZE_MAX, false);
        return;
    }
    mailbox_armed = impl->mailbox_armed;
    impl->mailbox_armed = false;
    impl->closed = true;
    impl->cancelled = true;
    impl->lifecycle = CFLOW_MACHINE_CONTROL_CANCEL_REQUESTED;
    settle_now = impl->worker_phase != CFLOW_MACHINE_WORKER_EXECUTING &&
                 impl->worker_phase != CFLOW_MACHINE_WORKER_COMMITTING;
    if (settle_now) {
        publish_control_terminal_locked(impl);
        waker = take_downstream_waiter_locked(impl);
        terminal_waker = take_terminal_waiter_locked(impl);
    }
    cmeta_mutex_unlock(&impl->lock);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, false);
    if (mailbox_armed) cflow_waitable_cancel(&impl->mailbox_waitable);
    cancel_mailbox(impl);
    invoke_waker(waker);
    invoke_waker(terminal_waker);
}

static void machine_cancel(void *state) {
    request_cancel((cflow_machine_instance_impl *)state);
}

static void machine_detach(void *state) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    if (impl == NULL) return;
    machine_cancel(impl);
    cmeta_mutex_lock(&impl->lock);
    impl->adapter_attached = false;
    cmeta_mutex_unlock(&impl->lock);
}

static const cflow_resumable_ops machine_resumable_ops = {
    machine_resume,
    machine_cancel,
    machine_detach
};

static const char *machine_source_name(void *state) {
    (void)state;
    return "machine";
}

static const cmeta_type_desc *machine_source_type(void *state) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    return impl != NULL ? impl->output_type : NULL;
}

static void machine_source_bind_terminal(void *state, cflow_waker waker) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    bool terminal;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    terminal = impl->done || impl->error != NULL;
    impl->terminal_waiter = terminal ? (cflow_waker){0} : waker;
    cmeta_mutex_unlock(&impl->lock);
    if (terminal) invoke_waker(waker);
}

static cflow_publisher_terminal machine_source_poll_terminal(
    void *state, const char **error) {
    cflow_machine_instance_impl *impl =
        (cflow_machine_instance_impl *)state;
    cflow_publisher_terminal result = CFLOW_PUBLISHER_OPEN;
    if (error != NULL) *error = NULL;
    if (impl == NULL) {
        if (error != NULL) *error = "machine source is invalid";
        return CFLOW_PUBLISHER_ERROR;
    }
    cmeta_mutex_lock(&impl->lock);
    if (impl->error != NULL) {
        result = CFLOW_PUBLISHER_ERROR;
        if (error != NULL) *error = impl->error;
    } else if (impl->done) {
        result = CFLOW_PUBLISHER_DONE;
    }
    cmeta_mutex_unlock(&impl->lock);
    return result;
}

CMETA_IMPLEMENTS(cflow_publisher, cflow_machine_source, 0,
    .name = machine_source_name,
    .output_type = machine_source_type,
    .resume = machine_resume,
    .cancel = machine_cancel,
    .destroy = machine_detach,
    .bind_terminal_waker = machine_source_bind_terminal,
    .poll_terminal = machine_source_poll_terminal
);

static cflow_machine_instance_status copy_and_validate_bindings(
    cflow_machine_instance_impl *impl,
    const cflow_machine_instance_config *config) {
    const size_t guard_count = cflow_machine_guard_count(config->machine);
    const size_t action_count = cflow_machine_action_count(config->machine);
    size_t index;

    if (config->guard_count != guard_count ||
        config->action_count != action_count ||
        (guard_count != 0u && config->guards == NULL) ||
        (action_count != 0u && config->actions == NULL))
        return CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH;

    if (guard_count != 0u) {
        if (guard_count > SIZE_MAX / sizeof(*impl->guards))
            return CFLOW_MACHINE_INSTANCE_INVALID_ARGUMENT;
        impl->guards = (cflow_machine_guard_binding *)malloc(
            guard_count * sizeof(*impl->guards));
        if (impl->guards == NULL)
            return CFLOW_MACHINE_INSTANCE_ALLOCATION_FAILED;
        memcpy(impl->guards, config->guards,
               guard_count * sizeof(*impl->guards));
        qsort(impl->guards, guard_count, sizeof(*impl->guards),
              compare_guard_binding);
        for (index = 0u; index < guard_count; ++index) {
            const cflow_machine_guard *declaration =
                cflow_machine_guard_at(config->machine, index);
            if (impl->guards[index].id != declaration->id ||
                impl->guards[index].fn == NULL)
                return CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH;
        }
    }

    if (action_count != 0u) {
        if (action_count > SIZE_MAX / sizeof(*impl->actions))
            return CFLOW_MACHINE_INSTANCE_INVALID_ARGUMENT;
        impl->actions = (cflow_machine_action_binding *)malloc(
            action_count * sizeof(*impl->actions));
        if (impl->actions == NULL)
            return CFLOW_MACHINE_INSTANCE_ALLOCATION_FAILED;
        memcpy(impl->actions, config->actions,
               action_count * sizeof(*impl->actions));
        qsort(impl->actions, action_count, sizeof(*impl->actions),
              compare_action_binding);
        for (index = 0u; index < action_count; ++index) {
            const cflow_machine_action *declaration =
                cflow_machine_action_at(config->machine, index);
            if (impl->actions[index].id != declaration->id ||
                impl->actions[index].fn == NULL)
                return CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH;
        }
    }

    impl->guard_count = guard_count;
    impl->action_count = action_count;
    return CFLOW_MACHINE_INSTANCE_OK;
}

static cflow_machine_instance_status measure_supported_types(
    cflow_machine_instance_impl *impl,
    const cflow_machine_instance_config *config) {
    size_t index;
    size_t state_capacity = 0u;
    size_t observation_capacity = config->output_type->size;

    if (!runtime_type_supported(config->output_type))
        return CFLOW_MACHINE_INSTANCE_UNSUPPORTED_TYPE;

    for (index = 0u; index < cflow_machine_state_count(config->machine);
         ++index) {
        const cflow_machine_state *state =
            cflow_machine_state_at(config->machine, index);
        if (!runtime_type_supported(state->value_type))
            return CFLOW_MACHINE_INSTANCE_UNSUPPORTED_TYPE;
        if (state->value_type->size > state_capacity)
            state_capacity = state->value_type->size;
    }

    for (index = 0u; index < cflow_machine_action_count(config->machine);
         ++index) {
        const cflow_machine_action *action =
            cflow_machine_action_at(config->machine, index);
        if (action->observation == CFLOW_MACHINE_ACTION_VALUE) {
            if (!cmeta_type_equal(action->output_type, config->output_type))
                return CFLOW_MACHINE_INSTANCE_TYPE_MISMATCH;
        }
        if (action->observation != CFLOW_MACHINE_ACTION_NONE) {
            if (!runtime_type_supported(action->output_type))
                return CFLOW_MACHINE_INSTANCE_UNSUPPORTED_TYPE;
            if (action->output_type->size > observation_capacity)
                observation_capacity = action->output_type->size;
        }
    }

    impl->state_capacity = state_capacity;
    impl->observation_capacity = observation_capacity;
    return CFLOW_MACHINE_INSTANCE_OK;
}

static cflow_machine_instance_status initialize_mailbox(
    cflow_machine_instance_impl *impl,
    const cflow_machine_instance_config *config) {
    const size_t event_count = cflow_machine_event_count(config->machine);
    cflow_event_type *schema;
    size_t index;
    cflow_mailbox_status status;

    if (event_count == 0u) return CFLOW_MACHINE_INSTANCE_OK;
    if (event_count > SIZE_MAX / sizeof(*schema))
        return CFLOW_MACHINE_INSTANCE_INVALID_ARGUMENT;
    schema = (cflow_event_type *)malloc(event_count * sizeof(*schema));
    if (schema == NULL) return CFLOW_MACHINE_INSTANCE_ALLOCATION_FAILED;
    for (index = 0u; index < event_count; ++index)
        schema[index] = *cflow_machine_event_at(config->machine, index);
    status = cflow_mailbox_init(&impl->mailbox, schema, event_count,
                                config->mailbox_capacity);
    free(schema);
    if (status == CFLOW_MAILBOX_ALLOCATION_FAILED)
        return CFLOW_MACHINE_INSTANCE_ALLOCATION_FAILED;
    if (status != CFLOW_MAILBOX_OK)
        return CFLOW_MACHINE_INSTANCE_UNSUPPORTED_TYPE;
    impl->mailbox_initialized = true;
    impl->event_capacity = cflow_mailbox_payload_capacity(&impl->mailbox);
    return CFLOW_MACHINE_INSTANCE_OK;
}

cflow_machine_instance_status cflow_machine_instance_init_internal(
    cflow_machine_instance *instance,
    const cflow_machine_instance_config *config,
    cflow_machine_transition_commit_hook commit_hook,
    void *commit_user,
    cflow_machine_commit_boundary_hook boundary_hook,
    void *boundary_user) {
    cflow_machine_instance_impl *impl;
    cflow_machine_instance_status status;
    const cflow_machine_state *initial;

    if (instance == NULL || config == NULL || instance->impl != NULL ||
        config->machine == NULL || config->machine->impl == NULL ||
        config->initial_state == NULL || config->output_type == NULL ||
        config->executor == NULL || config->mailbox_capacity == 0u)
        return CFLOW_MACHINE_INSTANCE_INVALID_ARGUMENT;
    if (!cflow_executor_valid(config->executor) ||
        !cflow_executor_has(config->executor, CMETA_EXEC_CAP_SERIAL) ||
        (cflow_executor_has(config->executor, CMETA_EXEC_CAP_MANUAL) &&
         !cflow_executor_has(config->executor, CMETA_EXEC_CAP_OWNER_AFFINE)) ||
        (cflow_executor_has(config->executor, CMETA_EXEC_CAP_OWNER_AFFINE) &&
         !cflow_executor_owner_is_thread_internal(config->executor)))
        return CFLOW_MACHINE_INSTANCE_INVALID_EXECUTOR;

    initial = find_state(config->machine,
                         cflow_machine_initial_state(config->machine));
    if (initial == NULL) return CFLOW_MACHINE_INSTANCE_INVALID_ARGUMENT;

    impl = (cflow_machine_instance_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return CFLOW_MACHINE_INSTANCE_ALLOCATION_FAILED;
    impl->machine = config->machine;
    impl->executor = config->executor;
    impl->output_type = config->output_type;
    impl->state = initial;
    impl->commit_hook = commit_hook;
    impl->commit_user = commit_user;
    impl->boundary_hook = boundary_hook;
    impl->boundary_user = boundary_user;

    status = copy_and_validate_bindings(impl, config);
    if (status != CFLOW_MACHINE_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }
    status = measure_supported_types(impl, config);
    if (status != CFLOW_MACHINE_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }
    status = initialize_mailbox(impl, config);
    if (status != CFLOW_MACHINE_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }

    impl->state_value = (unsigned char *)malloc(impl->state_capacity);
    impl->target_value = (unsigned char *)malloc(impl->state_capacity);
    impl->event_value = (unsigned char *)malloc(
        impl->event_capacity != 0u ? impl->event_capacity : 1u);
    impl->observation_value = (unsigned char *)malloc(
        impl->observation_capacity);
    cmeta_mutex_init(&impl->lock);
    if (impl->state_value == NULL || impl->target_value == NULL ||
        impl->event_value == NULL || impl->observation_value == NULL ||
        impl->lock == NULL) {
        instance_impl_free(impl);
        return CFLOW_MACHINE_INSTANCE_ALLOCATION_FAILED;
    }
    memcpy(impl->state_value, config->initial_state,
           initial->value_type->size);
    impl->done = initial->kind != CFLOW_MACHINE_STATE_ACTIVE;
    if (impl->done)
        impl->lifecycle = CFLOW_MACHINE_CONTROL_TERMINAL;
    if (initial->kind == CFLOW_MACHINE_STATE_DONE)
        impl->ready = CFLOW_MACHINE_READY_DONE;
    else if (initial->kind == CFLOW_MACHINE_STATE_ERROR) {
        impl->error = runtime_copy_error("initial error state");
        impl->error_owned = impl->error != NULL;
        if (impl->error == NULL)
            impl->error = "machine runtime could not preserve error text";
        impl->ready = CFLOW_MACHINE_READY_ERROR;
    }
    if (impl->mailbox_initialized)
        impl->mailbox_waitable = cflow_mailbox_as_waitable(&impl->mailbox);
    instance->impl = impl;
    if (impl->done) cancel_mailbox(impl);
    return CFLOW_MACHINE_INSTANCE_OK;
}

cflow_machine_instance_status cflow_machine_instance_init(
    cflow_machine_instance *instance,
    const cflow_machine_instance_config *config) {
    return cflow_machine_instance_init_internal(
        instance, config, NULL, NULL, NULL, NULL);
}

cflow_mailbox_status cflow_machine_instance_try_send(
    cflow_machine_instance *instance,
    const cflow_event_view *event) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    if (impl == NULL || !impl->mailbox_initialized)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    return cflow_mailbox_try_send(&impl->mailbox, event);
}

cflow_mailbox_status cflow_machine_instance_try_send_detach_internal(
    cflow_machine_instance *instance, const cflow_event_view *event,
    cflow_waker *out_waker) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    if (out_waker != NULL) *out_waker = (cflow_waker){0};
    if (impl == NULL || !impl->mailbox_initialized)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    return cflow_mailbox_try_send_detach_internal(&impl->mailbox, event, out_waker);
}

bool cflow_machine_instance_timer_payload_capacity(
    const cflow_machine_instance *instance,
    size_t *out_capacity) {
    const cflow_machine_instance_impl *impl = instance != NULL
        ? (const cflow_machine_instance_impl *)instance->impl : NULL;
    if (impl == NULL || !impl->mailbox_initialized || out_capacity == NULL ||
        impl->event_capacity == 0u)
        return false;
    *out_capacity = impl->event_capacity;
    return true;
}

cflow_mailbox_status cflow_machine_instance_timer_event_contract(
    const cflow_machine_instance *instance,
    const cflow_event_view *event,
    const cmeta_type_desc **out_canonical_type) {
    const cflow_machine_instance_impl *impl = instance != NULL
        ? (const cflow_machine_instance_impl *)instance->impl : NULL;
    size_t left = 0u;
    size_t right;
    if (out_canonical_type != NULL) *out_canonical_type = NULL;
    if (impl == NULL || event == NULL || event->id == 0u ||
        event->payload_type == NULL || event->payload == NULL ||
        out_canonical_type == NULL)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    right = cflow_machine_event_count(impl->machine);
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_event_type *expected =
            cflow_machine_event_at(impl->machine, middle);
        if (expected->id == event->id) {
            if (!cmeta_type_equal(expected->payload_type,
                                  event->payload_type))
                return CFLOW_MAILBOX_TYPE_MISMATCH;
            *out_canonical_type = expected->payload_type;
            return CFLOW_MAILBOX_OK;
        }
        if (expected->id < event->id)
            left = middle + 1u;
        else
            right = middle;
    }
    return CFLOW_MAILBOX_INVALID_ARGUMENT;
}

bool cflow_machine_instance_as_resumable(
    cflow_machine_instance *instance,
    cflow_resumable *out) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    if (impl == NULL || out == NULL || out->ops != NULL ||
        out->state != NULL)
        return false;
    cmeta_mutex_lock(&impl->lock);
    if (impl->adapter_attached) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    impl->adapter_attached = true;
    cmeta_mutex_unlock(&impl->lock);
    *out = (cflow_resumable){
        "machine", impl->output_type, &machine_resumable_ops, impl};
    return true;
}

bool cflow_machine_instance_as_publisher(
    cflow_machine_instance *instance,
    cflow_publisher *out) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    if (impl == NULL || out == NULL || cflow_publisher_valid(out)) return false;
    cmeta_mutex_lock(&impl->lock);
    if (impl->adapter_attached) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    impl->adapter_attached = true;
    cmeta_mutex_unlock(&impl->lock);
    *out = cflow_machine_source_as_cflow_publisher(impl);
    return true;
}

void cflow_machine_instance_close(cflow_machine_instance *instance) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    cflow_waker waker = {0};
    cflow_waker terminal_waker = {0};
    bool mailbox_armed;
    bool terminal;
    if (impl == NULL) return;
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, true);
    cmeta_mutex_lock(&impl->lock);
    impl->closed = true;
    if (impl->lifecycle == CFLOW_MACHINE_CONTROL_OPEN)
        impl->lifecycle = CFLOW_MACHINE_CONTROL_CLOSE_REQUESTED;
    mailbox_armed = impl->mailbox_armed;
    impl->mailbox_armed = false;
    terminal = impl->done || impl->error != NULL ||
               (impl->worker_phase != CFLOW_MACHINE_WORKER_EXECUTING &&
                impl->worker_phase != CFLOW_MACHINE_WORKER_COMMITTING);
    if (terminal && impl->error == NULL) {
        publish_control_terminal_locked(impl);
    }
    if (terminal) {
        waker = take_downstream_waiter_locked(impl);
        terminal_waker = take_terminal_waiter_locked(impl);
    }
    cmeta_mutex_unlock(&impl->lock);
    if (impl->commit_hook != NULL)
        impl->commit_hook(impl->commit_user, SIZE_MAX, false);
    if (mailbox_armed) cflow_waitable_cancel(&impl->mailbox_waitable);
    cancel_mailbox(impl);
    invoke_waker(waker);
    invoke_waker(terminal_waker);
}

void cflow_machine_instance_cancel(cflow_machine_instance *instance) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    request_cancel(impl);
}

bool cflow_machine_instance_copy_state(
    const cflow_machine_instance *instance,
    const cmeta_type_desc **out_type,
    void *out_value,
    size_t out_value_capacity) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    if (out_type != NULL) *out_type = NULL;
    if (impl == NULL || out_type == NULL || out_value == NULL)
        return false;
    cmeta_mutex_lock(&impl->lock);
    if (out_value_capacity < impl->state->value_type->size) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    memcpy(out_value, impl->state_value, impl->state->value_type->size);
    *out_type = impl->state->value_type;
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

cflow_machine_state_id cflow_machine_instance_current_state(
    const cflow_machine_instance *instance) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    cflow_machine_state_id result = 0u;
    if (impl == NULL) return 0u;
    cmeta_mutex_lock(&impl->lock);
    result = impl->state->id;
    cmeta_mutex_unlock(&impl->lock);
    return result;
}

typedef struct machine_stats_observer {
    cflow_machine_instance_impl *impl;
    cflow_machine_instance_stats snapshot;
} machine_stats_observer;

static void machine_observe_stats(void *user, const cflow_mailbox_stats *mailbox_stats) {
    machine_stats_observer *observer = (machine_stats_observer *)user;
    cflow_machine_instance_impl *impl = observer->impl;
    cflow_machine_instance_stats *snapshot = &observer->snapshot;
    cmeta_mutex_lock(&impl->lock);
    snapshot->accepted = mailbox_stats->accepted;
    snapshot->completed = impl->completed;
    snapshot->failed = impl->failed;
    snapshot->cancelled_events = mailbox_stats->cancelled +
                                impl->cancelled_in_flight;
    snapshot->emitted_values = impl->emitted_values;
    snapshot->emitted_events = impl->emitted_events;
    snapshot->pending = mailbox_stats->pending;
    /* Dequeue and terminal accounting are frozen by mailbox -> instance lock
     * order. Derive the public count so the interval before the worker records
     * its current event cannot disappear from this snapshot. Receive itself
     * needs no nested locks. Saturated counters no longer encode a difference;
     * retain the existing live count in that case. */
    snapshot->in_flight = mailbox_stats->received == UINT64_MAX
        ? impl->in_flight
        : (size_t)(mailbox_stats->received - impl->completed - impl->failed -
                   impl->cancelled_in_flight);
    snapshot->current_state = impl->state->id;
    snapshot->closed = impl->closed;
    snapshot->cancelled = impl->cancelled;
    snapshot->done = impl->done;
    snapshot->errored = impl->error != NULL;
    cmeta_mutex_unlock(&impl->lock);
}

bool cflow_machine_instance_get_stats(
    const cflow_machine_instance *instance,
    cflow_machine_instance_stats *out) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    machine_stats_observer observer = {impl, {0}};
    cflow_mailbox_stats mailbox_stats = {0};
    if (impl == NULL || out == NULL) return false;
    if (impl->mailbox_initialized) {
        if (!cflow_mailbox_get_stats_observe_internal(
                &impl->mailbox, &mailbox_stats, machine_observe_stats, &observer))
            return false;
    } else {
        machine_observe_stats(&observer, &mailbox_stats);
    }
    *out = observer.snapshot;
    return true;
}

const char *cflow_machine_instance_error(
    const cflow_machine_instance *instance) {
    cflow_machine_instance_impl *impl = instance != NULL
        ? (cflow_machine_instance_impl *)instance->impl : NULL;
    const char *error = NULL;
    if (impl == NULL) return NULL;
    cmeta_mutex_lock(&impl->lock);
    error = impl->error;
    cmeta_mutex_unlock(&impl->lock);
    return error;
}

void cflow_machine_instance_destroy(cflow_machine_instance *instance) {
    cflow_machine_instance_impl *impl;
    bool owner_bound;
    bool mailbox_armed;
    if (instance == NULL) return;
    impl = (cflow_machine_instance_impl *)instance->impl;
    if (impl == NULL) return;
    owner_bound = cflow_executor_has(impl->executor, CMETA_EXEC_CAP_OWNER_AFFINE);

    /* A host-owner Executor has no worker. Waiting for its outstanding tasks
     * and then freeing the Machine would leave borrowed task state dangling.
     * Only the captured owner thread may cooperatively settle these tasks.
     * Callback/wrong-owner destroy is a caller violation. */
    if (owner_bound &&
        (!cflow_executor_owner_is_thread_internal(impl->executor) ||
         cflow_executor_is_current_internal(impl->executor))) {
        assert(!"owner-bound Machine destroy requires an idle owner caller");
        abort(); /* a void destroy cannot safely report partial teardown */
    }

    cmeta_mutex_lock(&impl->lock);
    mailbox_armed = impl->mailbox_armed;
    impl->mailbox_armed = false;
    cmeta_mutex_unlock(&impl->lock);
    if (mailbox_armed) cflow_waitable_cancel(&impl->mailbox_waitable);

    if (owner_bound) {
        /* Host must quiesce new producers before destroying Machine state. */
        while (cflow_executor_pending(impl->executor) != 0u) {
            if (!cflow_executor_run_one(impl->executor)) {
                assert(!"owner-bound Machine has unprogressable tasks");
                abort(); /* never free a queued task's borrowed Machine state */
            }
        }
    } else {
        (void)cflow_executor_wait_idle(impl->executor);
    }
    instance->impl = NULL;
    instance_impl_free(impl);
}
