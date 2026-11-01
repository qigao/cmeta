#include <cflow/actor.h>

#include <cflow/lower.h>

#include "machine_instance_internal.h"
#include "statechart_instance_internal.h"

#include <thread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct cflow_actor_impl cflow_actor_impl;

typedef enum cflow_actor_runtime_kind {
    CFLOW_ACTOR_RUNTIME_MACHINE = 0,
    CFLOW_ACTOR_RUNTIME_STATECHART
} cflow_actor_runtime_kind;

typedef struct cflow_actor_runtime_ops {
    bool (*as_source)(cflow_actor_impl *impl, cflow_publisher *out);
    cflow_mailbox_status (*try_send)(
        cflow_actor_impl *impl, const cflow_event_view *event, cflow_waker *wake);
    void (*close)(cflow_actor_impl *impl);
    void (*cancel)(cflow_actor_impl *impl);
    void (*destroy)(cflow_actor_impl *impl);
    const char *attach_error;
    bool natural_done_is_success;
} cflow_actor_runtime_ops;

struct cflow_actor_impl {
    atomic_size_t refs;
    cmeta_mutex_t gate;
    cmeta_cond_t changed;
    cflow_actor_state state;
    /* FAILED stays externally visible while waiters remain behind settlement. */
    bool terminal_settled;
    bool stale;
    bool starting;
    /* Only detached progress wakes need a lifetime reservation. Ordinary
     * commits stay under gate and require no second lock/unlock pair. */
    size_t pending_wakes;
    cflow_actor_runtime_kind runtime_kind;
    const cflow_actor_runtime_ops *runtime_ops;
    cflow_machine_instance machine;
    cflow_statechart_instance statechart;
    cflow_graph graph;
    cflow_subscription run;
    cflow_scheduler *scheduler;
    cflow_subscriber_callbacks callbacks;
    cflow_subscriber_callbacks bridge_callbacks;
    char *error;
    uint64_t rejected_not_started;
    uint64_t rejected_stopping;
    uint64_t rejected_stopped;
    uint64_t rejected_failed;
    uint64_t rejected_stale;
};

static const char actor_generic_failure[] = "actor failed";
static const char actor_unexpected_done[] =
    "Run completed while Actor was RUNNING";

static bool actor_machine_as_source(
    cflow_actor_impl *impl, cflow_publisher *out) {
    return cflow_machine_instance_as_publisher(&impl->machine, out);
}

static cflow_mailbox_status actor_machine_try_send(
    cflow_actor_impl *impl, const cflow_event_view *event, cflow_waker *wake) {
    return cflow_machine_instance_try_send_detach_internal(&impl->machine, event, wake);
}

static void actor_machine_close(cflow_actor_impl *impl) {
    cflow_machine_instance_close(&impl->machine);
}

static void actor_machine_cancel(cflow_actor_impl *impl) {
    cflow_machine_instance_cancel(&impl->machine);
}

static void actor_machine_destroy(cflow_actor_impl *impl) {
    cflow_machine_instance_destroy(&impl->machine);
}

static const cflow_actor_runtime_ops actor_machine_instance_ops = {
    actor_machine_as_source,
    actor_machine_try_send,
    actor_machine_close,
    actor_machine_cancel,
    actor_machine_destroy,
    "actor could not attach Machine source",
    false
};

static bool actor_statechart_as_source(
    cflow_actor_impl *impl, cflow_publisher *out) {
    return cflow_statechart_instance_as_terminal_publisher(
        &impl->statechart, out);
}

static cflow_mailbox_status actor_statechart_try_send(
    cflow_actor_impl *impl, const cflow_event_view *event, cflow_waker *wake) {
    return cflow_statechart_instance_try_send_detach_internal(
        &impl->statechart, event, UINT64_C(0), wake);
}

static void actor_statechart_close(cflow_actor_impl *impl) {
    cflow_statechart_instance_close(&impl->statechart);
}

static void actor_statechart_cancel(cflow_actor_impl *impl) {
    cflow_statechart_instance_cancel(&impl->statechart);
}

