#include <cflow/machine_instance.h>

#include <clock.h>
#include <thread.h>

#include "../src/machine_instance_internal.h"
#include "tinytest.h"

#include <stdatomic.h>
#include <string.h>

enum { MACHINE_STATS_TIMEOUT_MS = 5000 };

typedef struct runtime_probe {
    atomic_int guards;
    atomic_int actions;
    atomic_int wakes;
} runtime_probe;

typedef struct runtime_control_probe {
    cflow_machine_instance *instance;
    bool cancel;
} runtime_control_probe;

typedef struct runtime_commit_barrier {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    bool entered;
    bool released;
    bool committed;
} runtime_commit_barrier;

typedef struct runtime_producer_context {
    cflow_machine_instance *instance;
    size_t count;
    atomic_int failures;
} runtime_producer_context;

static void runtime_producer(void *user) {
    runtime_producer_context *context =
        (runtime_producer_context *)user;
    const bool payload = true;
    const cflow_event_view event = {
        100u, &cmeta_type_bool, &payload
    };
    size_t index;
    if (context == NULL) return;
    for (index = 0u; index < context->count; ++index) {
        if (cflow_machine_instance_try_send(context->instance, &event) !=
            CFLOW_MAILBOX_OK)
            atomic_fetch_add(&context->failures, 1);
    }
}

static bool runtime_commit_barrier_init(runtime_commit_barrier *barrier) {
    if (barrier == NULL) return false;
    memset(barrier, 0, sizeof(*barrier));
    cmeta_mutex_init(&barrier->lock);
    cmeta_cond_init(&barrier->changed);
    if (barrier->lock == NULL || barrier->changed == NULL) {
        if (barrier->changed != NULL) cmeta_cond_destroy(&barrier->changed);
        if (barrier->lock != NULL) cmeta_mutex_destroy(&barrier->lock);
        *barrier = (runtime_commit_barrier){0};
        return false;
    }
    return true;
}

static void runtime_commit_barrier_destroy(runtime_commit_barrier *barrier) {
    if (barrier == NULL) return;
    if (barrier->changed != NULL) cmeta_cond_destroy(&barrier->changed);
    if (barrier->lock != NULL) cmeta_mutex_destroy(&barrier->lock);
    *barrier = (runtime_commit_barrier){0};
}

static void runtime_commit_boundary(void *user) {
    runtime_commit_barrier *barrier = (runtime_commit_barrier *)user;
    if (barrier == NULL) return;
    cmeta_mutex_lock(&barrier->lock);
    barrier->entered = true;
    cmeta_cond_broadcast(&barrier->changed);
    while (!barrier->released)
        cmeta_cond_wait(&barrier->changed, &barrier->lock);
    cmeta_mutex_unlock(&barrier->lock);
}

static void runtime_commit_barrier_wait(runtime_commit_barrier *barrier) {
    cmeta_mutex_lock(&barrier->lock);
    while (!barrier->entered)
        cmeta_cond_wait(&barrier->changed, &barrier->lock);
    cmeta_mutex_unlock(&barrier->lock);
}

static void runtime_commit_barrier_release(runtime_commit_barrier *barrier) {
    cmeta_mutex_lock(&barrier->lock);
    barrier->released = true;
    cmeta_cond_broadcast(&barrier->changed);
    cmeta_mutex_unlock(&barrier->lock);
}

static void runtime_transition_commit_probe(
    void *user, size_t transition_index, bool begin) {
    runtime_commit_barrier *barrier = (runtime_commit_barrier *)user;
    if (barrier == NULL || begin || transition_index == SIZE_MAX) return;
    cmeta_mutex_lock(&barrier->lock);
    barrier->committed = true;
    cmeta_cond_broadcast(&barrier->changed);
    cmeta_mutex_unlock(&barrier->lock);
}

static void runtime_commit_barrier_wait_committed(
    runtime_commit_barrier *barrier) {
    cmeta_mutex_lock(&barrier->lock);
    while (!barrier->committed)
        cmeta_cond_wait(&barrier->changed, &barrier->lock);
    cmeta_mutex_unlock(&barrier->lock);
}

