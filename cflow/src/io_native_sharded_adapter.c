#include <cflow/io_native_sharded_adapter.h>

#include <error_codes.h>
#include <thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct cflow_io_native_sharded_adapter_impl
    cflow_io_native_sharded_adapter_impl;

typedef enum cflow_io_native_sharded_bridge_phase {
    CFLOW_IO_NATIVE_SHARDED_BRIDGE_FREE = 0,
    CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED
} cflow_io_native_sharded_bridge_phase;

typedef struct cflow_io_native_sharded_bridge {
    cflow_io_native_sharded_adapter_impl *owner;
    cflow_io_actor *actor;
    cflow_io_request_id actor_request;
    native_io_sharded_operation *operation;
    native_io_sharded_request native_request;
    uint32_t generation;
    uint32_t next_free;
    cflow_io_native_sharded_bridge_phase phase;
    bool admission_done;
    bool cancel_requested;
    bool cancel_route_active;
    bool io_finalized;
} cflow_io_native_sharded_bridge;

struct cflow_io_native_sharded_adapter_impl {
    native_io_sharded *runtime;
    cflow_io_native_sharded_bridge *bridges;
    cflow_io_actor *bound_actor;
    cmeta_mutex_t lock;
    size_t bridge_capacity;
    size_t active_bridges;
    uint32_t free_head;
    uint64_t accepted_routes;
    uint64_t raw_admissions;
    uint64_t raw_admission_failures;
    uint64_t terminal_completions;
    uint64_t stale_actor_completions;
    uint64_t cancel_routes;
    uint64_t cancel_route_rejections;
    uint64_t native_cancel_errors;
    bool closed;
};

enum { CFLOW_IO_NATIVE_SHARDED_NO_SLOT = UINT32_MAX };

static cflow_io_native_sharded_adapter_impl *
sharded_adapter_impl(cflow_io_native_sharded_adapter *adapter) {
    return adapter != NULL
        ? (cflow_io_native_sharded_adapter_impl *)adapter->impl
        : NULL;
}

static const cflow_io_native_sharded_adapter_impl *
sharded_adapter_const_impl(const cflow_io_native_sharded_adapter *adapter) {
    return adapter != NULL
        ? (const cflow_io_native_sharded_adapter_impl *)adapter->impl
        : NULL;
}

static cflow_io_native_sharded_bridge *
sharded_adapter_reserve_locked(cflow_io_native_sharded_adapter_impl *impl) {
    cflow_io_native_sharded_bridge *bridge;
    const uint32_t index = impl->free_head;

    if (index == CFLOW_IO_NATIVE_SHARDED_NO_SLOT)
        return NULL;
    bridge = &impl->bridges[index];
    impl->free_head = bridge->next_free;
    bridge->next_free = CFLOW_IO_NATIVE_SHARDED_NO_SLOT;
    bridge->phase = CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED;
    bridge->admission_done = false;
    bridge->cancel_requested = false;
    bridge->cancel_route_active = false;
    bridge->io_finalized = false;
    bridge->native_request = (native_io_sharded_request){0};
    ++impl->active_bridges;
    return bridge;
}

static void sharded_adapter_release_locked(
    cflow_io_native_sharded_adapter_impl *impl,
    cflow_io_native_sharded_bridge *bridge) {
    const size_t index = (size_t)(bridge - impl->bridges);

    bridge->actor = NULL;
    bridge->actor_request = 0u;
    bridge->operation = NULL;
    bridge->native_request = (native_io_sharded_request){0};
    bridge->admission_done = false;
    bridge->cancel_requested = false;
    bridge->cancel_route_active = false;
    bridge->io_finalized = false;
    ++bridge->generation;
    if (bridge->generation == 0u)
        ++bridge->generation;
    bridge->phase = CFLOW_IO_NATIVE_SHARDED_BRIDGE_FREE;
    bridge->next_free = impl->free_head;
    impl->free_head = (uint32_t)index;
    --impl->active_bridges;
}