static void actor_statechart_destroy(cflow_actor_impl *impl) {
    /* Run detachment and the callback restriction make WOULD_BLOCK invalid. */
    (void)cflow_statechart_instance_destroy(&impl->statechart);
}

static const cflow_actor_runtime_ops actor_statechart_instance_ops = {
    actor_statechart_as_source,
    actor_statechart_try_send,
    actor_statechart_close,
    actor_statechart_cancel,
    actor_statechart_destroy,
    "actor could not attach Statechart terminal source",
    true
};

static char *actor_copy_error(const char *message) {
    const char *source = message != NULL && message[0] != '\0'
        ? message : actor_generic_failure;
    const size_t length = strlen(source);
    char *copy;
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1u);
    if (copy != NULL) memcpy(copy, source, length + 1u);
    return copy;
}

static void actor_shell_destroy(cflow_actor_impl *impl) {
    if (impl == NULL) return;
    free(impl->error);
    cmeta_cond_destroy(&impl->changed);
    cmeta_mutex_destroy(&impl->gate);
    free(impl);
}

static void actor_release(cflow_actor_impl *impl) {
    if (impl != NULL &&
        atomic_fetch_sub_explicit(&impl->refs, 1u, memory_order_acq_rel) == 1u)
        actor_shell_destroy(impl);
}

static bool actor_retain(cflow_actor_impl *impl) {
    size_t current;
    if (impl == NULL) return false;
    current = atomic_load_explicit(&impl->refs, memory_order_relaxed);
    while (current != 0u && current != SIZE_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &impl->refs, &current, current + 1u,
                memory_order_relaxed, memory_order_relaxed))
            return true;
    }
    return false;
}

static bool actor_mark_failed(cflow_actor_impl *impl, const char *message,
                              bool defer_callback_settlement) {
    char *copy;
    bool first = false;
    if (impl == NULL) return false;
    copy = actor_copy_error(message);
    cmeta_mutex_lock(&impl->gate);
    impl->starting = false;
    if (impl->state != CFLOW_ACTOR_STATE_STOPPED &&
        impl->state != CFLOW_ACTOR_STATE_FAILED) {
        impl->terminal_settled = false;
        impl->state = CFLOW_ACTOR_STATE_FAILED;
        first = true;
        cmeta_cond_broadcast(&impl->changed);
    }
    if (impl->error == NULL && copy != NULL) {
        impl->error = copy;
        copy = NULL;
    }
    cmeta_mutex_unlock(&impl->gate);
    free(copy);
    if (first) {
        impl->runtime_ops->cancel(impl);
        if (!defer_callback_settlement) {
            cmeta_mutex_lock(&impl->gate);
            impl->terminal_settled = true;
            cmeta_cond_broadcast(&impl->changed);
            cmeta_mutex_unlock(&impl->gate);
        }
    }
    return first;
}

static bool actor_sink_value(void *user,
                             const cmeta_type_desc *type,
                             const void *value) {
    cflow_actor_impl *impl = (cflow_actor_impl *)user;
    cflow_value_fn callback = impl != NULL ? impl->callbacks.on_value : NULL;
    return callback == NULL || callback(impl->callbacks.user, type, value);
}

static void actor_sink_error(void *user, const char *message) {
    cflow_actor_impl *impl = (cflow_actor_impl *)user;
    cflow_error_fn callback;
    void *callback_user;
    bool settle;
    if (impl == NULL) return;
    settle = actor_mark_failed(impl, message, true);
    callback = impl->callbacks.on_error;
    callback_user = impl->callbacks.user;
    if (callback != NULL) callback(callback_user, message);
    if (settle) {
        cmeta_mutex_lock(&impl->gate);
        impl->terminal_settled = true;
        cmeta_cond_broadcast(&impl->changed);
        cmeta_mutex_unlock(&impl->gate);
    }
}