static bool guard_enabled(void *user,
                          const void *state,
                          const void *event,
                          bool *out_enabled,
                          const char **out_error) {
    runtime_probe *probe = (runtime_probe *)user;
    (void)state;
    (void)event;
    if (out_enabled == NULL || out_error == NULL) return false;
    *out_enabled = true;
    *out_error = NULL;
    if (probe != NULL) atomic_fetch_add(&probe->guards, 1);
    return true;
}

static bool guard_disabled(void *user,
                           const void *state,
                           const void *event,
                           bool *out_enabled,
                           const char **out_error) {
    runtime_probe *probe = (runtime_probe *)user;
    (void)state;
    (void)event;
    if (out_enabled == NULL || out_error == NULL) return false;
    *out_enabled = false;
    *out_error = NULL;
    if (probe != NULL) atomic_fetch_add(&probe->guards, 1);
    return true;
}

static bool action_to_long(void *user,
                           const void *state,
                           const void *event,
                           void *out_target_state,
                           void *out_observation,
                           const char **out_error) {
    runtime_probe *probe = (runtime_probe *)user;
    (void)event;
    if (state == NULL || out_target_state == NULL ||
        out_observation == NULL || out_error == NULL)
        return false;
    *(long *)out_target_state = (long)*(const int *)state + 1L;
    *(long *)out_observation = 70L;
    *out_error = NULL;
    if (probe != NULL) atomic_fetch_add(&probe->actions, 1);
    return true;
}

static bool action_emit_event(void *user,
                              const void *state,
                              const void *event,
                              void *out_target_state,
                              void *out_observation,
                              const char **out_error) {
    runtime_probe *probe = (runtime_probe *)user;
    if (state == NULL || event == NULL || out_target_state == NULL ||
        out_observation == NULL || out_error == NULL)
        return false;
    *(int *)out_target_state = *(const int *)state +
                              (*(const bool *)event ? 1 : 0);
    *(int *)out_observation = 5;
    *out_error = NULL;
    if (probe != NULL) atomic_fetch_add(&probe->actions, 1);
    return true;
}

static bool action_finish(void *user,
                          const void *state,
                          const void *event,
                          void *out_target_state,
                          void *out_observation,
                          const char **out_error) {
    runtime_probe *probe = (runtime_probe *)user;
    if (state == NULL || event == NULL || out_target_state == NULL ||
        out_observation == NULL || out_error == NULL)
        return false;
    *(long *)out_target_state =
        (long)*(const int *)state + (long)*(const int *)event;
    *(long *)out_observation = 88L;
    *out_error = NULL;
    if (probe != NULL) atomic_fetch_add(&probe->actions, 1);
    return true;
}

static bool action_fail(void *user,
                        const void *state,
                        const void *event,
                        void *out_target_state,
                        void *out_observation,
                        const char **out_error) {
    runtime_probe *probe = (runtime_probe *)user;
    (void)state;
    (void)event;
    (void)out_target_state;
    (void)out_observation;
    if (out_error == NULL) return false;
    *out_error = "literal action failure";
    if (probe != NULL) atomic_fetch_add(&probe->actions, 1);
    return false;
}

static bool action_control_instance(void *user,
                                    const void *state,
                                    const void *event,
                                    void *out_target_state,
                                    void *out_observation,
                                    const char **out_error) {
    runtime_control_probe *probe = (runtime_control_probe *)user;
    (void)event;
    if (probe == NULL || probe->instance == NULL || state == NULL ||
        out_target_state == NULL || out_observation == NULL ||
        out_error == NULL)
        return false;
    if (probe->cancel)
        cflow_machine_instance_cancel(probe->instance);
    else {
        cflow_machine_instance_close(probe->instance);
        cflow_machine_instance_close(probe->instance);
    }
    *(long *)out_target_state = (long)*(const int *)state + 9L;
    *(long *)out_observation = 99L;
    *out_error = NULL;
    return true;
}