static void sharded_adapter_maybe_release_locked(
    cflow_io_native_sharded_adapter_impl *impl,
    cflow_io_native_sharded_bridge *bridge) {
    if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED &&
        bridge->io_finalized && !bridge->cancel_route_active)
        sharded_adapter_release_locked(impl, bridge);
}

static cflow_io_native_sharded_bridge *
sharded_adapter_find_request_locked(
    cflow_io_native_sharded_adapter_impl *impl,
    cflow_io_request_id request_id) {
    size_t index;

    for (index = 0u; index < impl->bridge_capacity; ++index) {
        cflow_io_native_sharded_bridge *bridge = &impl->bridges[index];
        if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED &&
            bridge->actor_request == request_id)
            return bridge;
    }
    return NULL;
}

static cflow_io_completion sharded_adapter_completion(
    const native_io_sharded_completion *native) {
    cflow_io_completion completion = {
        CFLOW_IO_COMPLETION_FAILED, native->bytes, native->status};

    switch (native->kind) {
    case NATIVE_IO_COMPLETION_OK:
        completion.kind = CFLOW_IO_COMPLETION_OK;
        completion.error = SALTS_OK;
        break;
    case NATIVE_IO_COMPLETION_EOF:
        completion.kind = CFLOW_IO_COMPLETION_EOF;
        completion.error = SALTS_OK;
        break;
    case NATIVE_IO_COMPLETION_CANCELLED:
        completion.kind = CFLOW_IO_COMPLETION_CANCELLED;
        completion.error = SALTS_OK;
        break;
    case NATIVE_IO_COMPLETION_FAILED:
        break;
    default:
        completion.kind = CFLOW_IO_COMPLETION_FAILED;
        completion.error = SALTS_EPROTO;
        break;
    }
    return completion;
}

static void sharded_adapter_record_complete(
    cflow_io_native_sharded_adapter_impl *impl,
    cflow_io_actor *actor,
    cflow_io_request_id request_id,
    const cflow_io_completion *completion) {
    const cflow_io_complete_status completed =
        cflow_io_actor_complete(actor, request_id, completion);

    if (completed != CFLOW_IO_COMPLETE_ACCEPTED) {
        cmeta_mutex_lock(&impl->lock);
        ++impl->stale_actor_completions;
        cmeta_mutex_unlock(&impl->lock);
    }
}