static void actor_sink_done(void *user) {
    cflow_actor_impl *impl = (cflow_actor_impl *)user;
    cflow_done_fn done_callback = NULL;
    cflow_error_fn error_callback = NULL;
    void *callback_user;
    char *copy = NULL;
    const char *error = NULL;
    bool running;
    bool natural_done_is_success;
    bool failed = false;
    bool settle = false;
    if (impl == NULL) return;

    cmeta_mutex_lock(&impl->gate);
    running = impl->state == CFLOW_ACTOR_STATE_RUNNING;
    natural_done_is_success =
        impl->runtime_ops->natural_done_is_success;
    cmeta_mutex_unlock(&impl->gate);
    if (running && !natural_done_is_success)
        copy = actor_copy_error(actor_unexpected_done);

    cmeta_mutex_lock(&impl->gate);
    if (impl->state == CFLOW_ACTOR_STATE_STOPPING) {
        impl->terminal_settled = false;
        impl->state = CFLOW_ACTOR_STATE_STOPPED;
        done_callback = impl->callbacks.on_done;
        settle = true;
    } else if (impl->state == CFLOW_ACTOR_STATE_RUNNING &&
               natural_done_is_success) {
        impl->terminal_settled = false;
        impl->state = CFLOW_ACTOR_STATE_STOPPED;
        done_callback = impl->callbacks.on_done;
        settle = true;
    } else if (impl->state == CFLOW_ACTOR_STATE_RUNNING) {
        if (impl->error == NULL && copy != NULL) {
            impl->error = copy;
            copy = NULL;
        }
        error = impl->error != NULL ? impl->error : actor_generic_failure;
        impl->terminal_settled = false;
        impl->state = CFLOW_ACTOR_STATE_FAILED;
        cmeta_cond_broadcast(&impl->changed);
        error_callback = impl->callbacks.on_error;
        failed = true;
        settle = true;
    }
    callback_user = impl->callbacks.user;
    cmeta_mutex_unlock(&impl->gate);
    free(copy);

    if (failed) impl->runtime_ops->cancel(impl);
    if (error_callback != NULL) error_callback(callback_user, error);
    if (done_callback != NULL) done_callback(callback_user);
    /* Expose terminal settlement only after the user callback returns.
     * cflow_actor_wait cannot release borrowed Scope/DSO callback storage
     * while the worker is still executing on_done/on_error. */
    if (settle) {
        cmeta_mutex_lock(&impl->gate);
        impl->terminal_settled = true;
        cmeta_cond_broadcast(&impl->changed);
        cmeta_mutex_unlock(&impl->gate);
    }
}

static cflow_actor_status actor_state_status(cflow_actor_state state) {
    switch (state) {
        case CFLOW_ACTOR_STATE_RUNNING:
            return CFLOW_ACTOR_ALREADY_STARTED;
        case CFLOW_ACTOR_STATE_STOPPING:
            return CFLOW_ACTOR_STOPPING;
        case CFLOW_ACTOR_STATE_STOPPED:
            return CFLOW_ACTOR_STOPPED;
        case CFLOW_ACTOR_STATE_FAILED:
            return CFLOW_ACTOR_FAILED;
        case CFLOW_ACTOR_STATE_START:
            break;
    }
    return CFLOW_ACTOR_OK;
}

static bool actor_scheduler_is_valid(cflow_scheduler *scheduler) {
    return scheduler != NULL && cflow_scheduler_valid(scheduler) &&
        (cflow_scheduler_capabilities(scheduler) &
         CMETA_SCHED_CAP_CONCURRENT) != 0u;
}