static void runtime_wake(void *user) {
    runtime_probe *probe = (runtime_probe *)user;
    if (probe != NULL) atomic_fetch_add(&probe->wakes, 1);
}

static void destroy_resumable(cflow_resumable *resumable) {
    if (resumable == NULL) return;
    if (resumable->ops != NULL && resumable->ops->destroy != NULL)
        resumable->ops->destroy(resumable->state);
    *resumable = (cflow_resumable){0};
}

static void destroy_source(cflow_publisher *source) {
    if (source == NULL) return;
    if (cflow_publisher_valid(source)) cflow_publisher_destroy(source);
    *source = (cflow_publisher){0};
}

static cflow_machine_definition runtime_definition(
    cflow_machine_state *states,
    cflow_event_type *events,
    cflow_machine_guard *guards,
    cflow_machine_action *actions,
    cflow_machine_transition *transitions) {
    states[0] = (cflow_machine_state){
        10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    states[1] = (cflow_machine_state){
        20u, &cmeta_type_long, CFLOW_MACHINE_STATE_DONE};
    events[0] = (cflow_event_type){100u, &cmeta_type_bool};
    guards[0] = (cflow_machine_guard){
        200u, &cmeta_type_int, 100u, &cmeta_type_bool,
        CMETA_EFFECT_PURE,
        CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
    actions[0] = (cflow_machine_action){
        300u, &cmeta_type_int, 100u, &cmeta_type_bool,
        &cmeta_type_long, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
        CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_long, 0u};
    transitions[0] = (cflow_machine_transition){
        10u, 100u, 200u, 300u, 20u, 7u};
    return (cflow_machine_definition){
        states, 2u, 10u,
        events, 1u,
        guards, 1u,
        actions, 1u,
        transitions, 1u};
}

static bool owned_value_copy(void *destination, const void *source) {
    if (destination == NULL || source == NULL) return false;
    *(int *)destination = *(const int *)source;
    return true;
}

static void owned_value_destroy(void *value) {
    (void)value;
}

static const cmeta_type_traits owned_value_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY,
    NULL,
    NULL,
    NULL,
    owned_value_copy,
    NULL,
    owned_value_destroy
};

static const cmeta_type_desc owned_value_type = {
    "machine_owned_value",
    sizeof(int),
    CMETA_ALIGNOF(int),
    CMETA_T_OBJECT,
    NULL,
    &owned_value_traits,
    NULL
};

suite("CFlow Machine Resumable runtime") {
    it("transactionally initializes exact bindings on SerialExecutor") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, NULL}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, NULL}
        };
        int initial = 7;
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        const cmeta_type_desc *state_type = NULL;
        int state_value = 0;

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        initial = 99;
        check_equal(cflow_machine_instance_current_state(&instance),
                    (cflow_machine_state_id)10u);
        check_true(cflow_machine_instance_copy_state(
            &instance, &state_type, &state_value, sizeof(state_value)));
        check_true(cmeta_type_equal(state_type, &cmeta_type_int));
        check_equal(state_value, 7);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)0u);
        check_equal(stats.completed, (uint64_t)0u);
        check_equal(stats.pending, (size_t)0u);
        check_false(stats.closed);
        check_false(stats.cancelled);

        cflow_machine_instance_destroy(&instance);
        check_null(instance.impl);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("rejects binding drift and unsupported executors without publication") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor serial = {0};
        cflow_executor manual = {0};
        cflow_executor owner = {0};
        cflow_machine_instance instance = {0};
        cflow_machine_guard_binding guard_bindings[2] = {
            {200u, guard_enabled, NULL},
            {200u, guard_enabled, NULL}
        };
        cflow_machine_action_binding action_binding = {
            300u, action_to_long, NULL
        };
        int initial = 7;
        cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, &action_binding, 1u, 4u, &serial
        };

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&serial));
        check_true(cflow_executor_manual_init(&manual));
        check_true(cflow_executor_owner_init_with_capacity(
            &owner, 4u, NULL, NULL));

        config.guard_count = 0u;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH);
        check_null(instance.impl);
        config.guard_count = 2u;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH);
        check_null(instance.impl);
        config.guard_count = 1u;
        guard_bindings[0].id = 201u;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH);
        check_null(instance.impl);
        guard_bindings[0].id = 200u;
        guard_bindings[0].fn = NULL;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_BINDING_MISMATCH);
        check_null(instance.impl);
        guard_bindings[0].fn = guard_enabled;
        config.executor = &manual;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_INVALID_EXECUTOR);
        check_null(instance.impl);
        config.executor = &owner;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_not_null(instance.impl);
        cflow_machine_instance_destroy(&instance);
        check_null(instance.impl);
        config.executor = &serial;
        config.output_type = &cmeta_type_int;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_TYPE_MISMATCH);
        check_null(instance.impl);

        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&manual);
        cflow_executor_destroy(&owner);
        cflow_executor_destroy(&serial);
        cflow_machine_destroy(&machine);
    }

    it("rejects nontrivial state storage in the supported fragment") {
        const cflow_machine_state states[] = {
            {1u, &owned_value_type, CFLOW_MACHINE_STATE_DONE}
        };
        const cflow_machine_definition definition = {
            states, 1u, 1u, NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u
        };
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        const int initial = 4;
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_int,
            NULL, 0u, NULL, 0u, 1u, &executor
        };

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_UNSUPPORTED_TYPE);
        check_null(instance.impl);

        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("maps one demanded terminal transition through WAIT then VALUE_AND_DONE") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_probe probe;
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, &probe}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, &probe}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        const cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;

        atomic_init(&probe.guards, 0);
        atomic_init(&probe.actions, 0);
        atomic_init(&probe.wakes, 0);
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        check_equal(atomic_load(&probe.guards), 0);
        check_equal(atomic_load(&probe.actions), 0);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));

        step = resumable.ops->resume(
            resumable.state, (cflow_publish_context *)&resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){runtime_wake, &probe}));
        check_true(cflow_executor_wait_idle(&executor));
        check_equal(atomic_load(&probe.wakes), 1);

        step = resumable.ops->resume(
            resumable.state, (cflow_publish_context *)&resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 70L);
        check_equal(atomic_load(&probe.guards), 1);
        check_equal(atomic_load(&probe.actions), 1);
        check_equal(cflow_machine_instance_current_state(&instance),
                    (cflow_machine_state_id)20u);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("arms an empty mailbox and wakes only after a producer sends") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_probe probe;
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, &probe}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, &probe}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;

        atomic_init(&probe.guards, 0);
        atomic_init(&probe.actions, 0);
        atomic_init(&probe.wakes, 0);
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));

        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){runtime_wake, &probe}));
        check_true(cflow_executor_wait_idle(&executor));
        check_equal(atomic_load(&probe.wakes), 0);

        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&executor));
        check_equal(atomic_load(&probe.wakes), 1);
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 70L);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("preserves state and the first error when no transition is enabled") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_probe probe;
        cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_disabled, &probe}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_fail, &probe}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;
        int state_value = 0;
        const cmeta_type_desc *state_type = NULL;

        atomic_init(&probe.guards, 0);
        atomic_init(&probe.actions, 0);
        atomic_init(&probe.wakes, 0);
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);

        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_executor_wait_idle(&executor));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_equal(strcmp(step.error, "no enabled transition"), 0);
        check_equal(strcmp(cflow_machine_instance_error(&instance),
                           "no enabled transition"), 0);
        check_true(cflow_machine_instance_copy_state(
            &instance, &state_type, &state_value, sizeof(state_value)));
        check_equal(state_value, 7);
        check_equal(atomic_load(&probe.guards), 1);
        check_equal(atomic_load(&probe.actions), 0);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)1u);
        check_equal(stats.failed, (uint64_t)1u);
        check_equal(stats.completed, (uint64_t)0u);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);

        guard_bindings[0].fn = guard_enabled;
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_executor_wait_idle(&executor));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_ERROR);
        check_equal(strcmp(step.error, "literal action failure"), 0);
        check_equal(strcmp(cflow_machine_instance_error(&instance),
                           "literal action failure"), 0);
        check_true(cflow_machine_instance_copy_state(
            &instance, &state_type, &state_value, sizeof(state_value)));
        check_equal(state_value, 7);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("consumes a self-emitted Event before satisfying one VALUE demand") {
        const cflow_machine_state states[] = {
            {10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {20u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {30u, &cmeta_type_long, CFLOW_MACHINE_STATE_DONE}
        };
        const cflow_event_type events[] = {
            {100u, &cmeta_type_bool},
            {101u, &cmeta_type_int}
        };
        const cflow_machine_action actions[] = {
            {300u, &cmeta_type_int, 100u, &cmeta_type_bool,
             &cmeta_type_int, CMETA_EFFECT_PURE,
             CMETA_PROP_DETERMINISTIC | CMETA_PROP_TOTAL |
                 CMETA_PROP_NO_ALIAS,
             CFLOW_MACHINE_ACTION_EVENT, &cmeta_type_int, 101u},
            {301u, &cmeta_type_int, 101u, &cmeta_type_int,
             &cmeta_type_long, CMETA_EFFECT_PURE,
             CMETA_PROP_DETERMINISTIC | CMETA_PROP_TOTAL |
                 CMETA_PROP_NO_ALIAS,
             CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_long, 0u}
        };
        const cflow_machine_transition transitions[] = {
            {10u, 100u, 0u, 300u, 20u, 1u},
            {20u, 101u, 0u, 301u, 30u, 1u}
        };
        const cflow_machine_definition definition = {
            states, 3u, 10u, events, 2u, NULL, 0u,
            actions, 2u, transitions, 2u
        };
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_probe probe;
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_emit_event, &probe},
            {301u, action_finish, &probe}
        };
        const int initial = 2;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            NULL, 0u, action_bindings, 2u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;

        atomic_init(&probe.guards, 0);
        atomic_init(&probe.actions, 0);
        atomic_init(&probe.wakes, 0);
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);

        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){runtime_wake, &probe}));
        check_true(cflow_executor_wait_idle(&executor));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 88L);
        check_equal(atomic_load(&probe.actions), 2);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)2u);
        check_equal(stats.completed, (uint64_t)2u);
        check_equal(stats.emitted_events, (uint64_t)1u);
        check_equal(stats.emitted_values, (uint64_t)1u);
        check_equal(stats.current_state, (cflow_machine_state_id)30u);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("exposes Source terminal state and rejects a second adapter") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_publisher source = {0};
        cflow_resumable rejected = {0};
        runtime_probe probe;
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, &probe}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, &probe}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        const char *terminal_error = NULL;
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;

        atomic_init(&probe.guards, 0);
        atomic_init(&probe.actions, 0);
        atomic_init(&probe.wakes, 0);
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_publisher(&instance, &source));
        check_false(cflow_machine_instance_as_resumable(
            &instance, &rejected));
        check_equal(cflow_publisher_poll_terminal(&source, &terminal_error),
                    CFLOW_PUBLISHER_OPEN);
        check_null(terminal_error);
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_executor_wait_idle(&executor));
        step = cflow_publisher_resume(&source, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 70L);
        check_equal(cflow_publisher_poll_terminal(&source, &terminal_error),
                    CFLOW_PUBLISHER_DONE);

        destroy_source(&source);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("close cancels queued Events and preserves terminal accounting") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, NULL}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, NULL}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        cflow_machine_instance_close(&instance);
        cflow_machine_instance_close(&instance);
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_CANCELLED);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_true(stats.closed);
        check_true(stats.done);
        check_equal(stats.accepted, (uint64_t)2u);
        check_equal(stats.completed + stats.failed + stats.cancelled_events,
                    stats.accepted);
        check_equal(stats.cancelled_events, (uint64_t)2u);
        check_equal(stats.pending, (size_t)0u);
        check_equal(stats.in_flight, (size_t)0u);

        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("lets cancellation win before transition commit arbitration") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_commit_barrier barrier = {0};
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, NULL}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, NULL}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;
        int copied_state = 0;
        const cmeta_type_desc *state_type = NULL;

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_true(runtime_commit_barrier_init(&barrier));
        check_equal(cflow_machine_instance_init_internal(
                        &instance, &config, NULL, NULL,
                        runtime_commit_boundary, &barrier),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);

        runtime_commit_barrier_wait(&barrier);
        cflow_machine_instance_cancel(&instance);
        runtime_commit_barrier_release(&barrier);
        check_true(cflow_executor_wait_idle(&executor));

        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_DONE);
        check_true(cflow_machine_instance_copy_state(
            &instance, &state_type, &copied_state, sizeof(copied_state)));
        check_true(state_type == &cmeta_type_int);
        check_equal(copied_state, initial);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.completed, (uint64_t)0u);
        check_equal(stats.cancelled_events, (uint64_t)1u);
        check_equal(stats.accepted,
                    stats.completed + stats.failed + stats.cancelled_events);
        check_equal(stats.in_flight, (size_t)0u);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        runtime_commit_barrier_destroy(&barrier);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("preserves one committed value when commit wins cancellation") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_commit_barrier barrier = {0};
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, NULL}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_to_long, NULL}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;

        states[1].kind = CFLOW_MACHINE_STATE_ACTIVE;
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_true(runtime_commit_barrier_init(&barrier));
        check_equal(cflow_machine_instance_init_internal(
                        &instance, &config,
                        runtime_transition_commit_probe, &barrier,
                        runtime_commit_boundary, &barrier),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);

        runtime_commit_barrier_wait(&barrier);
        runtime_commit_barrier_release(&barrier);
        runtime_commit_barrier_wait_committed(&barrier);
        cflow_machine_instance_cancel(&instance);
        check_true(cflow_executor_wait_idle(&executor));

        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 70L);
        check_equal(cflow_machine_instance_current_state(&instance),
                    (cflow_machine_state_id)20u);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)2u);
        check_equal(stats.completed, (uint64_t)1u);
        check_equal(stats.cancelled_events, (uint64_t)1u);
        check_equal(stats.accepted,
                    stats.completed + stats.failed + stats.cancelled_events);
        check_equal(stats.in_flight, (size_t)0u);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        runtime_commit_barrier_destroy(&barrier);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("lets a reentrant close commit the executing transition once") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_control_probe probe = {&instance, false};
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, NULL}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_control_instance, &probe}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_executor_wait_idle(&executor));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
        check_equal(output, 99L);
        check_equal(cflow_machine_instance_current_state(&instance),
                    (cflow_machine_state_id)20u);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)1u);
        check_equal(stats.completed, (uint64_t)1u);
        check_equal(stats.cancelled_events, (uint64_t)0u);
        check_true(stats.closed);
        check_false(stats.cancelled);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("discards an executing transition when cancellation is reentrant") {
        cflow_machine_state states[2];
        cflow_event_type events[1];
        cflow_machine_guard guards[1];
        cflow_machine_action actions[1];
        cflow_machine_transition transitions[1];
        cflow_machine_definition definition = runtime_definition(
            states, events, guards, actions, transitions);
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_control_probe probe = {&instance, true};
        const cflow_machine_guard_binding guard_bindings[] = {
            {200u, guard_enabled, NULL}
        };
        const cflow_machine_action_binding action_bindings[] = {
            {300u, action_control_instance, &probe}
        };
        const int initial = 7;
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload
        };
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            guard_bindings, 1u, action_bindings, 1u, 4u, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;
        int state_value = 0;
        const cmeta_type_desc *state_type = NULL;

        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        check_equal(cflow_machine_instance_try_send(&instance, &event),
                    CFLOW_MAILBOX_OK);
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        check_true(cflow_executor_wait_idle(&executor));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_DONE);
        check_true(cflow_machine_instance_copy_state(
            &instance, &state_type, &state_value, sizeof(state_value)));
        check_equal(state_value, 7);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)1u);
        check_equal(stats.cancelled_events, (uint64_t)1u);
        check_equal(stats.completed, (uint64_t)0u);
        check_equal(stats.accepted,
                    stats.completed + stats.failed + stats.cancelled_events);
        check_equal(stats.in_flight, (size_t)0u);
        check_true(stats.cancelled);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }

    it("serializes transitions admitted by concurrent producers") {
        enum {
            PRODUCER_COUNT = 4,
            EVENTS_PER_PRODUCER = 256,
            TOTAL_EVENTS = PRODUCER_COUNT * EVENTS_PER_PRODUCER
        };
        const cflow_machine_state states[] = {
            {10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
        };
        const cflow_event_type events[] = {
            {100u, &cmeta_type_bool}
        };
        const cflow_machine_transition transitions[] = {
            {10u, 100u, 0u, 0u, 10u, 1u}
        };
        const cflow_machine_definition definition = {
            states, 1u, 10u, events, 1u, NULL, 0u,
            NULL, 0u, transitions, 1u
        };
        cflow_machine machine = {0};
        cflow_executor executor = {0};
        cflow_machine_instance instance = {0};
        cflow_resumable resumable = {0};
        runtime_producer_context producer = {
            &instance, EVENTS_PER_PRODUCER
        };
        cmeta_thread_t threads[PRODUCER_COUNT];
        const int initial = 7;
        const cflow_machine_instance_config config = {
            &machine, &initial, &cmeta_type_long,
            NULL, 0u, NULL, 0u, TOTAL_EVENTS, &executor
        };
        cflow_machine_instance_stats stats = {0};
        cflow_publish_context resume_context = {NULL};
        cflow_step step;
        long output = -1L;
        size_t index;

        atomic_init(&producer.failures, 0);
        check_equal(cflow_machine_build(&machine, &definition),
                    CFLOW_MACHINE_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_equal(cflow_machine_instance_init(&instance, &config),
                    CFLOW_MACHINE_INSTANCE_OK);
        check_true(cflow_machine_instance_as_resumable(
            &instance, &resumable));
        for (index = 0u; index < PRODUCER_COUNT; ++index)
            check_equal(cmeta_thread_create(
                &threads[index], runtime_producer, &producer), 0);
        for (index = 0u; index < PRODUCER_COUNT; ++index)
            check_equal(cmeta_thread_join(&threads[index]), 0);
        check_equal(atomic_load(&producer.failures), 0);

        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        const uint64_t stats_started = cmeta_monotonic_ms();
        do {
            check_true(cflow_machine_instance_get_stats(&instance, &stats));
            check_equal(stats.accepted, stats.completed + stats.failed +
                stats.cancelled_events + stats.pending + stats.in_flight);
        } while (stats.completed != TOTAL_EVENTS &&
                 cmeta_monotonic_ms() - stats_started < MACHINE_STATS_TIMEOUT_MS);
        check_equal(stats.completed, (uint64_t)TOTAL_EVENTS);
        check_true(cflow_executor_wait_idle(&executor));
        cflow_machine_instance_close(&instance);
        check_true(cflow_machine_instance_get_stats(&instance, &stats));
        check_equal(stats.accepted, (uint64_t)TOTAL_EVENTS);
        check_equal(stats.completed, (uint64_t)TOTAL_EVENTS);
        check_equal(stats.cancelled_events, (uint64_t)0u);
        check_equal(stats.in_flight, (size_t)0u);
        check_equal(stats.pending, (size_t)0u);

        destroy_resumable(&resumable);
        cflow_machine_instance_destroy(&instance);
        cflow_executor_destroy(&executor);
        cflow_machine_destroy(&machine);
    }
}