static void sharded_adapter_admission(
    native_io_sharded_context *context,
    int status,
    native_io_sharded_request request,
    void *arg) {
    cflow_io_native_sharded_bridge *bridge =
        (cflow_io_native_sharded_bridge *)arg;
    cflow_io_native_sharded_adapter_impl *impl = bridge->owner;
    cflow_io_actor *actor = NULL;
    cflow_io_request_id actor_request = 0u;
    bool request_cancel = false;

    cmeta_mutex_lock(&impl->lock);
    if (bridge->phase != CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED) {
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    bridge->admission_done = true;
    actor = bridge->actor;
    actor_request = bridge->actor_request;
    if (status == SALTS_OK) {
        bridge->native_request = request;
        ++impl->raw_admissions;
        request_cancel = bridge->cancel_requested;
    } else {
        ++impl->raw_admission_failures;
    }
    cmeta_mutex_unlock(&impl->lock);

    if (status != SALTS_OK) {
        const cflow_io_completion completion = {
            CFLOW_IO_COMPLETION_FAILED, 0u, status};
        sharded_adapter_record_complete(
            impl, actor, actor_request, &completion);
        return;
    }

    if (request_cancel) {
        const int cancel_status =
            native_io_sharded_context_cancel(context, request);
        if (cancel_status != SALTS_OK &&
            cancel_status != SALTS_EALREADY &&
            cancel_status != SALTS_ENOENT) {
            cmeta_mutex_lock(&impl->lock);
            ++impl->native_cancel_errors;
            cmeta_mutex_unlock(&impl->lock);
        }
    }
}

static void sharded_adapter_terminal(
    native_io_sharded_context *context,
    const native_io_sharded_completion *native,
    void *arg) {
    cflow_io_native_sharded_bridge *bridge =
        (cflow_io_native_sharded_bridge *)arg;
    cflow_io_native_sharded_adapter_impl *impl = bridge->owner;
    cflow_io_actor *actor = NULL;
    cflow_io_request_id actor_request = 0u;
    native_io_sharded_operation *operation = NULL;
    cflow_io_completion completion;

    (void)context;
    cmeta_mutex_lock(&impl->lock);
    if (bridge->phase != CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED) {
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    actor = bridge->actor;
    actor_request = bridge->actor_request;
    operation = bridge->operation;
    ++impl->terminal_completions;
    cmeta_mutex_unlock(&impl->lock);

    if (operation != NULL &&
        operation->kind == NATIVE_IO_OPERATION_UDP_RECV_FROM)
        operation->address_length = native->address_length;
    completion = sharded_adapter_completion(native);
    sharded_adapter_record_complete(
        impl, actor, actor_request, &completion);
}

static void sharded_adapter_finalize(void *arg) {
    cflow_io_native_sharded_bridge *bridge =
        (cflow_io_native_sharded_bridge *)arg;
    cflow_io_native_sharded_adapter_impl *impl = bridge->owner;

    cmeta_mutex_lock(&impl->lock);
    if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED) {
        bridge->io_finalized = true;
        sharded_adapter_maybe_release_locked(impl, bridge);
    }
    cmeta_mutex_unlock(&impl->lock);
}

static void sharded_adapter_cancel_run(
    native_io_sharded_context *context, void *arg) {
    cflow_io_native_sharded_bridge *bridge =
        (cflow_io_native_sharded_bridge *)arg;
    cflow_io_native_sharded_adapter_impl *impl = bridge->owner;
    native_io_sharded_request request = {0};
    bool valid = false;

    cmeta_mutex_lock(&impl->lock);
    if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED &&
        bridge->cancel_route_active &&
        native_io_sharded_request_valid(bridge->native_request) &&
        !bridge->io_finalized) {
        request = bridge->native_request;
        valid = true;
    }
    cmeta_mutex_unlock(&impl->lock);

    if (valid) {
        const int status =
            native_io_sharded_context_cancel(context, request);
        if (status != SALTS_OK &&
            status != SALTS_EALREADY &&
            status != SALTS_ENOENT) {
            cmeta_mutex_lock(&impl->lock);
            ++impl->native_cancel_errors;
            cmeta_mutex_unlock(&impl->lock);
        }
    }
}

static void sharded_adapter_cancel_task_cancel(void *arg, int status) {
    cflow_io_native_sharded_bridge *bridge =
        (cflow_io_native_sharded_bridge *)arg;
    cflow_io_native_sharded_adapter_impl *impl = bridge->owner;

    (void)status;
    cmeta_mutex_lock(&impl->lock);
    ++impl->cancel_route_rejections;
    cmeta_mutex_unlock(&impl->lock);
}

static void sharded_adapter_cancel_task_finalize(void *arg) {
    cflow_io_native_sharded_bridge *bridge =
        (cflow_io_native_sharded_bridge *)arg;
    cflow_io_native_sharded_adapter_impl *impl = bridge->owner;

    cmeta_mutex_lock(&impl->lock);
    if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED) {
        bridge->cancel_route_active = false;
        sharded_adapter_maybe_release_locked(impl, bridge);
    }
    cmeta_mutex_unlock(&impl->lock);
}

