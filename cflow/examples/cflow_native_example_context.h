#ifndef CFLOW_NATIVE_EXAMPLE_CONTEXT_H
#define CFLOW_NATIVE_EXAMPLE_CONTEXT_H

#include <cflow/io_native.h>
#include <cflow/io_native_adapter.h>

#include <clock.h>
#include <error_codes.h>
#include <thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { CFLOW_NATIVE_EXAMPLE_CAPACITY = 2, CFLOW_NATIVE_EXAMPLE_MAX_STEPS = 64 };

static const uint64_t CFLOW_NATIVE_EXAMPLE_TIMEOUT_NS = UINT64_C(5000000000);

typedef struct cflow_native_example_completion_log {
    cflow_io_request_id request_ids[CFLOW_NATIVE_EXAMPLE_CAPACITY];
    cflow_io_completion completions[CFLOW_NATIVE_EXAMPLE_CAPACITY];
    size_t count;
    size_t acknowledged;
    bool overflow;
} cflow_native_example_completion_log;

typedef struct cflow_native_example_context {
    cflow_io_native_backend backend;
    cflow_io_native_adapter adapter;
    cflow_executor executor;
    cflow_io_actor actor;
    cflow_native_example_completion_log log;
    bool backend_initialized;
    bool adapter_initialized;
    bool executor_initialized;
    bool actor_initialized;
} cflow_native_example_context;

typedef int (*cflow_native_example_forget_fn)(cflow_io_native_backend *backend, uintptr_t identity);

static int cflow_native_example_forget_until_quiescent(cflow_native_example_context *context,
                                                       uintptr_t identity,
                                                       cflow_native_example_forget_fn forget) {
    const uint64_t started = cmeta_hrtime();
    int status;

    if (context == NULL || forget == NULL)
        return SALTS_EINVAL;
    do {
        status = forget(&context->backend, identity);
        if (status != SALTS_EBUSY)
            return status;
        cmeta_thread_yield();
    } while (cmeta_hrtime() - started < CFLOW_NATIVE_EXAMPLE_TIMEOUT_NS);
    return SALTS_ETIMEDOUT;
}

static void cflow_native_example_completed(void *user, cflow_io_request_id request_id,
                                           cflow_io_lease_id lease_id, void *operation_user,
                                           const cflow_io_completion *completion) {
    cflow_native_example_completion_log *log = (cflow_native_example_completion_log *)user;
    (void)lease_id;
    (void)operation_user;
    if (log == NULL || completion == NULL)
        return;
    if (log->count >= CFLOW_NATIVE_EXAMPLE_CAPACITY) {
        log->overflow = true;
        return;
    }
    log->request_ids[log->count] = request_id;
    log->completions[log->count] = *completion;
    ++log->count;
}

static int cflow_native_example_context_init(cflow_native_example_context *context,
                                             cflow_io_native_backend_kind backend_kind,
                                             cflow_io_backend_ops backend_ops) {
    cflow_io_native_backend_config backend_config = {backend_kind, CFLOW_NATIVE_EXAMPLE_CAPACITY,
                                                     CFLOW_NATIVE_EXAMPLE_CAPACITY};
    cflow_io_actor_config actor_config;
    int status;

    if (context == NULL || backend_ops.submit == NULL)
        return SALTS_EINVAL;
    memset(context, 0, sizeof(*context));
    if (!cflow_io_native_backend_supported(backend_kind))
        return SALTS_ENOTSUP;
    status = cflow_io_native_backend_init(&context->backend, &backend_config);
    if (status != SALTS_OK)
        return status;
    context->backend_initialized = true;
    if (!cflow_executor_manual_init_with_capacity(&context->executor,
                                                  CFLOW_NATIVE_EXAMPLE_CAPACITY))
        return SALTS_ENOMEM;
    context->executor_initialized = true;

    memset(&actor_config, 0, sizeof(actor_config));
    actor_config.request_capacity = CFLOW_NATIVE_EXAMPLE_CAPACITY;
    actor_config.command_capacity = CFLOW_NATIVE_EXAMPLE_CAPACITY;
    actor_config.executor = &context->executor;
    actor_config.backend = backend_ops;
    actor_config.backend_user = &context->backend;
    actor_config.completion = cflow_native_example_completed;
    actor_config.completion_user = &context->log;
    status = cflow_io_actor_init(&context->actor, &actor_config);
    if (status == SALTS_OK)
        context->actor_initialized = true;
    return status;
}