static cflow_actor_impl *actor_shell_create(
    cflow_scheduler *scheduler, cflow_subscriber_callbacks callbacks) {
    cflow_actor_impl *impl =
        (cflow_actor_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return NULL;
    atomic_init(&impl->refs, 1u);
    impl->graph.root = CMETA_INVALID_ID;
    impl->scheduler = scheduler;
    impl->callbacks = callbacks;
    impl->state = CFLOW_ACTOR_STATE_START;
    cmeta_mutex_init(&impl->gate);
    cmeta_cond_init(&impl->changed);
    if (impl->gate == NULL || impl->changed == NULL) {
        actor_shell_destroy(impl);
        return NULL;
    }
    impl->bridge_callbacks = (cflow_subscriber_callbacks){
        actor_sink_value, actor_sink_error, actor_sink_done, impl};
    return impl;
}

static bool actor_identity_graph_init(
    cflow_actor_impl *impl, const cmeta_type_desc *output_type) {
    cflow_graph surface = {0};
    bool normalized;
    surface.root = CMETA_INVALID_ID;
    cflow_graph_init(&surface, output_type);
    normalized = surface.root != CMETA_INVALID_ID &&
        cflow_graph_normalize(&impl->graph, &surface);
    cflow_graph_destroy(&surface);
    return normalized;
}

cflow_actor_init_result cflow_actor_init(
    cflow_actor *actor, const cflow_actor_config *config) {
    cflow_actor_init_result result = {
        CFLOW_ACTOR_INVALID_ARGUMENT, CFLOW_MACHINE_INSTANCE_OK};
    cflow_actor_impl *impl;
    if (actor == NULL || config == NULL || actor->impl != NULL)
        return result;
    if (!actor_scheduler_is_valid(config->scheduler)) {
        result.status = CFLOW_ACTOR_INVALID_SCHEDULER;
        return result;
    }

    impl = actor_shell_create(config->scheduler, config->callbacks);
    if (impl == NULL) {
        result.status = CFLOW_ACTOR_ALLOCATION_FAILED;
        return result;
    }

    result.machine_status = cflow_machine_instance_init(
        &impl->machine, &config->machine);
    if (result.machine_status != CFLOW_MACHINE_INSTANCE_OK) {
        result.status = CFLOW_ACTOR_MACHINE_REJECTED;
        actor_release(impl);
        return result;
    }
    impl->runtime_kind = CFLOW_ACTOR_RUNTIME_MACHINE;
    impl->runtime_ops = &actor_machine_instance_ops;

    if (!actor_identity_graph_init(impl, config->machine.output_type)) {
        impl->runtime_ops->destroy(impl);
        result.status = CFLOW_ACTOR_ALLOCATION_FAILED;
        actor_release(impl);
        return result;
    }
    actor->impl = impl;
    result.status = CFLOW_ACTOR_OK;
    return result;
}

cflow_statechart_actor_init_result cflow_statechart_actor_init(
    cflow_actor *actor, const cflow_statechart_actor_config *config) {
    cflow_statechart_actor_init_result result = {
        CFLOW_ACTOR_INVALID_ARGUMENT, CFLOW_STATECHART_INSTANCE_OK};
    cflow_actor_impl *impl;
    const cmeta_type_desc *state_type;

    if (actor == NULL || config == NULL || actor->impl != NULL)
        return result;
    if (!actor_scheduler_is_valid(config->scheduler)) {
        result.status = CFLOW_ACTOR_INVALID_SCHEDULER;
        return result;
    }
    impl = actor_shell_create(config->scheduler, config->callbacks);
    if (impl == NULL) {
        result.status = CFLOW_ACTOR_ALLOCATION_FAILED;
        return result;
    }
    result.statechart_status = cflow_statechart_instance_init(
        &impl->statechart, &config->statechart);
    if (result.statechart_status != CFLOW_STATECHART_INSTANCE_OK) {
        result.status = CFLOW_ACTOR_STATECHART_REJECTED;
        actor_release(impl);
        return result;
    }
    impl->runtime_kind = CFLOW_ACTOR_RUNTIME_STATECHART;
    impl->runtime_ops = &actor_statechart_instance_ops;
    state_type = cflow_statechart_state_type(config->statechart.statechart);
    if (!actor_identity_graph_init(impl, state_type)) {
        impl->runtime_ops->destroy(impl);
        result.status = CFLOW_ACTOR_ALLOCATION_FAILED;
        actor_release(impl);
        return result;
    }
    actor->impl = impl;
    result.status = CFLOW_ACTOR_OK;
    return result;
}

cflow_actor_status cflow_actor_start(cflow_actor *actor) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    cflow_publisher source = {0};
    cflow_subscriber sink;
    cflow_actor_status status;
    bool opened = false;

    if (impl == NULL) return CFLOW_ACTOR_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->gate);
    status = actor_state_status(impl->state);
    if (status == CFLOW_ACTOR_OK) {
        /* subscribe may deliver a terminal signal before it returns. Publish
         * the receiving lifecycle first, while ingress still reports START. */
        impl->starting = true;
        impl->state = CFLOW_ACTOR_STATE_RUNNING;
    }
    cmeta_mutex_unlock(&impl->gate);
    if (status != CFLOW_ACTOR_OK) return status;

    if (!impl->runtime_ops->as_source(impl, &source)) {
        (void)actor_mark_failed(impl, impl->runtime_ops->attach_error, false);
        return CFLOW_ACTOR_FAILED;
    }
    sink = cflow_subscriber_from_callbacks(&impl->bridge_callbacks);
    opened = cflow_subscribe(&impl->run, &impl->graph, &source,
                            impl->scheduler, &sink);
    if (!opened) {
        if (cflow_publisher_valid(&source)) cflow_publisher_destroy(&source);
        (void)actor_mark_failed(impl, "actor could not open Run", false);
        return CFLOW_ACTOR_FAILED;
    }

    cmeta_mutex_lock(&impl->gate);
    impl->starting = false;
    cmeta_mutex_unlock(&impl->gate);
    if (!cflow_subscription_request(&impl->run, SIZE_MAX)) {
        bool terminal;
        cmeta_mutex_lock(&impl->gate);
        terminal = impl->state == CFLOW_ACTOR_STATE_STOPPED;
        status = impl->state == CFLOW_ACTOR_STATE_FAILED
            ? CFLOW_ACTOR_FAILED : CFLOW_ACTOR_OK;
        cmeta_mutex_unlock(&impl->gate);
        if (!terminal) {
            (void)actor_mark_failed(impl, "actor could not request Run demand", false);
            status = CFLOW_ACTOR_FAILED;
        }
        cflow_subscription_close(&impl->run);
        cmeta_mutex_lock(&impl->gate);
        impl->terminal_settled = true;
        cmeta_cond_broadcast(&impl->changed);
        cmeta_mutex_unlock(&impl->gate);
    } else {
        status = CFLOW_ACTOR_OK;
    }
    return status;
}