static int sharded_adapter_actor_submit(
    void *backend_user,
    cflow_io_actor *actor,
    cflow_io_request_id request_id,
    cflow_io_lease_id lease_id,
    void *operation_user) {
    cflow_io_native_sharded_adapter *adapter =
        (cflow_io_native_sharded_adapter *)backend_user;
    cflow_io_native_sharded_adapter_impl *impl =
        sharded_adapter_impl(adapter);
    native_io_sharded_operation *operation =
        (native_io_sharded_operation *)operation_user;
    cflow_io_native_sharded_bridge *bridge;
    native_io_sharded_ownership ownership;
    int status;

    (void)lease_id;
    if (impl == NULL || actor == NULL || request_id == 0u ||
        operation == NULL ||
        !native_io_sharded_operation_valid(operation))
        return SALTS_EINVAL;

    cmeta_mutex_lock(&impl->lock);
    if (impl->closed) {
        cmeta_mutex_unlock(&impl->lock);
        return SALTS_ESHUTDOWN;
    }
    if (impl->bound_actor != NULL && impl->bound_actor != actor) {
        cmeta_mutex_unlock(&impl->lock);
        return SALTS_EINVAL;
    }
    if (impl->bound_actor == NULL)
        impl->bound_actor = actor;
    bridge = sharded_adapter_reserve_locked(impl);
    if (bridge == NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return SALTS_ENOBUFS;
    }
    bridge->actor = actor;
    bridge->actor_request = request_id;
    bridge->operation = operation;
    cmeta_mutex_unlock(&impl->lock);

    ownership = (native_io_sharded_ownership){
        sharded_adapter_terminal, sharded_adapter_finalize, bridge};
    status = native_io_sharded_try_submit_owned(
        impl->runtime, operation, &ownership,
        sharded_adapter_admission, bridge);
    if (status != SALTS_OK) {
        cmeta_mutex_lock(&impl->lock);
        if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED &&
            bridge->actor == actor &&
            bridge->actor_request == request_id)
            sharded_adapter_release_locked(impl, bridge);
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }

    cmeta_mutex_lock(&impl->lock);
    ++impl->accepted_routes;
    cmeta_mutex_unlock(&impl->lock);
    return SALTS_OK;
}

static int sharded_adapter_actor_cancel(
    void *backend_user, cflow_io_request_id request_id) {
    cflow_io_native_sharded_adapter *adapter =
        (cflow_io_native_sharded_adapter *)backend_user;
    cflow_io_native_sharded_adapter_impl *impl =
        sharded_adapter_impl(adapter);
    cflow_io_native_sharded_bridge *bridge;
    native_io_sharded_task task;
    size_t owner_shard = SIZE_MAX;
    bool route = false;
    int status;

    if (impl == NULL || request_id == 0u)
        return SALTS_EINVAL;

    cmeta_mutex_lock(&impl->lock);
    bridge = sharded_adapter_find_request_locked(impl, request_id);
    if (bridge == NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return SALTS_ENOENT;
    }
    bridge->cancel_requested = true;
    if (bridge->admission_done &&
        native_io_sharded_request_valid(bridge->native_request) &&
        !bridge->io_finalized &&
        !bridge->cancel_route_active) {
        bridge->cancel_route_active = true;
        owner_shard =
            native_io_sharded_request_owner_shard(bridge->native_request);
        route = owner_shard != SIZE_MAX;
    }
    cmeta_mutex_unlock(&impl->lock);

    if (!route)
        return SALTS_OK;

    task = (native_io_sharded_task){
        sharded_adapter_cancel_run,
        sharded_adapter_cancel_task_cancel,
        sharded_adapter_cancel_task_finalize,
        bridge};
    status = native_io_sharded_try_submit_to(
        impl->runtime, owner_shard, &task);
    if (status != SALTS_OK) {
        cmeta_mutex_lock(&impl->lock);
        if (bridge->phase == CFLOW_IO_NATIVE_SHARDED_BRIDGE_ROUTED &&
            bridge->actor_request == request_id) {
            bridge->cancel_route_active = false;
            ++impl->cancel_route_rejections;
            sharded_adapter_maybe_release_locked(impl, bridge);
        }
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }

    cmeta_mutex_lock(&impl->lock);
    ++impl->cancel_routes;
    cmeta_mutex_unlock(&impl->lock);
    return SALTS_OK;
}