static int cflow_native_example_adapter_context_init(cflow_native_example_context *context,
                                                     native_io_backend_kind backend_kind) {
    const cflow_io_native_adapter_config adapter_config = {
        {backend_kind, CFLOW_NATIVE_EXAMPLE_CAPACITY, CFLOW_NATIVE_EXAMPLE_CAPACITY,
         CFLOW_NATIVE_EXAMPLE_CAPACITY}};
    cflow_io_actor_config actor_config;
    int status;

    if (context == NULL)
        return SALTS_EINVAL;
    memset(context, 0, sizeof(*context));
    if (!native_io_backend_kind_supports_pipe(backend_kind))
        return SALTS_ENOTSUP;
    status = cflow_io_native_adapter_init(&context->adapter, &adapter_config);
    if (status != SALTS_OK)
        return status;
    context->adapter_initialized = true;
    if (!cflow_executor_manual_init_with_capacity(&context->executor,
                                                  CFLOW_NATIVE_EXAMPLE_CAPACITY))
        return SALTS_ENOMEM;
    context->executor_initialized = true;

    memset(&actor_config, 0, sizeof(actor_config));
    actor_config.request_capacity = CFLOW_NATIVE_EXAMPLE_CAPACITY;
    actor_config.command_capacity = CFLOW_NATIVE_EXAMPLE_CAPACITY;
    actor_config.executor = &context->executor;
    actor_config.backend = cflow_io_native_adapter_actor_ops();
    actor_config.backend_user = &context->adapter;
    actor_config.completion = cflow_native_example_completed;
    actor_config.completion_user = &context->log;
    status = cflow_io_actor_init(&context->actor, &actor_config);
    if (status == SALTS_OK)
        context->actor_initialized = true;
    return status;
}

static int cflow_native_example_acknowledge_ready(cflow_native_example_context *context) {
    while (context->log.acknowledged < context->log.count) {
        const cflow_io_request_id request_id = context->log.request_ids[context->log.acknowledged];
        if (cflow_io_actor_acknowledge(&context->actor, request_id) != CFLOW_IO_ACK_RELEASED)
            return SALTS_EPROTO;
        ++context->log.acknowledged;
    }
    return context->log.overflow ? SALTS_ERANGE : SALTS_OK;
}

static int cflow_native_example_drive_once(cflow_native_example_context *context,
                                           size_t *progressed) {
    cflow_io_run_result actor_result =
        cflow_io_actor_run_ready(&context->actor, CFLOW_NATIVE_EXAMPLE_MAX_STEPS);
    size_t actor_progress = actor_result.progressed;
    size_t observed = 0u;
    size_t executor_progress;
    int status;

    if (actor_result.status == CFLOW_IO_RUN_INVALID_ARGUMENT)
        return SALTS_EINVAL;
    if (actor_result.status == CFLOW_IO_RUN_BUSY)
        return SALTS_EBUSY;
    if (context->adapter_initialized) {
        status = cflow_io_native_adapter_observe(&context->adapter, 0u, &observed);
        if (status != SALTS_OK && status != SALTS_ETIMEDOUT)
            return status;
        actor_result = cflow_io_actor_run_ready(&context->actor, CFLOW_NATIVE_EXAMPLE_MAX_STEPS);
        if (actor_result.status == CFLOW_IO_RUN_INVALID_ARGUMENT)
            return SALTS_EINVAL;
        if (actor_result.status == CFLOW_IO_RUN_BUSY)
            return SALTS_EBUSY;
        actor_progress += actor_result.progressed;
    }
    executor_progress = cflow_executor_run_ready(&context->executor);
    status = cflow_native_example_acknowledge_ready(context);
    if (status != SALTS_OK)
        return status;
    if (progressed != NULL)
        *progressed = actor_progress + observed + executor_progress;
    return SALTS_OK;
}