cflow_actor_status cflow_actor_request_stop(cflow_actor *actor) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    bool close_runtime = false;
    cflow_actor_status result;
    if (impl == NULL) return CFLOW_ACTOR_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->gate);
    switch (impl->state) {
        case CFLOW_ACTOR_STATE_START:
            impl->state = CFLOW_ACTOR_STATE_STOPPED;
            impl->terminal_settled = true;
            cmeta_cond_broadcast(&impl->changed);
            close_runtime = true;
            result = CFLOW_ACTOR_OK;
            break;
        case CFLOW_ACTOR_STATE_RUNNING:
            impl->state = CFLOW_ACTOR_STATE_STOPPING;
            close_runtime = true;
            result = CFLOW_ACTOR_OK;
            break;
        case CFLOW_ACTOR_STATE_STOPPING:
            result = CFLOW_ACTOR_STOPPING;
            break;
        case CFLOW_ACTOR_STATE_STOPPED:
            result = CFLOW_ACTOR_STOPPED;
            break;
        case CFLOW_ACTOR_STATE_FAILED:
            result = CFLOW_ACTOR_FAILED;
            break;
        default:
            result = CFLOW_ACTOR_FAILED;
            break;
    }
    cmeta_mutex_unlock(&impl->gate);
    if (close_runtime) impl->runtime_ops->close(impl);
    return result;
}

cflow_actor_state cflow_actor_wait(cflow_actor *actor) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    cflow_actor_state state;
    if (impl == NULL) return CFLOW_ACTOR_STATE_FAILED;
    cmeta_mutex_lock(&impl->gate);
    while ((impl->state != CFLOW_ACTOR_STATE_STOPPED &&
            impl->state != CFLOW_ACTOR_STATE_FAILED) ||
           !impl->terminal_settled)
        cmeta_cond_wait(&impl->changed, &impl->gate);
    state = impl->state;
    cmeta_mutex_unlock(&impl->gate);
    return state;
}

cflow_actor_state cflow_actor_current_state(const cflow_actor *actor) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    cflow_actor_state state = CFLOW_ACTOR_STATE_START;
    if (impl == NULL) return state;
    cmeta_mutex_lock(&impl->gate);
    state = impl->state;
    cmeta_mutex_unlock(&impl->gate);
    return state;
}