int cflow_io_native_sharded_adapter_init(
    cflow_io_native_sharded_adapter *adapter,
    const cflow_io_native_sharded_adapter_config *config) {
    cflow_io_native_sharded_adapter_impl *impl;
    size_t index;

    if (adapter == NULL || config == NULL || adapter->impl != NULL ||
        config->runtime == NULL || config->bridge_capacity == 0u ||
        config->bridge_capacity > UINT32_MAX ||
        config->bridge_capacity >
            SIZE_MAX / sizeof(cflow_io_native_sharded_bridge))
        return SALTS_EINVAL;

    impl = (cflow_io_native_sharded_adapter_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return SALTS_ENOMEM;
    impl->bridges = (cflow_io_native_sharded_bridge *)calloc(
        config->bridge_capacity, sizeof(*impl->bridges));
    if (impl->bridges == NULL) {
        free(impl);
        return SALTS_ENOMEM;
    }
    cmeta_mutex_init(&impl->lock);
    if (impl->lock == NULL) {
        free(impl->bridges);
        free(impl);
        return SALTS_ENOMEM;
    }

    impl->runtime = config->runtime;
    impl->bridge_capacity = config->bridge_capacity;
    impl->free_head = 0u;
    for (index = 0u; index < impl->bridge_capacity; ++index) {
        impl->bridges[index].owner = impl;
        impl->bridges[index].generation = 1u;
        impl->bridges[index].phase =
            CFLOW_IO_NATIVE_SHARDED_BRIDGE_FREE;
        impl->bridges[index].next_free =
            index + 1u < impl->bridge_capacity
                ? (uint32_t)(index + 1u)
                : CFLOW_IO_NATIVE_SHARDED_NO_SLOT;
    }
    adapter->impl = impl;
    return SALTS_OK;
}

cflow_io_backend_ops cflow_io_native_sharded_adapter_actor_ops(void) {
    const cflow_io_backend_ops ops = {
        sharded_adapter_actor_submit,
        sharded_adapter_actor_cancel};
    return ops;
}

int cflow_io_native_sharded_adapter_close(
    cflow_io_native_sharded_adapter *adapter) {
    cflow_io_native_sharded_adapter_impl *impl =
        sharded_adapter_impl(adapter);

    if (impl == NULL)
        return SALTS_EINVAL;
    cmeta_mutex_lock(&impl->lock);
    if (impl->closed) {
        cmeta_mutex_unlock(&impl->lock);
        return SALTS_EALREADY;
    }
    impl->closed = true;
    cmeta_mutex_unlock(&impl->lock);
    return SALTS_OK;
}

bool cflow_io_native_sharded_adapter_get_stats(
    const cflow_io_native_sharded_adapter *adapter,
    cflow_io_native_sharded_adapter_stats *out_stats) {
    const cflow_io_native_sharded_adapter_impl *const_impl =
        sharded_adapter_const_impl(adapter);
    cflow_io_native_sharded_adapter_impl *impl =
        (cflow_io_native_sharded_adapter_impl *)const_impl;
    native_io_sharded_stats native =
        NATIVE_IO_SHARDED_STATS_V1_INITIALIZER;

    if (impl == NULL || out_stats == NULL ||
        !native_io_sharded_get_stats(impl->runtime, &native))
        return false;

    cmeta_mutex_lock(&impl->lock);
    *out_stats = (cflow_io_native_sharded_adapter_stats){
        .native = native,
        .bridge_capacity = impl->bridge_capacity,
        .active_bridges = impl->active_bridges,
        .accepted_routes = impl->accepted_routes,
        .raw_admissions = impl->raw_admissions,
        .raw_admission_failures = impl->raw_admission_failures,
        .terminal_completions = impl->terminal_completions,
        .stale_actor_completions = impl->stale_actor_completions,
        .cancel_routes = impl->cancel_routes,
        .cancel_route_rejections = impl->cancel_route_rejections,
        .native_cancel_errors = impl->native_cancel_errors,
        .closed = impl->closed};
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

int cflow_io_native_sharded_adapter_destroy(
    cflow_io_native_sharded_adapter *adapter) {
    cflow_io_native_sharded_adapter_impl *impl;

    if (adapter == NULL || adapter->impl == NULL)
        return SALTS_EINVAL;
    impl = (cflow_io_native_sharded_adapter_impl *)adapter->impl;

    cmeta_mutex_lock(&impl->lock);
    if (!impl->closed || impl->active_bridges != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return SALTS_EBUSY;
    }
    adapter->impl = NULL;
    cmeta_mutex_unlock(&impl->lock);

    cmeta_mutex_destroy(&impl->lock);
    free(impl->bridges);
    free(impl);
    return SALTS_OK;
}