static int cflow_native_example_drive_until(cflow_native_example_context *context,
                                            size_t expected_completions) {
    const uint64_t started = cmeta_hrtime();
    if (context == NULL || !context->actor_initialized ||
        expected_completions > CFLOW_NATIVE_EXAMPLE_CAPACITY)
        return SALTS_EINVAL;
    while (context->log.count < expected_completions ||
           context->log.acknowledged < context->log.count) {
        size_t progressed = 0u;
        const int status = cflow_native_example_drive_once(context, &progressed);
        if (status != SALTS_OK)
            return status;
        if (cmeta_hrtime() - started >= CFLOW_NATIVE_EXAMPLE_TIMEOUT_NS)
            return SALTS_ETIMEDOUT;
        if (progressed == 0u)
            cmeta_thread_yield();
    }
    return SALTS_OK;
}

static int cflow_native_example_close_actor(cflow_native_example_context *context) {
    const uint64_t started = cmeta_hrtime();
    int first_error = SALTS_OK;
    int status;

    if (context == NULL || !context->actor_initialized)
        return SALTS_OK;
    status = cflow_io_actor_close(&context->actor);
    if (status != SALTS_OK && status != SALTS_EALREADY)
        first_error = status;
    while (!cflow_io_actor_is_quiescent(&context->actor)) {
        size_t progressed = 0u;
        status = cflow_native_example_drive_once(context, &progressed);
        if (status != SALTS_OK && first_error == SALTS_OK)
            first_error = status;
        if (status != SALTS_OK || cmeta_hrtime() - started >= CFLOW_NATIVE_EXAMPLE_TIMEOUT_NS) {
            if (first_error == SALTS_OK)
                first_error = SALTS_ETIMEDOUT;
            break;
        }
        if (progressed == 0u)
            cmeta_thread_yield();
    }
    status = cflow_io_actor_destroy(&context->actor);
    if (status == SALTS_OK)
        context->actor_initialized = false;
    else if (first_error == SALTS_OK)
        first_error = status;
    return first_error;
}

static int cflow_native_example_destroy_context(cflow_native_example_context *context) {
    int first_error = SALTS_OK;
    int status;
    if (context == NULL)
        return SALTS_EINVAL;
    if (context->actor_initialized) {
        status = cflow_native_example_close_actor(context);
        if (status != SALTS_OK)
            first_error = status;
        if (context->actor_initialized)
            return first_error != SALTS_OK ? first_error : SALTS_EBUSY;
    }
    if (context->backend_initialized) {
        status = cflow_io_native_backend_shutdown(&context->backend);
        if (status != SALTS_OK && status != SALTS_EALREADY && first_error == SALTS_OK)
            first_error = status;
        if (status != SALTS_OK && status != SALTS_EALREADY)
            return first_error;
        status = cflow_io_native_backend_destroy(&context->backend);
        if (status != SALTS_OK)
            return first_error != SALTS_OK ? first_error : status;
        context->backend_initialized = false;
    }
    if (context->adapter_initialized) {
        status = cflow_io_native_adapter_close(&context->adapter);
        if (status != SALTS_OK && status != SALTS_EALREADY && first_error == SALTS_OK)
            first_error = status;
        if (status != SALTS_OK && status != SALTS_EALREADY)
            return first_error;
        status = cflow_io_native_adapter_destroy(&context->adapter);
        if (status != SALTS_OK)
            return first_error != SALTS_OK ? first_error : status;
        context->adapter_initialized = false;
    }
    if (context->executor_initialized) {
        if (!cflow_executor_shutdown(&context->executor))
            return first_error != SALTS_OK ? first_error : SALTS_EBUSY;
        cflow_executor_destroy(&context->executor);
        context->executor_initialized = false;
    }
    return first_error;
}

#endif /* CFLOW_NATIVE_EXAMPLE_CONTEXT_H */