bool cflow_actor_get_stats(const cflow_actor *actor, cflow_actor_stats *out) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    cflow_actor_stats snapshot = {0};
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->gate);
    if (impl->runtime_kind != CFLOW_ACTOR_RUNTIME_MACHINE) {
        cmeta_mutex_unlock(&impl->gate);
        return false;
    }
    snapshot.state = impl->state;
    snapshot.rejected_not_started = impl->rejected_not_started;
    snapshot.rejected_stopping = impl->rejected_stopping;
    snapshot.rejected_stopped = impl->rejected_stopped;
    snapshot.rejected_failed = impl->rejected_failed;
    snapshot.rejected_stale = impl->rejected_stale;
    if (!cflow_machine_instance_get_stats(&impl->machine,
                                          &snapshot.machine)) {
        cmeta_mutex_unlock(&impl->gate);
        return false;
    }
    cmeta_mutex_unlock(&impl->gate);
    *out = snapshot;
    return true;
}

bool cflow_statechart_actor_get_stats(
    const cflow_actor *actor, cflow_statechart_actor_stats *out) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    cflow_statechart_actor_stats snapshot = {0};
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->gate);
    if (impl->runtime_kind != CFLOW_ACTOR_RUNTIME_STATECHART) {
        cmeta_mutex_unlock(&impl->gate);
        return false;
    }
    snapshot.state = impl->state;
    snapshot.rejected_not_started = impl->rejected_not_started;
    snapshot.rejected_stopping = impl->rejected_stopping;
    snapshot.rejected_stopped = impl->rejected_stopped;
    snapshot.rejected_failed = impl->rejected_failed;
    snapshot.rejected_stale = impl->rejected_stale;
    if (!cflow_statechart_instance_get_stats(
            &impl->statechart, &snapshot.statechart)) {
        cmeta_mutex_unlock(&impl->gate);
        return false;
    }
    cmeta_mutex_unlock(&impl->gate);
    *out = snapshot;
    return true;
}

const char *cflow_actor_error(const cflow_actor *actor) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    const char *error;
    if (impl == NULL) return NULL;
    cmeta_mutex_lock(&impl->gate);
    error = impl->error;
    if (error == NULL && impl->state == CFLOW_ACTOR_STATE_FAILED)
        error = actor_generic_failure;
    cmeta_mutex_unlock(&impl->gate);
    return error;
}

bool cflow_actor_ref_acquire(const cflow_actor *actor, cflow_actor_ref *out) {
    cflow_actor_impl *impl = actor != NULL
        ? (cflow_actor_impl *)actor->impl : NULL;
    bool retained = false;
    if (impl == NULL || out == NULL || out->impl != NULL) return false;
    cmeta_mutex_lock(&impl->gate);
    if (!impl->stale) retained = actor_retain(impl);
    cmeta_mutex_unlock(&impl->gate);
    if (retained) out->impl = impl;
    return retained;
}

bool cflow_actor_ref_retain(const cflow_actor_ref *ref, cflow_actor_ref *out) {
    cflow_actor_impl *impl = ref != NULL
        ? (cflow_actor_impl *)ref->impl : NULL;
    if (impl == NULL || out == NULL || out->impl != NULL ||
        !actor_retain(impl))
        return false;
    out->impl = impl;
    return true;
}

void cflow_actor_ref_release(cflow_actor_ref *ref) {
    cflow_actor_impl *impl;
    if (ref == NULL) return;
    impl = (cflow_actor_impl *)ref->impl;
    ref->impl = NULL;
    actor_release(impl);
}

cflow_actor_send_status cflow_actor_ref_try_send(
    const cflow_actor_ref *ref, const cflow_event_view *event) {
    cflow_actor_impl *impl = ref != NULL
        ? (cflow_actor_impl *)ref->impl : NULL;
    cflow_mailbox_status mailbox_status;
    cflow_actor_send_status result;
    cflow_waker wake = {0};
    if (impl == NULL) return CFLOW_ACTOR_SEND_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->gate);
    if (impl->stale) {
        ++impl->rejected_stale;
        result = CFLOW_ACTOR_SEND_STALE;
    } else if (event == NULL || event->id == 0u ||
               event->payload_type == NULL || event->payload == NULL) {
        result = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
    } else if (impl->state == CFLOW_ACTOR_STATE_START || impl->starting) {
        ++impl->rejected_not_started;
        result = CFLOW_ACTOR_SEND_NOT_STARTED;
    } else if (impl->state == CFLOW_ACTOR_STATE_STOPPING) {
        ++impl->rejected_stopping;
        result = CFLOW_ACTOR_SEND_STOPPING;
    } else if (impl->state == CFLOW_ACTOR_STATE_STOPPED) {
        ++impl->rejected_stopped;
        result = CFLOW_ACTOR_SEND_STOPPED;
    } else if (impl->state == CFLOW_ACTOR_STATE_FAILED) {
        ++impl->rejected_failed;
        result = CFLOW_ACTOR_SEND_FAILED;
    } else {
        /* A finite control block cannot admit more wake reservations than
         * size_t represents. Reject before committing in that impossible-full
         * case rather than wrapping the destruction barrier. */
        if (impl->pending_wakes == SIZE_MAX) {
            cmeta_mutex_unlock(&impl->gate);
            return CFLOW_ACTOR_SEND_FULL;
        }
        mailbox_status = impl->runtime_ops->try_send(impl, event, &wake);
        if (wake.wake != NULL) ++impl->pending_wakes;
        switch (mailbox_status) {
            case CFLOW_MAILBOX_OK:
                result = CFLOW_ACTOR_SEND_ACCEPTED;
                break;
            case CFLOW_MAILBOX_TYPE_MISMATCH:
                result = CFLOW_ACTOR_SEND_TYPE_MISMATCH;
                break;
            case CFLOW_MAILBOX_FULL:
                result = CFLOW_ACTOR_SEND_FULL;
                break;
            case CFLOW_MAILBOX_CLOSED:
            case CFLOW_MAILBOX_CANCELLED:
                ++impl->rejected_failed;
                result = CFLOW_ACTOR_SEND_FAILED;
                break;
            case CFLOW_MAILBOX_INVALID_ARGUMENT:
            case CFLOW_MAILBOX_EMPTY:
            case CFLOW_MAILBOX_ALLOCATION_FAILED:
            case CFLOW_MAILBOX_BUFFER_TOO_SMALL:
            default:
                result = CFLOW_ACTOR_SEND_INVALID_ARGUMENT;
                break;
        }
    }
    cmeta_mutex_unlock(&impl->gate);
    if (wake.wake != NULL) {
        wake.wake(wake.user);
        cmeta_mutex_lock(&impl->gate);
        --impl->pending_wakes;
        if (impl->stale && impl->pending_wakes == 0u)
            cmeta_cond_broadcast(&impl->changed);
        cmeta_mutex_unlock(&impl->gate);
    }
    return result;
}

void cflow_actor_destroy(cflow_actor *actor) {
    cflow_actor_impl *impl;
    if (actor == NULL || actor->impl == NULL) return;
    impl = (cflow_actor_impl *)actor->impl;

    cmeta_mutex_lock(&impl->gate);
    impl->stale = true;
    if (impl->state == CFLOW_ACTOR_STATE_START)
        impl->state = CFLOW_ACTOR_STATE_STOPPED;
    else if (impl->state == CFLOW_ACTOR_STATE_RUNNING)
        impl->state = CFLOW_ACTOR_STATE_STOPPING;
    while (impl->pending_wakes != 0u)
        cmeta_cond_wait(&impl->changed, &impl->gate);
    cmeta_mutex_unlock(&impl->gate);

    impl->runtime_ops->close(impl);
    cflow_subscription_close(&impl->run);
    impl->runtime_ops->destroy(impl);
    cflow_graph_destroy(&impl->graph);

    cmeta_mutex_lock(&impl->gate);
    if (impl->state != CFLOW_ACTOR_STATE_FAILED)
        impl->state = CFLOW_ACTOR_STATE_STOPPED;
    impl->terminal_settled = true;
    cmeta_cond_broadcast(&impl->changed);
    cmeta_mutex_unlock(&impl->gate);
    actor->impl = NULL;
    actor_release(impl);
}
