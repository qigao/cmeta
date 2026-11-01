#include <cflow/executor.h>
#include <cflow/statechart.h>
#include <cflow/statechart_instance.h>

#include "statechart_instance_internal.h"
#include "tinytest.h"

#include <error_codes.h>
#include <thread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool cflow_executor_is_current_internal(const cflow_executor *executor);

typedef struct runtime_fixture {
    cflow_statechart_state states[12];
    cflow_event_type events[2];
    cflow_statechart_guard guards[4];
    cflow_statechart_transition transitions[12];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_statechart_instance instance;
    int initial_state;
} runtime_fixture;

typedef struct selection_guard_probe {
    const void *first_state;
    int expected_state;
    size_t calls;
    bool enabled;
    bool fail;
    const char *error;
} selection_guard_probe;

typedef struct contextual_selection_guard_probe {
    cflow_machine_state_id active_state;
    cflow_machine_state_id inactive_state;
    cflow_machine_state_id pseudo_state;
    bool expect_event;
    bool enabled;
    bool observations_valid;
    size_t calls;
} contextual_selection_guard_probe;

typedef struct contextual_guard_raise_probe {
    cflow_event_id event_id;
    int first_payload;
    size_t raise_count;
    bool enabled;
    bool ignore_raise_failure;
    cflow_statechart_instance *instance;
    bool cancel_after_first_raise;
    size_t calls;
    size_t raised;
} contextual_guard_raise_probe;

typedef struct selection_error_reader_probe {
    const cflow_statechart_instance *instance;
    atomic_bool started;
    atomic_bool stop;
    atomic_bool observed;
} selection_error_reader_probe;

enum { SELECTION_ERROR_OBSERVE_ATTEMPTS = 1000 };
enum {
    SELECTION_ROOT = 700,
    SELECTION_LEFT_REGION = 40,
    SELECTION_LEFT_INITIAL = 900,
    SELECTION_LEFT_LEAF = 800,
    SELECTION_RIGHT_REGION = 300,
    SELECTION_RIGHT_INITIAL = 5,
    SELECTION_RIGHT_LEAF = 10,
    SELECTION_LEFT_FINAL = 600
};
static const char selection_guard_contract_error[] =
    "Statechart guard contract violation";

static void selection_error_reader(void *user) {
    selection_error_reader_probe *probe =
        (selection_error_reader_probe *)user;
    atomic_store(&probe->started, true);
    while (!atomic_load(&probe->stop)) {
        const char *error = cflow_statechart_instance_error(probe->instance);
        if (error != NULL &&
            strcmp(error, selection_guard_contract_error) == 0) {
            atomic_store(&probe->observed, true);
            return;
        }
    }
}

typedef struct runtime_destroy_probe {
    cflow_statechart_instance *instance;
    atomic_int destroy_status;
    atomic_bool query_succeeded;
} runtime_destroy_probe;

typedef struct runtime_shared_executor_probe {
    cflow_executor *executor;
    cflow_statechart_instance *instance;
    const cflow_statechart_instance_config *config;
    atomic_bool blocker_entered;
    atomic_bool blocker_release;
    atomic_bool init_done;
    atomic_bool destroy_started;
    atomic_bool destroy_done;
    atomic_int hook_status;
    atomic_int init_status;
    atomic_int destroy_status;
} runtime_shared_executor_probe;

enum { RUNTIME_SHARED_WAIT_ATTEMPTS = 1000 };

static bool runtime_wait_flag(atomic_bool *flag) {
    size_t attempt;
    for (attempt = 0u; attempt < RUNTIME_SHARED_WAIT_ATTEMPTS; ++attempt) {
        if (atomic_load(flag)) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(flag);
}

static void runtime_unrelated_blocker(void *user) {
    runtime_shared_executor_probe *probe =
        (runtime_shared_executor_probe *)user;
    atomic_store(&probe->blocker_entered, true);
    while (!atomic_load(&probe->blocker_release)) cmeta_thread_yield();
}

static void runtime_queue_unrelated_after_statechart_idle(void *user) {
    runtime_shared_executor_probe *probe =
        (runtime_shared_executor_probe *)user;
    int status = 0;
    if (!cflow_executor_wait_idle(probe->executor)) {
        status = 1;
    } else if (cflow_executor_try_post(
                   probe->executor, runtime_unrelated_blocker, probe) !=
               CFLOW_ADMISSION_ACCEPTED) {
        status = 2;
    } else if (!runtime_wait_flag(&probe->blocker_entered)) {
        status = 3;
    }
    atomic_store(&probe->hook_status, status);
}

static void runtime_init_on_shared_executor(void *user) {
    runtime_shared_executor_probe *probe =
        (runtime_shared_executor_probe *)user;
    const cflow_statechart_instance_status status =
        cflow_statechart_instance_init_test_internal(
            probe->instance, probe->config,
            runtime_queue_unrelated_after_statechart_idle, probe);
    atomic_store(&probe->init_status, (int)status);
    atomic_store(&probe->init_done, true);
}

static void runtime_destroy_on_shared_executor(void *user) {
    runtime_shared_executor_probe *probe =
        (runtime_shared_executor_probe *)user;
    atomic_store(&probe->destroy_started, true);
    const cflow_statechart_instance_status status =
        cflow_statechart_instance_destroy(probe->instance);
    atomic_store(&probe->destroy_status, (int)status);
    atomic_store(&probe->destroy_done, true);
}

static void destroy_from_executor_callback(void *user) {
    runtime_destroy_probe *probe = (runtime_destroy_probe *)user;
    cflow_machine_state_id states[3] = {0};
    size_t state_count = 0u;
    uint64_t version = 0u;
    cflow_statechart_instance_status destroy_status;
    bool query_succeeded;
    destroy_status = cflow_statechart_instance_destroy(probe->instance);
    query_succeeded = destroy_status == CFLOW_STATECHART_INSTANCE_WOULD_BLOCK &&
        cflow_statechart_instance_copy_configuration(
            probe->instance, states, 3u, &state_count, &version) ==
            CFLOW_STATECHART_SNAPSHOT_OK &&
        state_count == 3u && version == UINT64_C(1) &&
        states[0] == 70u && states[1] == 20u && states[2] == 90u;
    atomic_store(&probe->destroy_status, (int)destroy_status);
    atomic_store(&probe->query_succeeded, query_succeeded);
}

static void nested_compound_fixture(runtime_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        90u, 20u, CFLOW_STATECHART_ATOMIC, 50u};
    fixture->states[1] = (cflow_statechart_state){
        40u, 70u, CFLOW_STATECHART_INITIAL, 20u};
    fixture->states[2] = (cflow_statechart_state){
        70u, 0u, CFLOW_STATECHART_COMPOUND, 10u};
    fixture->states[3] = (cflow_statechart_state){
        80u, 20u, CFLOW_STATECHART_INITIAL, 40u};
    fixture->states[4] = (cflow_statechart_state){
        20u, 70u, CFLOW_STATECHART_COMPOUND, 30u};
    fixture->transitions[0] = (cflow_statechart_transition){
        10u, 40u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 20u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        11u, 80u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 90u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 5u, NULL, 0u, NULL, 0u, NULL, 0u,
        fixture->transitions, 2u, NULL, 0u, NULL, 0u};
    fixture->initial_state = 41;
}

static void parallel_fixture(runtime_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        90u, 50u, CFLOW_STATECHART_ATOMIC, 40u};
    fixture->states[1] = (cflow_statechart_state){
        20u, 70u, CFLOW_STATECHART_COMPOUND, 50u};
    fixture->states[2] = (cflow_statechart_state){
        70u, 0u, CFLOW_STATECHART_PARALLEL, 10u};
    fixture->states[3] = (cflow_statechart_state){
        10u, 50u, CFLOW_STATECHART_INITIAL, 30u};
    fixture->states[4] = (cflow_statechart_state){
        50u, 70u, CFLOW_STATECHART_COMPOUND, 20u};
    fixture->states[5] = (cflow_statechart_state){
        80u, 20u, CFLOW_STATECHART_INITIAL, 60u};
    fixture->states[6] = (cflow_statechart_state){
        40u, 20u, CFLOW_STATECHART_ATOMIC, 70u};
    fixture->transitions[0] = (cflow_statechart_transition){
        10u, 10u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 90u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        11u, 80u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 40u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 7u, NULL, 0u, NULL, 0u, NULL, 0u,
        fixture->transitions, 2u, NULL, 0u, NULL, 0u};
    fixture->initial_state = 73;
}

static cflow_statechart_instance_status runtime_fixture_init(
    runtime_fixture *fixture) {
    cflow_statechart_instance_config config;
    cflow_statechart_status build_status = cflow_statechart_build(
        &fixture->statechart, &fixture->definition);
    check_equal(build_status, CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .external_event_capacity = 4u,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static void runtime_fixture_destroy(runtime_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

static bool guard_binding_disabled(void *user, const void *state,
                                   const cflow_event_view *event,
                                   bool *out_enabled,
                                   const char **out_error) {
    (void)state;
    (void)event;
    if (out_enabled == NULL || out_error == NULL) return false;
    if (user != NULL) atomic_fetch_add((atomic_int *)user, 1);
    *out_enabled = false;
    *out_error = NULL;
    return true;
}

static bool contextual_guard_binding_disabled(
    void *user, const cflow_statechart_guard_context *context,
    bool *out_enabled, const char **out_error) {
    if (context == NULL || context->state == NULL || out_enabled == NULL ||
        out_error == NULL) {
        return false;
    }
    if (user != NULL) atomic_fetch_add((atomic_int *)user, 1);
    *out_enabled = false;
    *out_error = NULL;
    return true;
}

static bool contextual_selection_guard(
    void *user, const cflow_statechart_guard_context *context,
    bool *out_enabled, const char **out_error) {
    contextual_selection_guard_probe *probe =
        (contextual_selection_guard_probe *)user;
    const bool event_valid = probe != NULL &&
        (probe->expect_event
             ? context != NULL && context->event != NULL &&
                   context->event->id == 100u &&
                   context->event->payload_type == &cmeta_type_int &&
                   context->event->payload != NULL &&
                   *(const int *)context->event->payload == 7
             : context != NULL && context->event == NULL);
    if (probe == NULL || context == NULL || context->state == NULL ||
        context->is_active == NULL || context->configuration_user == NULL ||
        out_enabled == NULL || out_error == NULL) {
        return false;
    }
    probe->observations_valid = event_valid &&
        *(const int *)context->state == 41 &&
        context->is_active(context->configuration_user,
                           probe->active_state) &&
        !context->is_active(context->configuration_user,
                            probe->inactive_state) &&
        !context->is_active(context->configuration_user,
                            probe->pseudo_state) &&
        !context->is_active(context->configuration_user, 999999u);
    ++probe->calls;
    *out_enabled = probe->enabled;
    *out_error = NULL;
    return true;
}

static bool contextual_guard_raise_internal(
    void *user, const cflow_statechart_guard_context *context,
    bool *out_enabled, const char **out_error) {
    contextual_guard_raise_probe *probe =
        (contextual_guard_raise_probe *)user;
    size_t index;
    if (probe == NULL || context == NULL || out_enabled == NULL ||
        out_error == NULL) {
        return false;
    }
    ++probe->calls;
    for (index = 0u; index < probe->raise_count; ++index) {
        const int payload = probe->first_payload + (int)index;
        const cflow_event_view event = {
            probe->event_id, &cmeta_type_int, &payload};
        if (!cflow_statechart_guard_context_raise_internal(
                context, &event, UINT64_C(0), out_error)) {
            if (!probe->ignore_raise_failure) return false;
            break;
        }
        ++probe->raised;
        if (index == 0u && probe->cancel_after_first_raise &&
            probe->instance != NULL)
            cflow_statechart_instance_cancel(probe->instance);
    }
    *out_enabled = probe->enabled;
    *out_error = NULL;
    return true;
}

static bool selection_guard(void *user, const void *state,
                            const cflow_event_view *event,
                            bool *out_enabled, const char **out_error) {
    selection_guard_probe *probe = (selection_guard_probe *)user;
    if (probe == NULL || state == NULL || event == NULL ||
        out_enabled == NULL || out_error == NULL ||
        *(const int *)state != probe->expected_state)
        return false;
    if (probe->first_state == NULL) probe->first_state = state;
    else if (probe->first_state != state) return false;
    ++probe->calls;
    *out_error = probe->error;
    *out_enabled = probe->enabled;
    return !probe->fail;
}

static bool selection_nullable_event_guard(
    void *user, const void *state, const cflow_event_view *event,
    bool *out_enabled, const char **out_error) {
    selection_guard_probe *probe = (selection_guard_probe *)user;
    if (probe == NULL || state == NULL || event != NULL ||
        out_enabled == NULL || out_error == NULL ||
        *(const int *)state != probe->expected_state)
        return false;
    if (probe->first_state == NULL) probe->first_state = state;
    else if (probe->first_state != state) return false;
    ++probe->calls;
    *out_error = probe->error;
    *out_enabled = probe->enabled;
    return !probe->fail;
}

static void selection_fixture(runtime_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        SELECTION_RIGHT_LEAF, SELECTION_RIGHT_REGION,
        CFLOW_STATECHART_ATOMIC, 7u};
    fixture->states[1] = (cflow_statechart_state){
        SELECTION_LEFT_INITIAL, SELECTION_LEFT_REGION,
        CFLOW_STATECHART_INITIAL, 2u};
    fixture->states[2] = (cflow_statechart_state){
        SELECTION_ROOT, 0u, CFLOW_STATECHART_PARALLEL, 0u};
    fixture->states[3] = (cflow_statechart_state){
        SELECTION_LEFT_FINAL, SELECTION_LEFT_REGION,
        CFLOW_STATECHART_FINAL, 4u};
    fixture->states[4] = (cflow_statechart_state){
        SELECTION_RIGHT_REGION, SELECTION_ROOT,
        CFLOW_STATECHART_COMPOUND, 5u};
    fixture->states[5] = (cflow_statechart_state){
        SELECTION_LEFT_REGION, SELECTION_ROOT,
        CFLOW_STATECHART_COMPOUND, 1u};
    fixture->states[6] = (cflow_statechart_state){
        SELECTION_RIGHT_INITIAL, SELECTION_RIGHT_REGION,
        CFLOW_STATECHART_INITIAL, 6u};
    fixture->states[7] = (cflow_statechart_state){
        SELECTION_LEFT_LEAF, SELECTION_LEFT_REGION,
        CFLOW_STATECHART_ATOMIC, 3u};
    fixture->events[0] = (cflow_event_type){100u, &cmeta_type_int};
    fixture->transitions[0] = (cflow_statechart_transition){
        100u, SELECTION_LEFT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, SELECTION_LEFT_LEAF,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        101u, SELECTION_RIGHT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, SELECTION_RIGHT_LEAF,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 8u, fixture->events, 1u,
        NULL, 0u, NULL, 0u, fixture->transitions, 2u,
        NULL, 0u, NULL, 0u};
    fixture->initial_state = 41;
}

static void mixed_conflict_fixture(runtime_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        500u, 0u, CFLOW_STATECHART_PARALLEL, 0u};
    fixture->states[1] = (cflow_statechart_state){
        400u, 500u, CFLOW_STATECHART_COMPOUND, 1u};
    fixture->states[2] = (cflow_statechart_state){
        401u, 400u, CFLOW_STATECHART_INITIAL, 2u};
    fixture->states[3] = (cflow_statechart_state){
        300u, 400u, CFLOW_STATECHART_PARALLEL, 3u};
    fixture->states[4] = (cflow_statechart_state){
        302u, 300u, CFLOW_STATECHART_ATOMIC, 4u};
    fixture->states[5] = (cflow_statechart_state){
        200u, 500u, CFLOW_STATECHART_COMPOUND, 6u};
    fixture->states[6] = (cflow_statechart_state){
        201u, 200u, CFLOW_STATECHART_INITIAL, 7u};
    fixture->states[7] = (cflow_statechart_state){
        202u, 200u, CFLOW_STATECHART_ATOMIC, 8u};
    fixture->states[8] = (cflow_statechart_state){
        102u, 300u, CFLOW_STATECHART_ATOMIC, 5u};
    fixture->events[0] = (cflow_event_type){100u, &cmeta_type_int};
    fixture->transitions[0] = (cflow_statechart_transition){
        100u, 401u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 300u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        101u, 201u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 202u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        200u, 400u, CFLOW_STATECHART_TRIGGER_EVENT, 100u, 0u, 0u, 302u,
        CFLOW_STATECHART_TRANSITION_INTERNAL, 0u, 2u};
    fixture->transitions[3] = (cflow_statechart_transition){
        201u, 202u, CFLOW_STATECHART_TRIGGER_EVENT, 100u, 0u, 0u, 202u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 3u};
    fixture->transitions[4] = (cflow_statechart_transition){
        202u, 102u, CFLOW_STATECHART_TRIGGER_EVENT, 100u, 0u, 0u, 202u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 4u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 9u, fixture->events, 1u,
        NULL, 0u, NULL, 0u, fixture->transitions, 5u,
        NULL, 0u, NULL, 0u};
    fixture->initial_state = 41;
}

static void add_event_transition(runtime_fixture *fixture,
                                 cflow_statechart_transition_id id,
                                 cflow_machine_state_id source,
                                 cflow_statechart_guard_id guard,
                                 cflow_machine_state_id target,
                                 uint32_t priority) {
    const size_t index = fixture->definition.transition_count++;
    fixture->transitions[index] = (cflow_statechart_transition){
        id, source, CFLOW_STATECHART_TRIGGER_EVENT, 100u, 0u, guard, target,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, priority, (uint32_t)index};
}

static void add_eventless_transition(
    runtime_fixture *fixture, cflow_statechart_transition_id id,
    cflow_machine_state_id source, cflow_statechart_guard_id guard,
    cflow_machine_state_id target, uint32_t priority) {
    const size_t index = fixture->definition.transition_count++;
    fixture->transitions[index] = (cflow_statechart_transition){
        id, source, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, guard, target,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, priority, (uint32_t)index};
}

static void add_completion_transition(
    runtime_fixture *fixture, cflow_statechart_transition_id id,
    cflow_machine_state_id source, cflow_machine_state_id completion,
    cflow_statechart_guard_id guard, cflow_machine_state_id target,
    uint32_t priority) {
    const size_t index = fixture->definition.transition_count++;
    fixture->transitions[index] = (cflow_statechart_transition){
        id, source, CFLOW_STATECHART_TRIGGER_COMPLETION, 0u, completion,
        guard, target, CFLOW_STATECHART_TRANSITION_EXTERNAL, priority,
        (uint32_t)index};
}

static cflow_statechart_instance_status selection_fixture_init(
    runtime_fixture *fixture,
    const cflow_statechart_guard_binding *bindings,
    size_t binding_count) {
    cflow_statechart_instance_config config;
    cflow_statechart_status build_status;
    fixture->definition.transitions = fixture->transitions;
    build_status = cflow_statechart_build(
        &fixture->statechart, &fixture->definition);
    check_equal(build_status, CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .guards = bindings,
        .guard_count = binding_count,
        .external_event_capacity = 4u,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static cflow_statechart_instance_status select_event(
    runtime_fixture *fixture, cflow_statechart_selection_snapshot *out) {
    const int payload = 7;
    const cflow_event_view event = {100u, &cmeta_type_int, &payload};
    const cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
    return cflow_statechart_instance_select_internal(
        &fixture->instance, &trigger, out);
}

static cflow_statechart_instance_status select_eventless(
    runtime_fixture *fixture, const cflow_event_view *event,
    cflow_statechart_selection_snapshot *out) {
    const cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_EVENTLESS, event, 0u};
    return cflow_statechart_instance_select_internal(
        &fixture->instance, &trigger, out);
}

static cflow_statechart_instance_status select_completion(
    runtime_fixture *fixture, cflow_machine_state_id completion,
    cflow_statechart_selection_snapshot *out) {
    const cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_COMPLETION, NULL, completion};
    return cflow_statechart_instance_select_internal(
        &fixture->instance, &trigger, out);
}

static bool executable_binding(void *user,
                               cflow_statechart_action_phase phase,
                               cflow_machine_state_id owner,
                               const void *state,
                               const cflow_event_view *event,
                               void *out_state,
                               cflow_statechart_raise_fn raise_internal,
                               void *raise_user,
                               const char **out_error) {
    (void)user;
    (void)phase;
    (void)owner;
    (void)event;
    (void)raise_internal;
    (void)raise_user;
    if (state == NULL || out_state == NULL || out_error == NULL) return false;
    *(int *)out_state = *(const int *)state;
    *out_error = NULL;
    if (user != NULL) atomic_fetch_add((atomic_int *)user, 1);
    return true;
}

static bool contextual_executable_binding(
    void *user, const cflow_statechart_executable_context *context,
    const char **out_error) {
    if (context == NULL || context->state == NULL ||
        context->out_state == NULL || out_error == NULL)
        return false;
    *(int *)context->out_state = *(const int *)context->state;
    *out_error = NULL;
    if (user != NULL) atomic_fetch_add((atomic_int *)user, 1);
    return true;
}

typedef struct initial_entry_probe {
    cflow_executor *executor;
    cflow_statechart_action_phase phases[5];
    cflow_machine_state_id owners[5];
    size_t count;
    bool executor_only;
} initial_entry_probe;

static bool initial_entry_binding(
    void *user, cflow_statechart_action_phase phase,
    cflow_machine_state_id owner, const void *state,
    const cflow_event_view *event, void *out_state,
    cflow_statechart_raise_fn raise_internal, void *raise_user,
    const char **out_error) {
    initial_entry_probe *probe = (initial_entry_probe *)user;
    (void)raise_internal;
    (void)raise_user;
    if (probe == NULL || probe->executor == NULL || state == NULL ||
        out_state == NULL || out_error == NULL ||
        (phase != CFLOW_STATECHART_ACTION_INITIAL &&
         phase != CFLOW_STATECHART_ACTION_ENTRY) ||
        event != NULL || probe->count >= 5u)
        return false;
    probe->executor_only = probe->executor_only &&
        cflow_executor_is_current_internal(probe->executor);
    probe->phases[probe->count] = phase;
    probe->owners[probe->count] = owner;
    *(int *)out_state = *(const int *)state + 1;
    ++probe->count;
    *out_error = NULL;
    return true;
}

typedef struct managed_state_value {
    int *resource;
} managed_state_value;

static size_t managed_state_copy_attempts;
static size_t managed_state_copies;
static size_t managed_state_destroys;
static size_t managed_state_live_resources;
static size_t managed_state_fail_copy_at;

static void managed_state_reset(void) {
    managed_state_copy_attempts = 0u;
    managed_state_copies = 0u;
    managed_state_destroys = 0u;
    managed_state_live_resources = 0u;
    managed_state_fail_copy_at = SIZE_MAX;
}

static managed_state_value managed_state_make(int value) {
    managed_state_value result = {0};
    result.resource = (int *)malloc(sizeof(*result.resource));
    if (result.resource != NULL) {
        *result.resource = value;
        ++managed_state_live_resources;
    }
    return result;
}

static bool managed_state_copy(void *destination_, const void *source_) {
    managed_state_value *destination =
        (managed_state_value *)destination_;
    const managed_state_value *source =
        (const managed_state_value *)source_;
    const size_t attempt = managed_state_copy_attempts++;
    destination->resource = NULL;
    if (attempt == managed_state_fail_copy_at) return false;
    if (source->resource != NULL) {
        destination->resource =
            (int *)malloc(sizeof(*destination->resource));
        if (destination->resource == NULL) return false;
        *destination->resource = *source->resource;
        ++managed_state_live_resources;
    }
    ++managed_state_copies;
    return true;
}

static void managed_state_move(void *destination_, void *source_) {
    managed_state_value *destination =
        (managed_state_value *)destination_;
    managed_state_value *source = (managed_state_value *)source_;
    destination->resource = source->resource;
    source->resource = NULL;
}

static void managed_state_destroy(void *value_) {
    managed_state_value *value = (managed_state_value *)value_;
    if (value->resource != NULL) {
        free(value->resource);
        value->resource = NULL;
        --managed_state_live_resources;
    }
    ++managed_state_destroys;
}

static const cmeta_type_traits managed_state_traits = {
    .flags = CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY,
    .copy_construct = managed_state_copy,
    .move_construct = managed_state_move,
    .destroy = managed_state_destroy};

static const cmeta_type_desc managed_state_type = {
    .name = "managed_state_value",
    .size = sizeof(managed_state_value),
    .align = _Alignof(managed_state_value),
    .kind = CMETA_T_OBJECT,
    .traits = &managed_state_traits};

typedef struct managed_host_probe {
    size_t trigger_calls;
    size_t quiescence_calls;
    size_t guard_calls;
    int guard_observed;
    cflow_statechart_host_result external_result;
    cflow_statechart_host_result quiescence_result;
    size_t raise_count;
    size_t ticket_count;
    size_t ticket_commits;
    size_t ticket_discards;
} managed_host_probe;

static void managed_host_ticket_commit(void *user) {
    managed_host_probe *probe = (managed_host_probe *)user;
    if (probe != NULL) ++probe->ticket_commits;
}

static void managed_host_ticket_discard(void *user) {
    managed_host_probe *probe = (managed_host_probe *)user;
    if (probe != NULL) ++probe->ticket_discards;
}

static cflow_statechart_host_result managed_host_transaction(
    void *user, cflow_statechart_host_context *context,
    const char **out_error) {
    managed_host_probe *probe = (managed_host_probe *)user;
    const cflow_statechart_host_phase phase =
        cflow_statechart_host_context_phase(context);
    if (probe == NULL || context == NULL || out_error == NULL ||
        cflow_statechart_host_context_state(context) == NULL)
        return CFLOW_STATECHART_HOST_FATAL;
    if (phase == CFLOW_STATECHART_HOST_PREPARE_QUIESCENCE) {
        ++probe->quiescence_calls;
        *out_error = probe->quiescence_result ==
                CFLOW_STATECHART_HOST_FATAL
            ? "deliberate Statechart host quiescence failure" : NULL;
        return probe->quiescence_result;
    }
    if (phase == CFLOW_STATECHART_HOST_PREPARE_TRIGGER) {
        const cflow_statechart_observed_event *trigger =
            cflow_statechart_host_context_trigger(context);
        managed_state_value *state;
        if (trigger == NULL)
            return CFLOW_STATECHART_HOST_FATAL;
        if (trigger->kind != CFLOW_STATECHART_OBSERVED_EXTERNAL) {
            *out_error = NULL;
            return CFLOW_STATECHART_HOST_CONTINUE;
        }
        if (trigger->event == NULL || trigger->event->id != 7u)
            return CFLOW_STATECHART_HOST_FATAL;
        for (size_t index = 0u; index < probe->raise_count; ++index) {
            if (!cflow_statechart_host_context_raise_internal(
                    context, trigger->event, UINT64_C(99), out_error))
                return CFLOW_STATECHART_HOST_FATAL;
        }
        for (size_t index = 0u; index < probe->ticket_count; ++index) {
            const cflow_statechart_effect_ticket ticket = {
                managed_host_ticket_commit,
                managed_host_ticket_discard,
                probe};
            if (!cflow_statechart_host_context_stage_effect(
                    context, &ticket, out_error))
                return CFLOW_STATECHART_HOST_FATAL;
        }
        state = (managed_state_value *)
            cflow_statechart_host_context_edit_state(context, out_error);
        if (state == NULL || state->resource == NULL)
            return CFLOW_STATECHART_HOST_FATAL;
        ++*state->resource;
        ++probe->trigger_calls;
        *out_error = probe->external_result ==
                CFLOW_STATECHART_HOST_FATAL
            ? "deliberate Statechart host failure" : NULL;
        return probe->external_result;
    }
    *out_error = "unexpected Statechart host phase";
    return CFLOW_STATECHART_HOST_FATAL;
}

static bool managed_host_guard(
    void *user, const void *state, const cflow_event_view *event,
    bool *out_enabled, const char **out_error) {
    managed_host_probe *probe = (managed_host_probe *)user;
    const managed_state_value *value =
        (const managed_state_value *)state;
    if (probe == NULL || value == NULL || value->resource == NULL ||
        event == NULL || event->id != 7u || out_enabled == NULL ||
        out_error == NULL)
        return false;
    ++probe->guard_calls;
    probe->guard_observed = *value->resource;
    *out_enabled = probe->guard_observed == 42;
    *out_error = NULL;
    return true;
}

static cflow_statechart_instance_status managed_host_fixture_init(
    runtime_fixture *fixture, const managed_state_value *initial_state,
    managed_host_probe *probe) {
    static const cflow_event_type events[] = {
        {7u, &cmeta_type_int}};
    static const cflow_statechart_guard guards[] = {
        {9u, &managed_state_type,
         CMETA_EFFECT_MAY_FAIL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS}};
    const cflow_statechart_guard_binding bindings[] = {
        {9u, managed_host_guard, probe}};
    const cflow_statechart_instance_hooks hooks = {
        .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
        .struct_size = sizeof(cflow_statechart_instance_hooks),
        .on_host_transaction = managed_host_transaction};
    cflow_statechart_instance_config config;
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        1u, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        2u, 1u, CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        3u, 1u, CFLOW_STATECHART_ATOMIC, 2u};
    fixture->states[3] = (cflow_statechart_state){
        4u, 1u, CFLOW_STATECHART_FINAL, 3u};
    fixture->transitions[0] = (cflow_statechart_transition){
        10u, 2u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 3u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        11u, 3u, CFLOW_STATECHART_TRIGGER_EVENT, 7u, 0u, 9u, 4u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->definition = (cflow_statechart_definition){
        .state_type = &managed_state_type,
        .states = fixture->states,
        .state_count = 4u,
        .events = events,
        .event_count = 1u,
        .guards = guards,
        .guard_count = 1u,
        .transitions = fixture->transitions,
        .transition_count = 2u};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = initial_state,
        .guards = bindings,
        .guard_count = 1u,
        .external_event_capacity = 4u,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor,
        .effect_capacity = 2u,
        .hooks = &hooks,
        .hook_user = probe};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static cflow_statechart_instance_status managed_state_fixture_init_with_hooks(
    runtime_fixture *fixture, const managed_state_value *initial_state,
    const cflow_statechart_instance_hooks *hooks,
    void *hook_user) {
    cflow_statechart_instance_config config;
    nested_compound_fixture(fixture);
    fixture->definition.state_type = &managed_state_type;
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = initial_state,
        .external_event_capacity = 4u,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor,
        .hooks = hooks,
        .hook_user = hook_user};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static cflow_statechart_instance_status managed_state_fixture_init(
    runtime_fixture *fixture, const managed_state_value *initial_state) {
    return managed_state_fixture_init_with_hooks(
        fixture, initial_state, NULL, NULL);
}

typedef struct managed_action_probe {
    int observed[4];
    size_t calls;
    size_t fail_at;
    size_t cancel_at;
    cflow_statechart_instance *instance;
} managed_action_probe;

static bool managed_state_action(
    void *user, cflow_statechart_action_phase phase,
    cflow_machine_state_id owner, const void *state,
    const cflow_event_view *event, void *out_state,
    cflow_statechart_raise_fn raise_internal, void *raise_user,
    const char **out_error) {
    managed_action_probe *probe = (managed_action_probe *)user;
    const managed_state_value *current =
        (const managed_state_value *)state;
    managed_state_value next;
    size_t call;
    (void)phase;
    (void)owner;
    (void)event;
    (void)raise_internal;
    (void)raise_user;
    if (probe == NULL || current == NULL || current->resource == NULL ||
        out_state == NULL || out_error == NULL || probe->calls >= 4u)
        return false;
    call = probe->calls++;
    probe->observed[call] = *current->resource;
    if (call == probe->fail_at) {
        *out_error = "managed action failure";
        return false;
    }
    next = managed_state_make(*current->resource + 1);
    if (next.resource == NULL) {
        *out_error = "managed action allocation failed";
        return false;
    }
    *(managed_state_value *)out_state = next;
    if (call == probe->cancel_at && probe->instance != NULL)
        cflow_statechart_instance_cancel(probe->instance);
    *out_error = NULL;
    return true;
}

static cflow_statechart_instance_status managed_action_fixture_init(
    runtime_fixture *fixture, const managed_state_value *initial_state,
    managed_action_probe *probe) {
    static const cflow_event_type events[] = {
        {7u, &cmeta_type_int}};
    static const cflow_statechart_executable executables[] = {
        {20u, &managed_state_type,
         CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS},
        {21u, &managed_state_type,
         CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL,
         CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS}};
    static const cflow_statechart_transition_action transition_actions[] = {
        {11u, 20u, 0u}, {11u, 21u, 1u}};
    const cflow_statechart_executable_binding bindings[] = {
        {20u, managed_state_action, probe},
        {21u, managed_state_action, probe}};
    cflow_statechart_instance_config config;
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        1u, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        2u, 1u, CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        3u, 1u, CFLOW_STATECHART_ATOMIC, 2u};
    fixture->transitions[0] = (cflow_statechart_transition){
        10u, 2u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 3u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        11u, 3u, CFLOW_STATECHART_TRIGGER_EVENT, 7u, 0u, 0u, 0u,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->definition = (cflow_statechart_definition){
        .state_type = &managed_state_type,
        .states = fixture->states,
        .state_count = 3u,
        .events = events,
        .event_count = 1u,
        .executables = executables,
        .executable_count = 2u,
        .transitions = fixture->transitions,
        .transition_count = 2u,
        .transition_actions = transition_actions,
        .transition_action_count = 2u};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = initial_state,
        .executables = bindings,
        .executable_count = 2u,
        .external_event_capacity = 4u,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static void managed_action_send(runtime_fixture *fixture) {
    const int payload = 1;
    const cflow_event_view event = {
        7u, &cmeta_type_int, &payload};
    check_equal(cflow_statechart_instance_try_send(
                    &fixture->instance, &event),
                CFLOW_MAILBOX_OK);
    check_true(cflow_executor_wait_idle(&fixture->executor));
}

suite("CFlow Statechart instance initial configuration") {
    it("enters nested compound defaults and projects its sole leaf") {
        runtime_fixture fixture;
        cflow_machine_state_id actual[3] = {99u, 99u, 99u};
        const cflow_machine_state_id expected[] = {70u, 20u, 90u};
        size_t count = 0u;
        uint64_t version = 0u;
        nested_compound_fixture(&fixture);

        check_equal(runtime_fixture_init(&fixture),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 3u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(count, (size_t)3u);
        check_equal(version, UINT64_C(1));
        check_equal(actual, expected, sizeof(expected));
        check_equal(cflow_statechart_instance_current_state(&fixture.instance),
                    (cflow_machine_state_id)90u);
        runtime_fixture_destroy(&fixture);
    }

    it("enters all parallel regions without activating pseudo states") {
        runtime_fixture fixture;
        cflow_machine_state_id actual[5] = {0};
        const cflow_machine_state_id expected[] = {
            70u, 50u, 90u, 20u, 40u};
        size_t count = 0u;
        uint64_t version = 0u;
        parallel_fixture(&fixture);

        check_equal(runtime_fixture_init(&fixture),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 5u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(count, (size_t)5u);
        check_equal(actual, expected, sizeof(expected));
        check_equal(cflow_statechart_instance_current_state(&fixture.instance),
                    (cflow_machine_state_id)0u);
        runtime_fixture_destroy(&fixture);
    }

    it("enters every document-ordered target of a root initial transition") {
        runtime_fixture fixture;
        cflow_statechart_transition_target targets[] = {
            {10u, 51u, 0u}, {10u, 81u, 1u}};
        cflow_statechart_definition_v2 definition;
        cflow_statechart_instance_config config;
        cflow_machine_state_id actual[6] = {0};
        const cflow_machine_state_id expected[] = {
            1u, 3u, 4u, 51u, 7u, 81u};
        size_t count = 0u;
        uint64_t version = 0u;

        memset(&fixture, 0, sizeof(fixture));
        fixture.states[0] = (cflow_statechart_state){
            1u, 0u, CFLOW_STATECHART_COMPOUND, 0u};
        fixture.states[1] = (cflow_statechart_state){
            2u, 1u, CFLOW_STATECHART_INITIAL, 1u};
        fixture.states[2] = (cflow_statechart_state){
            3u, 1u, CFLOW_STATECHART_PARALLEL, 2u};
        fixture.states[3] = (cflow_statechart_state){
            4u, 3u, CFLOW_STATECHART_COMPOUND, 3u};
        fixture.states[4] = (cflow_statechart_state){
            5u, 4u, CFLOW_STATECHART_INITIAL, 4u};
        fixture.states[5] = (cflow_statechart_state){
            50u, 4u, CFLOW_STATECHART_ATOMIC, 5u};
        fixture.states[6] = (cflow_statechart_state){
            51u, 4u, CFLOW_STATECHART_ATOMIC, 6u};
        fixture.states[7] = (cflow_statechart_state){
            7u, 3u, CFLOW_STATECHART_COMPOUND, 7u};
        fixture.states[8] = (cflow_statechart_state){
            8u, 7u, CFLOW_STATECHART_INITIAL, 8u};
        fixture.states[9] = (cflow_statechart_state){
            80u, 7u, CFLOW_STATECHART_ATOMIC, 9u};
        fixture.states[10] = (cflow_statechart_state){
            81u, 7u, CFLOW_STATECHART_ATOMIC, 10u};
        fixture.transitions[0] = (cflow_statechart_transition){
            10u, 2u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 0u,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
        fixture.transitions[1] = (cflow_statechart_transition){
            11u, 5u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 50u,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
        fixture.transitions[2] = (cflow_statechart_transition){
            12u, 8u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 80u,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
        fixture.definition = (cflow_statechart_definition){
            &cmeta_type_int, fixture.states, 11u, NULL, 0u, NULL, 0u,
            NULL, 0u, fixture.transitions, 3u, NULL, 0u, NULL, 0u};
        definition = (cflow_statechart_definition_v2){
            .abi_version = CFLOW_STATECHART_DEFINITION_ABI_V2,
            .struct_size = sizeof(definition),
            .base = fixture.definition,
            .transition_targets = targets,
            .transition_target_count = 2u};
        fixture.initial_state = 9;

        check_equal(cflow_statechart_build_v2(
                        &fixture.statechart, &definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 6u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(count, (size_t)6u);
        check_equal(actual, expected, sizeof(expected));
        runtime_fixture_destroy(&fixture);
    }

    it("reports required snapshot size without a partial list or version") {
        runtime_fixture fixture;
        cflow_machine_state_id actual[2] = {91u, 92u};
        size_t count = 17u;
        uint64_t version = UINT64_C(33);
        nested_compound_fixture(&fixture);

        check_equal(runtime_fixture_init(&fixture),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_TOO_SMALL);
        check_equal(count, (size_t)3u);
        check_equal(version, UINT64_C(33));
        check_equal(actual[0], (cflow_machine_state_id)91u);
        check_equal(actual[1], (cflow_machine_state_id)92u);
        runtime_fixture_destroy(&fixture);
    }

    it("copies extended state and exposes the initial publication stats") {
        runtime_fixture fixture;
        const cmeta_type_desc *type = NULL;
        cflow_statechart_instance_stats stats = {0};
        int state = 0;
        nested_compound_fixture(&fixture);

        check_equal(runtime_fixture_init(&fixture),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &state, sizeof(state)));
        check_true(cmeta_type_equal(type, &cmeta_type_int));
        check_equal(state, 41);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.configuration_version, UINT64_C(1));
        check_equal(stats.active_state_count, (size_t)3u);
        check_equal(stats.active_leaf_count, (size_t)1u);
        check_null(cflow_statechart_instance_error(&fixture.instance));
        runtime_fixture_destroy(&fixture);
    }

    it("interleaves entry and initial actions on the SerialExecutor") {
        runtime_fixture fixture;
        const cflow_statechart_executable executable = {
            30u, &cmeta_type_int, CMETA_EFFECT_STATEFUL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        const cflow_statechart_state_action state_actions[] = {
            {90u, CFLOW_STATECHART_STATE_ACTION_ENTRY, 30u, 0u},
            {70u, CFLOW_STATECHART_STATE_ACTION_ENTRY, 30u, 0u},
            {20u, CFLOW_STATECHART_STATE_ACTION_ENTRY, 30u, 0u}};
        const cflow_statechart_transition_action transition_actions[] = {
            {10u, 30u, 0u}, {11u, 30u, 0u}};
        const cflow_statechart_action_phase expected_phases[] = {
            CFLOW_STATECHART_ACTION_ENTRY,
            CFLOW_STATECHART_ACTION_INITIAL,
            CFLOW_STATECHART_ACTION_ENTRY,
            CFLOW_STATECHART_ACTION_INITIAL,
            CFLOW_STATECHART_ACTION_ENTRY};
        const cflow_machine_state_id expected_owners[] = {
            70u, 40u, 20u, 80u, 90u};
        initial_entry_probe probe = {0};
        const cflow_statechart_executable_binding binding = {
            30u, initial_entry_binding, &probe};
        cflow_statechart_instance_config config;
        cflow_statechart_instance_stats stats = {0};
        const cmeta_type_desc *type = NULL;
        int state = 0;
        nested_compound_fixture(&fixture);
        fixture.definition.executables = &executable;
        fixture.definition.executable_count = 1u;
        fixture.definition.state_actions = state_actions;
        fixture.definition.state_action_count = 3u;
        fixture.definition.transition_actions = transition_actions;
        fixture.definition.transition_action_count = 2u;
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        probe.executor = &fixture.executor;
        probe.executor_only = true;
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .executables = &binding,
            .executable_count = 1u,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(probe.executor_only);
        check_equal(probe.count, (size_t)5u);
        check_equal(probe.phases, expected_phases, sizeof(expected_phases));
        check_equal(probe.owners, expected_owners, sizeof(expected_owners));
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &state, sizeof(state)));
        check_true(cmeta_type_equal(type, &cmeta_type_int));
        check_equal(state, 46);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.actions, UINT64_C(5));
        runtime_fixture_destroy(&fixture);
    }

    it("allocates history storage without activating history pseudo states") {
        runtime_fixture fixture;
        cflow_machine_state_id actual[2] = {0};
        const cflow_machine_state_id expected[] = {1u, 3u};
        size_t count = 0u;
        uint64_t version = 0u;
        cflow_statechart_storage_requirements requirements = {0};
        cflow_statechart_instance_config config;
        memset(&fixture, 0, sizeof(fixture));
        fixture.states[0] = (cflow_statechart_state){
            1u, 0u, CFLOW_STATECHART_COMPOUND, 0u};
        fixture.states[1] = (cflow_statechart_state){
            2u, 1u, CFLOW_STATECHART_INITIAL, 1u};
        fixture.states[2] = (cflow_statechart_state){
            3u, 1u, CFLOW_STATECHART_ATOMIC, 2u};
        fixture.states[3] = (cflow_statechart_state){
            4u, 1u, CFLOW_STATECHART_HISTORY_SHALLOW, 3u};
        fixture.states[4] = (cflow_statechart_state){
            5u, 1u, CFLOW_STATECHART_HISTORY_DEEP, 4u};
        fixture.transitions[0] = (cflow_statechart_transition){
            10u, 2u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 3u,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
        fixture.transitions[1] = (cflow_statechart_transition){
            11u, 4u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 3u,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
        fixture.transitions[2] = (cflow_statechart_transition){
            12u, 5u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u, 3u,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
        fixture.definition = (cflow_statechart_definition){
            &cmeta_type_int, fixture.states, 5u, NULL, 0u, NULL, 0u,
            NULL, 0u, fixture.transitions, 3u, NULL, 0u, NULL, 0u};
        fixture.initial_state = 19;

        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart, 4u, 0u, 4u, &requirements),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(requirements.history_bitset_bytes, (size_t)4u);
        check_equal(requirements.history_count_bytes,
                    (size_t)(4u * sizeof(size_t)));
        check_equal(requirements.total_bytes,
                    requirements.control_bytes + requirements.binding_bytes +
                    requirements.configuration_bytes +
                    requirements.history_bitset_bytes +
                    requirements.history_count_bytes +
                    requirements.extended_state_bytes +
                    requirements.index_work_bytes +
                    requirements.action_scratch_bytes +
                    requirements.internal_event_bytes +
                    requirements.external_event_bytes +
                    requirements.completion_bytes);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .max_storage_bytes = requirements.total_bytes - 1u,
            .executor = &fixture.executor};
        config.max_storage_bytes =
            (size_t)CFLOW_STATECHART_MAX_INSTANCE_BYTES + 1u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_null(fixture.instance.impl);
        config.max_storage_bytes = requirements.total_bytes - 1u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_null(fixture.instance.impl);
        config.max_storage_bytes = requirements.total_bytes;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(actual, expected, sizeof(expected));
        runtime_fixture_destroy(&fixture);
    }

    it("keeps storage live when destroy runs from its executor callback") {
        runtime_fixture fixture;
        runtime_destroy_probe probe;
        const cflow_executor_task task = {
            destroy_from_executor_callback, NULL, NULL, &probe};
        nested_compound_fixture(&fixture);
        check_equal(runtime_fixture_init(&fixture),
                    CFLOW_STATECHART_INSTANCE_OK);
        probe.instance = &fixture.instance;
        atomic_init(&probe.destroy_status, -1);
        atomic_init(&probe.query_succeeded, false);

        check_equal(cflow_executor_try_post_task(&fixture.executor, &task),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(atomic_load(&probe.destroy_status),
                    (int)CFLOW_STATECHART_INSTANCE_WOULD_BLOCK);
        check_true(atomic_load(&probe.query_succeeded));
        check_not_null(fixture.instance.impl);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_null(fixture.instance.impl);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("initializes after its own work settles while shared work remains") {
        runtime_fixture fixture;
        cflow_statechart_instance_config config;
        runtime_shared_executor_probe probe;
        cmeta_thread_t thread = NULL;
        bool returned_while_blocked;
        nested_compound_fixture(&fixture);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};
        memset(&probe, 0, sizeof(probe));
        probe.executor = &fixture.executor;
        probe.instance = &fixture.instance;
        probe.config = &config;
        atomic_init(&probe.blocker_entered, false);
        atomic_init(&probe.blocker_release, false);
        atomic_init(&probe.init_done, false);
        atomic_init(&probe.destroy_started, false);
        atomic_init(&probe.destroy_done, false);
        atomic_init(&probe.hook_status, -1);
        atomic_init(&probe.init_status, -1);
        atomic_init(&probe.destroy_status, -1);

        check_equal(cmeta_thread_create(
                        &thread, runtime_init_on_shared_executor, &probe),
                    SALTS_OK);
        check_true(runtime_wait_flag(&probe.blocker_entered));
        returned_while_blocked = runtime_wait_flag(&probe.init_done);
        atomic_store(&probe.blocker_release, true);
        check_equal(cmeta_thread_join(&thread), SALTS_OK);

        check_equal(atomic_load(&probe.hook_status), 0);
        check_equal(atomic_load(&probe.init_status),
                    (int)CFLOW_STATECHART_INSTANCE_OK);
        check_true(returned_while_blocked);
        if (fixture.instance.impl != NULL)
            check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                        CFLOW_STATECHART_INSTANCE_OK);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("destroys after its own work settles while shared work remains") {
        runtime_fixture fixture;
        runtime_shared_executor_probe probe;
        cmeta_thread_t thread = NULL;
        bool returned_while_blocked;
        nested_compound_fixture(&fixture);
        check_equal(runtime_fixture_init(&fixture),
                    CFLOW_STATECHART_INSTANCE_OK);
        memset(&probe, 0, sizeof(probe));
        probe.executor = &fixture.executor;
        probe.instance = &fixture.instance;
        atomic_init(&probe.blocker_entered, false);
        atomic_init(&probe.blocker_release, false);
        atomic_init(&probe.init_done, false);
        atomic_init(&probe.destroy_started, false);
        atomic_init(&probe.destroy_done, false);
        atomic_init(&probe.hook_status, -1);
        atomic_init(&probe.init_status, -1);
        atomic_init(&probe.destroy_status, -1);
        check_equal(cflow_executor_try_post(
                        &fixture.executor, runtime_unrelated_blocker, &probe),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(runtime_wait_flag(&probe.blocker_entered));
        check_equal(cmeta_thread_create(
                        &thread, runtime_destroy_on_shared_executor, &probe),
                    SALTS_OK);
        returned_while_blocked = runtime_wait_flag(&probe.destroy_done);
        atomic_store(&probe.blocker_release, true);
        check_equal(cmeta_thread_join(&thread), SALTS_OK);

        check_equal(atomic_load(&probe.destroy_status),
                    (int)CFLOW_STATECHART_INSTANCE_OK);
        check_true(returned_while_blocked);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("owns independent managed initial-state copies") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);

        check_equal(managed_state_fixture_init(&fixture, &initial_state),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(managed_state_copies, (size_t)2u);
        check_equal(managed_state_live_resources, (size_t)3u);
        runtime_fixture_destroy(&fixture);
        check_equal(managed_state_live_resources, (size_t)1u);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
        check_equal(managed_state_destroys, (size_t)3u);
    }

    it("rolls back managed initial-state copies when construction fails") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_state_reset();
        initial_state = managed_state_make(73);
        check_not_null(initial_state.resource);
        managed_state_fail_copy_at = 1u;

        check_equal(managed_state_fixture_init(&fixture, &initial_state),
                    CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED);
        check_null(fixture.instance.impl);
        check_equal(managed_state_copy_attempts, (size_t)2u);
        check_equal(managed_state_copies, (size_t)1u);
        check_equal(managed_state_live_resources, (size_t)1u);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
        check_equal(managed_state_destroys, (size_t)2u);
    }

    it("publishes managed state across both action buffer directions") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_action_probe probe = {
            .fail_at = SIZE_MAX, .cancel_at = SIZE_MAX};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_action_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);

        managed_action_send(&fixture);
        check_equal(probe.calls, (size_t)2u);
        check_equal(probe.observed[0], 41);
        check_equal(probe.observed[1], 42);
        check_equal(managed_state_live_resources, (size_t)3u);

        probe.fail_at = 2u;
        managed_action_send(&fixture);
        check_equal(probe.calls, (size_t)3u);
        check_equal(probe.observed[2], 43);
        check_equal(managed_state_live_resources, (size_t)2u);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("rolls back managed output when a later action fails") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_action_probe probe = {
            .fail_at = 1u, .cancel_at = SIZE_MAX};
        cflow_statechart_instance_stats stats = {0};
        managed_state_reset();
        initial_state = managed_state_make(19);
        check_not_null(initial_state.resource);
        check_equal(managed_action_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);

        managed_action_send(&fixture);
        check_equal(probe.calls, (size_t)2u);
        check_equal(probe.observed[0], 19);
        check_equal(probe.observed[1], 20);
        check_equal(managed_state_live_resources, (size_t)2u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.external_failed, UINT64_C(1));
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("destroys managed transaction state when cancellation wins") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_action_probe probe = {
            .fail_at = SIZE_MAX, .cancel_at = 0u};
        cflow_statechart_instance_stats stats = {0};
        managed_state_reset();
        initial_state = managed_state_make(23);
        check_not_null(initial_state.resource);
        check_equal(managed_action_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);
        probe.instance = &fixture.instance;

        managed_action_send(&fixture);
        check_equal(probe.calls, (size_t)2u);
        check_equal(managed_state_live_resources, (size_t)2u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.cancelled);
        check_equal(stats.external_cancelled, UINT64_C(1));
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("keeps managed published state when staging copy fails") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_action_probe probe = {
            .fail_at = SIZE_MAX, .cancel_at = SIZE_MAX};
        cflow_statechart_instance_stats stats = {0};
        managed_state_reset();
        initial_state = managed_state_make(29);
        check_not_null(initial_state.resource);
        check_equal(managed_action_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);
        managed_state_fail_copy_at = managed_state_copy_attempts;

        managed_action_send(&fixture);
        check_equal(probe.calls, (size_t)0u);
        check_equal(managed_state_live_resources, (size_t)2u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("commits a lazy host trigger edit before guard selection") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {0};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(probe.quiescence_calls, (size_t)1u);
        check_equal(managed_state_copies, (size_t)2u);

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)1u);
        check_equal(probe.guard_calls, (size_t)1u);
        check_equal(probe.guard_observed, 42);
        check_equal(managed_state_copies, (size_t)4u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.done);
        check_false(stats.errored);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("rolls back a dropped external host transaction") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {
            .external_result = CFLOW_STATECHART_HOST_DROP,
            .ticket_count = 1u};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)1u);
        check_equal(probe.quiescence_calls, (size_t)2u);
        check_equal(probe.guard_calls, (size_t)0u);
        check_equal(probe.ticket_commits, (size_t)0u);
        check_equal(probe.ticket_discards, (size_t)1u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_false(stats.done);
        check_false(stats.errored);
        check_equal(stats.external_completed, UINT64_C(1));

        probe.external_result = CFLOW_STATECHART_HOST_CONTINUE;
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)2u);
        check_equal(probe.guard_calls, (size_t)1u);
        check_equal(probe.guard_observed, 42);
        check_equal(probe.ticket_commits, (size_t)1u);
        check_equal(probe.ticket_discards, (size_t)1u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.done);
        check_false(stats.errored);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("prepares quiescence after an unhandled external trigger") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {0};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(40);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(probe.quiescence_calls, (size_t)1u);

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)1u);
        check_equal(probe.quiescence_calls, (size_t)2u);
        check_equal(probe.guard_calls, (size_t)1u);
        check_equal(probe.guard_observed, 41);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_false(stats.done);
        check_false(stats.errored);
        check_equal(stats.external_completed, UINT64_C(1));

        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("discards staged host work when a fatal result wins") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {
            .external_result = CFLOW_STATECHART_HOST_FATAL,
            .ticket_count = 1u};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)1u);
        check_equal(probe.guard_calls, (size_t)0u);
        check_equal(probe.ticket_commits, (size_t)0u);
        check_equal(probe.ticket_discards, (size_t)1u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_HOOK_FAILED);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("reports a lazy host state copy failure without calling a guard") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {0};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);
        managed_state_fail_copy_at = managed_state_copy_attempts;

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)0u);
        check_equal(probe.guard_calls, (size_t)0u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("fails fast when the host internal Event journal is full") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {.raise_count = 5u};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)0u);
        check_equal(probe.guard_calls, (size_t)0u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_INTERNAL_QUEUE_FULL);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("discards earlier tickets when the host effect journal is full") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {.ticket_count = 3u};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trigger_calls, (size_t)0u);
        check_equal(probe.ticket_commits, (size_t)0u);
        check_equal(probe.ticket_discards, (size_t)2u);
        check_equal(probe.guard_calls, (size_t)0u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EFFECT_JOURNAL_FULL);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("rejects DROP outside an external trigger phase") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {
            .quiescence_result = CFLOW_STATECHART_HOST_DROP};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_HOOK_FAILED);
        check_equal(probe.quiescence_calls, (size_t)1u);
        check_null(fixture.instance.impl);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("rejects an invalid host transaction result") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_host_probe probe = {
            .external_result = (cflow_statechart_host_result)99};
        cflow_statechart_instance_stats stats = {0};
        const int payload = 1;
        const cflow_event_view event = {
            7u, &cmeta_type_int, &payload};
        managed_state_reset();
        initial_state = managed_state_make(41);
        check_not_null(initial_state.resource);
        check_equal(managed_host_fixture_init(
                        &fixture, &initial_state, &probe),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.errored);
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_HOOK_FAILED);
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("does not expose a shallow snapshot of managed state") {
        runtime_fixture fixture;
        managed_state_value initial_state;
        managed_state_value output = {
            (int *)(uintptr_t)UINT64_C(0x1234)};
        const cmeta_type_desc *type = &cmeta_type_int;
        managed_state_reset();
        initial_state = managed_state_make(7);
        check_not_null(initial_state.resource);
        check_equal(managed_state_fixture_init(&fixture, &initial_state),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_false(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &output, sizeof(output)));
        check_null(type);
        check_equal((uintptr_t)output.resource,
                    (uintptr_t)UINT64_C(0x1234));
        runtime_fixture_destroy(&fixture);
        managed_state_destroy(&initial_state);
        check_equal(managed_state_live_resources, (size_t)0u);
    }

    it("rejects state without trivial or complete lifecycle traits") {
        static const cmeta_type_desc incomplete_state_type = {
            .name = "incomplete_state",
            .size = sizeof(int),
            .align = _Alignof(int),
            .kind = CMETA_T_OBJECT};
        runtime_fixture fixture;
        cflow_statechart_instance_config config;
        nested_compound_fixture(&fixture);
        fixture.definition.state_type = &incomplete_state_type;
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_UNSUPPORTED_TYPE);
        check_null(fixture.instance.impl);
        runtime_fixture_destroy(&fixture);
    }

    it("rejects a manual executor without publishing an instance") {
        runtime_fixture fixture;
        cflow_statechart_instance_config config;
        nested_compound_fixture(&fixture);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_manual_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_EXECUTOR);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        check_true(cflow_executor_owner_init_with_capacity(
            &fixture.executor, 4u, NULL, NULL));
        /* Initial stabilization synchronously waits for a worker today.
         * Do not silently accept owner-manual until it can be driven safely. */
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_EXECUTOR);
        check_null(fixture.instance.impl);
        runtime_fixture_destroy(&fixture);
    }

    it("validates wrong-child missing-ancestor and duplicate configurations") {
        runtime_fixture fixture;
        unsigned char scratch[1] = {0};
        const cflow_machine_state_id wrong_child[] = {70u, 50u, 90u};
        const cflow_machine_state_id missing_ancestor[] = {
            70u, 50u, 90u, 40u};
        const cflow_machine_state_id duplicate[] = {70u, 50u, 50u, 90u};
        parallel_fixture(&fixture);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);

        check_equal(cflow_statechart_configuration_validate_internal(
                        &fixture.statechart, wrong_child,
                        sizeof(wrong_child) / sizeof(wrong_child[0]),
                        scratch, sizeof(scratch)),
                    CFLOW_STATECHART_CONFIGURATION_WRONG_CHILD);
        check_equal(cflow_statechart_configuration_validate_internal(
                        &fixture.statechart, missing_ancestor,
                        sizeof(missing_ancestor) /
                            sizeof(missing_ancestor[0]),
                        scratch, sizeof(scratch)),
                    CFLOW_STATECHART_CONFIGURATION_MISSING_ANCESTOR);
        check_equal(cflow_statechart_configuration_validate_internal(
                        &fixture.statechart, duplicate, 4u,
                        scratch, sizeof(scratch)),
                    CFLOW_STATECHART_CONFIGURATION_DUPLICATE_STATE);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("rejects a nontrivial Event schema before publishing an instance") {
        static const cmeta_type_desc nontrivial_event_type = {
            "nontrivial_event", sizeof(int), _Alignof(int),
            CMETA_T_OBJECT, NULL, NULL, NULL};
        runtime_fixture fixture;
        cflow_statechart_instance_config config;
        nested_compound_fixture(&fixture);
        fixture.events[0] = (cflow_event_type){
            100u, &nontrivial_event_type};
        fixture.transitions[2] = (cflow_statechart_transition){
            12u, 90u, CFLOW_STATECHART_TRIGGER_EVENT, 100u,
            0u, 0u, 0u, CFLOW_STATECHART_TRANSITION_INTERNAL, 0u, 2u};
        fixture.definition.events = fixture.events;
        fixture.definition.event_count = 1u;
        fixture.definition.transition_count = 3u;
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_UNSUPPORTED_TYPE);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("normalizes exact bindings and rejects the binding mismatch matrix") {
        runtime_fixture fixture;
        const cflow_statechart_guard guard_declarations[] = {
            {20u, &cmeta_type_int, CMETA_EFFECT_PURE,
             CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS},
            {21u, &cmeta_type_int, CMETA_EFFECT_PURE,
             CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS}};
        const cflow_statechart_executable executable_declarations[] = {
            {30u, &cmeta_type_int, CMETA_EFFECT_PURE,
             CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS},
            {31u, &cmeta_type_int, CMETA_EFFECT_PURE,
             CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS}};
        const cflow_statechart_state_action state_actions[] = {
            {90u, CFLOW_STATECHART_STATE_ACTION_ENTRY, 30u, 0u},
            {90u, CFLOW_STATECHART_STATE_ACTION_ENTRY, 31u, 1u}};
        atomic_int guard_calls;
        atomic_int executable_calls;
        cflow_statechart_guard_binding guards[] = {
            {21u, guard_binding_disabled, &guard_calls},
            {20u, guard_binding_disabled, &guard_calls}};
        cflow_statechart_executable_binding executables[] = {
            {31u, executable_binding, &executable_calls},
            {30u, executable_binding, &executable_calls}};
        cflow_statechart_guard_binding invalid_guards[2];
        cflow_statechart_executable_binding invalid_executables[2];
        cflow_statechart_storage_requirements requirements = {0};
        cflow_statechart_instance_config config;
        atomic_init(&guard_calls, 0);
        atomic_init(&executable_calls, 0);
        nested_compound_fixture(&fixture);
        fixture.transitions[2] = (cflow_statechart_transition){
            12u, 90u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 20u, 0u,
            CFLOW_STATECHART_TRANSITION_INTERNAL, 0u, 2u};
        fixture.transitions[3] = (cflow_statechart_transition){
            13u, 90u, CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 21u, 0u,
            CFLOW_STATECHART_TRANSITION_INTERNAL, 1u, 3u};
        fixture.definition.guards = guard_declarations;
        fixture.definition.guard_count = 2u;
        fixture.definition.executables = executable_declarations;
        fixture.definition.executable_count = 2u;
        fixture.definition.transitions = fixture.transitions;
        fixture.definition.transition_count = 4u;
        fixture.definition.state_actions = state_actions;
        fixture.definition.state_action_count = 2u;
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .guards = guards,
            .guard_count = 2u,
            .executables = executables,
            .executable_count = 2u,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart, 4u, 4u, 4u, &requirements),
                    CFLOW_STATECHART_INSTANCE_OK);
        config.max_storage_bytes = 1u;
        config.guards =
            (const cflow_statechart_guard_binding *)(uintptr_t)1u;
        config.executables =
            (const cflow_statechart_executable_binding *)(uintptr_t)1u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_null(fixture.instance.impl);
        config.max_storage_bytes = requirements.total_bytes;
        config.guards = guards;
        config.executables = executables;

        config.guard_count = 1u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        check_null(fixture.instance.impl);
        config.guard_count = 2u;
        config.guards = NULL;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        config.guards = invalid_guards;
        invalid_guards[0] = guards[0];
        invalid_guards[1] = guards[0];
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        invalid_guards[0] = guards[0];
        invalid_guards[1] = guards[1];
        invalid_guards[0].id = 99u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        invalid_guards[0] = guards[0];
        invalid_guards[1] = guards[1];
        invalid_guards[1].fn = NULL;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);

        config.guards = guards;
        config.executable_count = 1u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        config.executable_count = 2u;
        config.executables = NULL;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        config.executables = invalid_executables;
        invalid_executables[0] = executables[0];
        invalid_executables[1] = executables[0];
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        invalid_executables[0] = executables[0];
        invalid_executables[1] = executables[1];
        invalid_executables[0].id = 99u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        invalid_executables[0] = executables[0];
        invalid_executables[1] = executables[1];
        invalid_executables[1].fn = NULL;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        check_equal(atomic_load(&guard_calls), 0);
        check_equal(atomic_load(&executable_calls), 0);

        config.executables = executables;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(atomic_load(&guard_calls), 2);
        check_equal(atomic_load(&executable_calls), 2);
        runtime_fixture_destroy(&fixture);
    }

    it("admits exactly one legacy or contextual executable callback") {
        runtime_fixture fixture;
        const cflow_statechart_executable declaration = {
            30u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        const cflow_statechart_transition_action action = {12u, 30u, 0u};
        atomic_int calls;
        const cflow_statechart_executable_binding legacy = {
            30u, executable_binding, &calls};
        const cflow_statechart_executable_binding contextual = {
            .id = 30u,
            .user = &calls,
            .contextual_fn = contextual_executable_binding};
        const cflow_statechart_executable_binding neither = {
            .id = 30u, .user = &calls};
        const cflow_statechart_executable_binding both = {
            .id = 30u,
            .fn = executable_binding,
            .user = &calls,
            .contextual_fn = contextual_executable_binding};
        cflow_statechart_instance_config config;
        atomic_init(&calls, 0);
        nested_compound_fixture(&fixture);
        fixture.events[0] = (cflow_event_type){100u, &cmeta_type_int};
        fixture.transitions[2] = (cflow_statechart_transition){
            12u, 90u, CFLOW_STATECHART_TRIGGER_EVENT, 100u, 0u,
            0u, 0u, CFLOW_STATECHART_TRANSITION_INTERNAL, 0u, 2u};
        fixture.definition.events = fixture.events;
        fixture.definition.event_count = 1u;
        fixture.definition.executables = &declaration;
        fixture.definition.executable_count = 1u;
        fixture.definition.transition_count = 3u;
        fixture.definition.transition_actions = &action;
        fixture.definition.transition_action_count = 1u;
        check_equal(
            cflow_statechart_build(&fixture.statechart, &fixture.definition),
            CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .executables = &legacy,
            .executable_count = 1u,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        config.executables = &contextual;
        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        config.executables = &neither;
        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        check_null(fixture.instance.impl);
        config.executables = &both;
        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        check_null(fixture.instance.impl);
        check_equal(atomic_load(&calls), 0);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("admits exactly one legacy or contextual guard callback") {
        runtime_fixture fixture;
        const cflow_statechart_guard declaration = {
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        atomic_int calls;
        const cflow_statechart_guard_binding legacy = {
            300u, guard_binding_disabled, &calls};
        const cflow_statechart_guard_binding contextual = {
            .id = 300u,
            .user = &calls,
            .contextual_fn = contextual_guard_binding_disabled};
        const cflow_statechart_guard_binding neither = {
            .id = 300u, .user = &calls};
        const cflow_statechart_guard_binding both = {
            .id = 300u,
            .fn = guard_binding_disabled,
            .user = &calls,
            .contextual_fn = contextual_guard_binding_disabled};
        cflow_statechart_instance_config config;
        atomic_init(&calls, 0);
        selection_fixture(&fixture);
        fixture.guards[0] = declaration;
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        fixture.definition.transitions = fixture.transitions;
        check_equal(
            cflow_statechart_build(&fixture.statechart, &fixture.definition),
            CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .guards = &legacy,
            .guard_count = 1u,
            .external_event_capacity = 4u,
            .internal_event_capacity = 4u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        config.guards = &contextual;
        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        config.guards = &neither;
        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        check_null(fixture.instance.impl);
        config.guards = &both;
        check_equal(
            cflow_statechart_instance_init(&fixture.instance, &config),
            CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH);
        check_null(fixture.instance.impl);
        check_equal(atomic_load(&calls), 0);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }
}

suite("CFlow Statechart deterministic transition selection") {
    it("commits disabled guard raises in evaluation order") {
        runtime_fixture fixture;
        contextual_guard_raise_probe first = {
            .event_id = 101u, .first_payload = 11, .raise_count = 1u};
        contextual_guard_raise_probe second = {
            .event_id = 101u, .first_payload = 22, .raise_count = 1u};
        const cflow_statechart_guard_binding bindings[] = {
            {.id = 300u,
             .user = &first,
             .contextual_fn = contextual_guard_raise_internal},
            {.id = 301u,
             .user = &second,
             .contextual_fn = contextual_guard_raise_internal}};
        cflow_statechart_selection_snapshot selected = {0};
        cflow_statechart_instance_stats stats = {0};
        const cmeta_type_desc *event_type = NULL;
        cflow_event_id event_id = 0u;
        int payload = 0;
        size_t index;
        const int expected[] = {11, 22};
        selection_fixture(&fixture);
        fixture.events[1] = (cflow_event_type){101u, &cmeta_type_int};
        fixture.definition.event_count = 2u;
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.guards[1] = (cflow_statechart_guard){
            301u, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 2u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        add_event_transition(&fixture, 201u, SELECTION_LEFT_LEAF,
                             301u, 0u, 1u);
        check_equal(selection_fixture_init(&fixture, bindings, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)0u);
        check_equal(first.calls, (size_t)1u);
        check_equal(second.calls, (size_t)1u);
        check_equal(first.raised, (size_t)1u);
        check_equal(second.raised, (size_t)1u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.internal_pending, (size_t)2u);
        for (index = 0u; index < 2u; ++index) {
            check_equal(cflow_statechart_instance_copy_internal_event_internal(
                            &fixture.instance, index, &event_id, &event_type,
                            &payload, sizeof(payload)),
                        CFLOW_STATECHART_INSTANCE_OK);
            check_equal(event_id, (cflow_event_id)101u);
            check_true(event_type == &cmeta_type_int);
            check_equal(payload, expected[index]);
        }
        runtime_fixture_destroy(&fixture);
    }

    it("rolls back guard raises when the selection journal fills") {
        runtime_fixture fixture;
        contextual_guard_raise_probe probe = {
            .event_id = 101u,
            .first_payload = 30,
            .raise_count = 5u,
            .ignore_raise_failure = true};
        const cflow_statechart_guard_binding binding = {
            .id = 300u,
            .user = &probe,
            .contextual_fn = contextual_guard_raise_internal};
        cflow_statechart_selection_snapshot selected = {0};
        cflow_statechart_instance_stats stats = {0};
        selection_fixture(&fixture);
        fixture.events[1] = (cflow_event_type){101u, &cmeta_type_int};
        fixture.definition.event_count = 2u;
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_GUARD_FAILED);
        check_equal(selected.transition_count, (size_t)0u);
        check_equal(probe.calls, (size_t)1u);
        check_equal(probe.raised, (size_t)4u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart internal event queue is full");
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.internal_pending, (size_t)0u);
        runtime_fixture_destroy(&fixture);
    }

    it("discards a guard raise when its callback cancels selection") {
        runtime_fixture fixture;
        contextual_guard_raise_probe probe = {
            .event_id = 101u,
            .first_payload = 40,
            .raise_count = 1u,
            .enabled = true,
            .cancel_after_first_raise = true};
        const cflow_statechart_guard_binding binding = {
            .id = 300u,
            .user = &probe,
            .contextual_fn = contextual_guard_raise_internal};
        cflow_statechart_selection_snapshot selected = {0};
        cflow_statechart_instance_stats stats = {0};
        selection_fixture(&fixture);
        fixture.events[1] = (cflow_event_type){101u, &cmeta_type_int};
        fixture.definition.event_count = 2u;
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        probe.instance = &fixture.instance;

        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_TASK_CANCELLED);
        check_equal(selected.transition_count, (size_t)0u);
        check_equal(probe.calls, (size_t)1u);
        check_equal(probe.raised, (size_t)1u);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.internal_pending, (size_t)0u);
        check_true(stats.cancelled);
        runtime_fixture_destroy(&fixture);
    }

    it("queries the published configuration from an Event contextual guard") {
        runtime_fixture fixture;
        contextual_selection_guard_probe probe = {
            SELECTION_LEFT_LEAF, SELECTION_LEFT_FINAL,
            SELECTION_LEFT_INITIAL, true, true, false, 0u};
        const cflow_statechart_guard_binding binding = {
            .id = 300u,
            .user = &probe,
            .contextual_fn = contextual_selection_guard};
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        check_equal(probe.calls, (size_t)1u);
        check_true(probe.observations_valid);
        runtime_fixture_destroy(&fixture);
    }

    it("passes no Event to an eventless contextual guard") {
        runtime_fixture fixture;
        contextual_selection_guard_probe probe = {
            SELECTION_RIGHT_LEAF, SELECTION_LEFT_FINAL,
            SELECTION_RIGHT_INITIAL, false, false, false, 0u};
        const cflow_statechart_guard_binding binding = {
            .id = 300u,
            .user = &probe,
            .contextual_fn = contextual_selection_guard};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_eventless_transition(&fixture, 200u, SELECTION_RIGHT_LEAF,
                                 300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(probe.calls >= (size_t)1u);
        check_true(probe.observations_valid);
        runtime_fixture_destroy(&fixture);
    }

    it("passes no Event to a completion contextual guard") {
        runtime_fixture fixture;
        contextual_selection_guard_probe probe = {
            SELECTION_ROOT, SELECTION_LEFT_FINAL,
            SELECTION_LEFT_INITIAL, false, true, false, 0u};
        const cflow_statechart_guard_binding binding = {
            .id = 300u,
            .user = &probe,
            .contextual_fn = contextual_selection_guard};
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_completion_transition(
            &fixture, 200u, SELECTION_ROOT, SELECTION_LEFT_REGION,
            300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_completion(
                        &fixture, SELECTION_LEFT_REGION, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        check_true(probe.calls >= (size_t)1u);
        check_true(probe.observations_valid);
        runtime_fixture_destroy(&fixture);
    }

    it("selects compatible transitions from both parallel regions") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        const cflow_statechart_transition_id expected[] = {200u, 201u};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        add_event_transition(&fixture, 201u, SELECTION_RIGHT_LEAF, 0u,
                             SELECTION_RIGHT_LEAF, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)2u);
        check_equal(selected.transition_ids, expected, sizeof(expected));
        check_true(cflow_statechart_selection_exits_internal(
            &fixture.instance, &selected, 0u, SELECTION_LEFT_LEAF));
        check_false(cflow_statechart_selection_exits_internal(
            &fixture.instance, &selected, 0u, SELECTION_RIGHT_LEAF));
        check_true(cflow_statechart_selection_exits_internal(
            &fixture.instance, &selected, 1u, SELECTION_RIGHT_LEAF));
        runtime_fixture_destroy(&fixture);
    }

    it("bubbles only after every same-source guard is disabled") {
        runtime_fixture fixture;
        selection_guard_probe first_child = {
            NULL, 41, 0u, false, false, NULL};
        selection_guard_probe second_child = {
            NULL, 41, 0u, false, false, NULL};
        selection_guard_probe parent = {NULL, 41, 0u, true, false, NULL};
        cflow_statechart_guard_binding bindings[] = {
            {300u, selection_guard, &first_child},
            {301u, selection_guard, &second_child},
            {302u, selection_guard, &parent}};
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.guards[1] = (cflow_statechart_guard){
            301u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.guards[2] = (cflow_statechart_guard){
            302u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 3u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        add_event_transition(&fixture, 201u, SELECTION_LEFT_LEAF,
                             301u, 0u, 1u);
        add_event_transition(&fixture, 202u, SELECTION_LEFT_REGION,
                             302u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, bindings, 3u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)202u);
        check_equal(first_child.calls, (size_t)1u);
        check_equal(second_child.calls, (size_t)1u);
        check_equal(parent.calls, (size_t)1u);
        check_equal(first_child.first_state, second_child.first_state);
        check_equal(first_child.first_state, parent.first_state);
        runtime_fixture_destroy(&fixture);
    }

    it("lets a child candidate hide its ancestor candidate") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        add_event_transition(&fixture, 201u, SELECTION_LEFT_REGION, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        runtime_fixture_destroy(&fixture);
    }

    it("uses lower numeric priority before declaration order") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             0u, 0u, 5u);
        add_event_transition(&fixture, 201u, SELECTION_LEFT_LEAF,
                             0u, 0u, 1u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)201u);
        runtime_fixture_destroy(&fixture);
    }

    it("continues within one source after a higher-priority guard disables") {
        runtime_fixture fixture;
        selection_guard_probe higher = {
            NULL, 41, 0u, false, false, NULL};
        selection_guard_probe lower = {
            NULL, 41, 0u, true, false, NULL};
        selection_guard_probe ancestor = {
            NULL, 41, 0u, true, false, NULL};
        cflow_statechart_guard_binding bindings[] = {
            {300u, selection_guard, &higher},
            {301u, selection_guard, &lower},
            {302u, selection_guard, &ancestor}};
        cflow_statechart_selection_snapshot selected = {0};
        size_t index;
        selection_fixture(&fixture);
        for (index = 0u; index < 3u; ++index) {
            fixture.guards[index] = (cflow_statechart_guard){
                (cflow_statechart_guard_id)(300u + index), &cmeta_type_int,
                CMETA_EFFECT_PURE,
                CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        }
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 3u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        add_event_transition(&fixture, 201u, SELECTION_LEFT_LEAF,
                             301u, 0u, 1u);
        add_event_transition(&fixture, 202u, SELECTION_LEFT_REGION,
                             302u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, bindings, 3u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)201u);
        check_equal(higher.calls, (size_t)1u);
        check_equal(lower.calls, (size_t)1u);
        check_equal(ancestor.calls, (size_t)0u);
        runtime_fixture_destroy(&fixture);
    }

    it("selects an eventless transition with a null guard event") {
        runtime_fixture fixture;
        selection_guard_probe guard = {
            NULL, 41, 0u, false, false, NULL};
        const cflow_statechart_guard_binding binding = {
            300u, selection_nullable_event_guard, &guard};
        const int payload = 7;
        const cflow_event_view unexpected_event = {
            100u, &cmeta_type_int, &payload};
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_eventless_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                                 300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        guard.enabled = true;
        guard.calls = 0u;
        check_equal(select_eventless(&fixture, NULL, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        check_equal(guard.calls, (size_t)1u);
        check_equal(select_eventless(
                        &fixture, &unexpected_event, &selected),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        runtime_fixture_destroy(&fixture);
    }

    it("selects compound and parallel completion with a null guard event") {
        runtime_fixture fixture;
        selection_guard_probe guard = {
            NULL, 41, 0u, true, false, NULL};
        const cflow_statechart_guard_binding binding = {
            300u, selection_nullable_event_guard, &guard};
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_completion_transition(
            &fixture, 200u, SELECTION_LEFT_LEAF,
            SELECTION_LEFT_REGION, 300u, 0u, 0u);
        add_completion_transition(
            &fixture, 201u, SELECTION_RIGHT_LEAF,
            SELECTION_ROOT, 300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_completion(
                        &fixture, SELECTION_LEFT_REGION, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        check_equal(select_completion(
                        &fixture, SELECTION_ROOT, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)201u);
        check_equal(guard.calls, (size_t)2u);
        runtime_fixture_destroy(&fixture);
    }

    it("rejects completion for atomic final pseudo and unknown states") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        const cflow_machine_state_id invalid[] = {
            SELECTION_LEFT_LEAF, SELECTION_LEFT_FINAL,
            SELECTION_LEFT_INITIAL, 999999u};
        size_t index;
        selection_fixture(&fixture);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        for (index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
            check_equal(select_completion(
                            &fixture, invalid[index], &selected),
                        CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        runtime_fixture_destroy(&fixture);
    }

    it("deduplicates one ancestor candidate reached from both leaves") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_ROOT, 0u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        runtime_fixture_destroy(&fixture);
    }

    it("keeps the earlier leaf when unrelated sources conflict") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF, 0u,
                             SELECTION_RIGHT_LEAF, 0u);
        add_event_transition(&fixture, 201u, SELECTION_RIGHT_LEAF, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)200u);
        runtime_fixture_destroy(&fixture);
    }

    it("lets a later descendant source preempt an earlier ancestor source") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_ROOT, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        add_event_transition(&fixture, 201u, SELECTION_RIGHT_LEAF, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids[0],
                    (cflow_statechart_transition_id)201u);
        runtime_fixture_destroy(&fixture);
    }

    it("finishes one contiguous subtree before unrelated conflict selection") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        const cflow_statechart_transition_id expected[] = {202u};
        mixed_conflict_fixture(&fixture);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)1u);
        check_equal(selected.transition_ids, expected, sizeof(expected));
        runtime_fixture_destroy(&fixture);
    }

    it("keeps targetless candidates from both regions") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot selected = {0};
        const cflow_statechart_transition_id expected[] = {200u, 201u};
        size_t byte;
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             0u, 0u, 0u);
        add_event_transition(&fixture, 201u, SELECTION_RIGHT_LEAF,
                             0u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(selected.transition_count, (size_t)2u);
        check_equal(selected.transition_ids, expected, sizeof(expected));
        for (byte = 0u;
             byte < selected.transition_count * selected.exit_set_stride;
             ++byte)
            check_equal(selected.exit_sets[byte], (unsigned char)0u);
        runtime_fixture_destroy(&fixture);
    }

    it("returns the same literal selection on repeated runs") {
        runtime_fixture fixture;
        cflow_statechart_selection_snapshot first = {0}, second = {0};
        cflow_statechart_transition_id saved[2] = {0};
        const cflow_statechart_transition_id expected[] = {200u, 201u};
        selection_fixture(&fixture);
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF, 0u,
                             SELECTION_LEFT_LEAF, 0u);
        add_event_transition(&fixture, 201u, SELECTION_RIGHT_LEAF, 0u,
                             SELECTION_RIGHT_LEAF, 0u);
        check_equal(selection_fixture_init(&fixture, NULL, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &first),
                    CFLOW_STATECHART_INSTANCE_OK);
        memcpy(saved, first.transition_ids, sizeof(saved));
        check_equal(select_event(&fixture, &second),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(saved, expected, sizeof(expected));
        check_equal(second.transition_ids, expected, sizeof(expected));
        runtime_fixture_destroy(&fixture);
    }

    it("owns the first user error from an independently fallible guard") {
        runtime_fixture fixture;
        char callback_error[64] = "fallible guard failed";
        selection_guard_probe guard = {
            NULL, 41, 0u, false, true, callback_error};
        const cflow_statechart_guard_binding binding = {
            300u, selection_guard, &guard};
        cflow_statechart_selection_snapshot selected = {0};
        cflow_machine_state_id states[5] = {0};
        const cflow_machine_state_id expected[] = {
            SELECTION_ROOT, SELECTION_LEFT_REGION, SELECTION_LEFT_LEAF,
            SELECTION_RIGHT_REGION, SELECTION_RIGHT_LEAF};
        size_t state_count = 0u;
        uint64_t version = 0u;
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_GUARD_FAILED);
        memcpy(callback_error, "mutated callback bytes", 23u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "fallible guard failed");
        check_equal(selected.transition_count, (size_t)0u);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_GUARD_FAILED);
        check_equal(guard.calls, (size_t)1u);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 5u,
                        &state_count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(version, UINT64_C(1));
        check_equal(states, expected, sizeof(expected));
        runtime_fixture_destroy(&fixture);
    }

    it("uses a stable default when a fallible guard returns no error") {
        runtime_fixture fixture;
        selection_guard_probe guard = {
            NULL, 41, 0u, false, true, NULL};
        const cflow_statechart_guard_binding binding = {
            300u, selection_guard, &guard};
        cflow_statechart_selection_snapshot selected = {0};
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, &binding, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_GUARD_FAILED);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart guard failed");
        runtime_fixture_destroy(&fixture);
    }

    it("preserves the first guard failure without publishing semantic state") {
        runtime_fixture fixture;
        selection_guard_probe first_guard = {
            NULL, 41, 0u, false, true, "first guard failed"};
        selection_guard_probe second_guard = {
            NULL, 41, 0u, false, true, "second guard failed"};
        cflow_statechart_guard_binding bindings[] = {
            {300u, selection_guard, &first_guard},
            {301u, selection_guard, &second_guard}};
        cflow_statechart_selection_snapshot selected = {0};
        cflow_machine_state_id states[5] = {0};
        const cflow_machine_state_id expected[] = {
            SELECTION_ROOT, SELECTION_LEFT_REGION, SELECTION_LEFT_LEAF,
            SELECTION_RIGHT_REGION, SELECTION_RIGHT_LEAF};
        size_t count = 0u;
        uint64_t version = 0u;
        selection_error_reader_probe reader_probe;
        cmeta_thread_t reader = NULL;
        size_t attempt;
        selection_fixture(&fixture);
        fixture.guards[0] = (cflow_statechart_guard){
            300u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.guards[1] = (cflow_statechart_guard){
            301u, &cmeta_type_int, CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 2u;
        add_event_transition(&fixture, 200u, SELECTION_LEFT_LEAF,
                             300u, 0u, 0u);
        add_event_transition(&fixture, 201u, SELECTION_RIGHT_LEAF,
                             301u, 0u, 0u);
        check_equal(selection_fixture_init(&fixture, bindings, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        reader_probe.instance = &fixture.instance;
        atomic_init(&reader_probe.started, false);
        atomic_init(&reader_probe.stop, false);
        atomic_init(&reader_probe.observed, false);
        check_equal(cmeta_thread_create(
                        &reader, selection_error_reader, &reader_probe),
                    SALTS_OK);
        while (!atomic_load(&reader_probe.started)) cmeta_thread_yield();
        check_equal(select_event(&fixture, &selected),
                    CFLOW_STATECHART_INSTANCE_GUARD_FAILED);
        for (attempt = 0u;
             attempt < (size_t)SELECTION_ERROR_OBSERVE_ATTEMPTS &&
                 !atomic_load(&reader_probe.observed);
             ++attempt)
            cmeta_sleep_ms(1u);
        atomic_store(&reader_probe.stop, true);
        check_equal(cmeta_thread_join(&reader), SALTS_OK);
        check_true(atomic_load(&reader_probe.observed));
        check_equal(selected.transition_count, (size_t)0u);
        check_equal(first_guard.calls, (size_t)1u);
        check_equal(second_guard.calls, (size_t)0u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    selection_guard_contract_error);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 5u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(version, UINT64_C(1));
        check_equal(states, expected, sizeof(expected));
        runtime_fixture_destroy(&fixture);
    }
}

typedef struct microstep_fixture {
    cflow_statechart_state states[12];
    cflow_event_type events[2];
    cflow_statechart_executable executables[2];
    cflow_statechart_transition transitions[8];
    cflow_statechart_state_action state_actions[8];
    cflow_statechart_transition_action transition_actions[8];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_statechart_instance instance;
    int initial_state;
} microstep_fixture;

typedef struct microstep_action_probe {
    cflow_executor *executor;
    int trace[24];
    int input_states[24];
    int markers[24];
    size_t calls;
    size_t fail_call;
    size_t raise_call;
    bool raise_twice;
    bool executor_only;
    bool no_alias;
    bool block_first;
    atomic_bool block_entered;
    atomic_bool block_release;
    int observed_event_payload;
} microstep_action_probe;

typedef struct configuration_trace_probe {
    int trace[16];
    unsigned int active_masks[16];
    size_t calls;
    bool rejected_unknown_and_pseudo;
} configuration_trace_probe;

typedef struct effect_terminal_probe effect_terminal_probe;

typedef struct effect_action_probe {
    cflow_statechart_instance *instance;
    effect_terminal_probe *terminals;
    size_t terminal_capacity;
    size_t prepared;
    size_t committed;
    size_t discarded;
    size_t calls;
    size_t stage_count;
    size_t fail_call;
    uint64_t expected_commit_version;
    bool stage_initial;
    bool fail_initial;
    bool block_after_stage;
    atomic_bool block_entered;
    atomic_bool block_release;
    bool commit_observed_published_configuration;
} effect_action_probe;

struct effect_terminal_probe {
    effect_action_probe *owner;
};

enum {
    MICRO_ROOT = 990u,
    MICRO_LEFT = 900u,
    MICRO_LEFT_INITIAL = 890u,
    MICRO_LEFT_NESTED = 800u,
    MICRO_NESTED_INITIAL = 790u,
    MICRO_LEFT_A = 700u,
    MICRO_LEFT_B = 690u,
    MICRO_RIGHT = 400u,
    MICRO_RIGHT_INITIAL = 390u,
    MICRO_RIGHT_C = 300u,
    MICRO_RIGHT_D = 290u,
    MICRO_EVENT = 10u,
    MICRO_RAISED_EVENT = 11u,
    MICRO_LEFT_TRANSITION = 1001u,
    MICRO_RIGHT_TRANSITION = 1000u,
    MICRO_EXECUTABLE = 500u,
    MICRO_SECOND_EXECUTABLE = 501u
};

static int microstep_trace_code(cflow_statechart_action_phase phase,
                                cflow_machine_state_id owner) {
    return (int)phase * 10000 + (int)owner;
}

static bool microstep_action(void *user,
                             cflow_statechart_action_phase phase,
                             cflow_machine_state_id owner,
                             const void *state,
                             const cflow_event_view *event,
                             void *out_state,
                             cflow_statechart_raise_fn raise_internal,
                             void *raise_user,
                             const char **out_error) {
    microstep_action_probe *probe = (microstep_action_probe *)user;
    const size_t call = probe != NULL ? probe->calls + 1u : 0u;
    int raised_value;
    cflow_event_view raised;
    const char *raise_error = NULL;
    bool raised_ok = true;
    if (probe != NULL &&
        (phase == CFLOW_STATECHART_ACTION_INITIAL ||
         phase == CFLOW_STATECHART_ACTION_ENTRY) &&
        event == NULL && state != NULL && out_state != NULL &&
        out_error != NULL) {
        *(int *)out_state = *(const int *)state;
        *out_error = NULL;
        return true;
    }
    if (probe == NULL || state == NULL || out_state == NULL ||
        out_error == NULL || event == NULL ||
        event->payload_type == NULL ||
        !cmeta_type_equal(event->payload_type, &cmeta_type_int))
        return false;
    probe->executor_only = probe->executor_only &&
        cflow_executor_is_current_internal(probe->executor);
    probe->no_alias = probe->no_alias && state != out_state;
    probe->trace[probe->calls] = microstep_trace_code(phase, owner);
    probe->input_states[probe->calls] = *(const int *)state;
    probe->observed_event_payload = *(const int *)event->payload;
    ++probe->calls;
    if (probe->block_first && call == 1u) {
        atomic_store(&probe->block_entered, true);
        while (!atomic_load(&probe->block_release)) cmeta_thread_yield();
    }
    *(int *)out_state = *(const int *)state + 1;
    *out_error = NULL;
    if (call == probe->raise_call) {
        raised_value = 700 + (int)call;
        raised = (cflow_event_view){
            MICRO_RAISED_EVENT, &cmeta_type_int, &raised_value};
        raised_ok = raise_internal != NULL &&
            raise_internal(raise_user, &raised, &raise_error);
        raised_value = -1;
        if (probe->raise_twice && raised_ok) {
            raised_value = 800 + (int)call;
            raised_ok = raise_internal(
                raise_user,
                &(cflow_event_view){
                    MICRO_RAISED_EVENT, &cmeta_type_int, &raised_value},
                &raise_error);
            raised_value = -2;
        }
        if (!raised_ok) {
            *out_error = raise_error;
            return false;
        }
    }
    if (call == probe->fail_call) {
        *out_error = "first action failure";
        return false;
    }
    return true;
}

static unsigned int configuration_trace_mask(
    const cflow_statechart_executable_context *context) {
    static const cflow_machine_state_id states[] = {
        MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_A,
        MICRO_LEFT_B, MICRO_RIGHT, MICRO_RIGHT_C, MICRO_RIGHT_D};
    unsigned int mask = 0u;
    size_t index;
    for (index = 0u; index < sizeof(states) / sizeof(states[0]); ++index) {
        if (context->is_active(context->configuration_user, states[index]))
            mask |= 1u << index;
    }
    return mask;
}

static bool configuration_trace_action(
    void *user, const cflow_statechart_executable_context *context,
    const char **out_error) {
    configuration_trace_probe *probe = (configuration_trace_probe *)user;
    if (probe == NULL || context == NULL || context->state == NULL ||
        context->out_state == NULL || context->is_active == NULL ||
        context->configuration_user == NULL || out_error == NULL ||
        probe->calls >= sizeof(probe->trace) / sizeof(probe->trace[0]))
        return false;
    probe->trace[probe->calls] =
        microstep_trace_code(context->phase, context->owner);
    probe->active_masks[probe->calls] = configuration_trace_mask(context);
    probe->rejected_unknown_and_pseudo =
        probe->rejected_unknown_and_pseudo &&
        !context->is_active(context->configuration_user, 123456u) &&
        !context->is_active(
            context->configuration_user, MICRO_LEFT_INITIAL);
    ++probe->calls;
    *(int *)context->out_state = *(const int *)context->state + 1;
    *out_error = NULL;
    return true;
}

static void effect_ticket_commit(void *user) {
    effect_terminal_probe *terminal = (effect_terminal_probe *)user;
    effect_action_probe *probe = terminal != NULL ? terminal->owner : NULL;
    cflow_machine_state_id states[8] = {0};
    size_t state_count = 0u;
    uint64_t version = 0u;
    if (probe == NULL) return;
    ++probe->committed;
    if (probe->expected_commit_version != 0u)
        probe->commit_observed_published_configuration =
            probe->commit_observed_published_configuration &&
            cflow_statechart_instance_copy_configuration(
                probe->instance, states, 8u, &state_count, &version) ==
                CFLOW_STATECHART_SNAPSHOT_OK &&
            version == probe->expected_commit_version;
}

static void effect_ticket_discard(void *user) {
    effect_terminal_probe *terminal = (effect_terminal_probe *)user;
    effect_action_probe *probe = terminal != NULL ? terminal->owner : NULL;
    if (probe != NULL) ++probe->discarded;
}

static bool effect_staging_action(
    void *user, const cflow_statechart_executable_context *context,
    const char **out_error) {
    effect_action_probe *probe = (effect_action_probe *)user;
    size_t index;
    if (probe == NULL || context == NULL || context->state == NULL ||
        context->out_state == NULL || out_error == NULL) {
        return false;
    }
    *(int *)context->out_state = *(const int *)context->state;
    *out_error = NULL;
    if (context->event != NULL) {
        ++probe->calls;
        *(int *)context->out_state = *(const int *)context->state + 1;
    }
    if ((context->event != NULL && probe->calls == 1u) ||
        (context->event == NULL && probe->stage_initial)) {
        for (index = 0u; index < probe->stage_count; ++index) {
            const char *stage_error = NULL;
            cflow_statechart_effect_ticket ticket;
            check(index < probe->terminal_capacity);
            probe->terminals[index].owner = probe;
            ticket = (cflow_statechart_effect_ticket){
                effect_ticket_commit,
                effect_ticket_discard,
                &probe->terminals[index]};
            ++probe->prepared;
            if (!context->stage_effect(
                    context->effect_user, &ticket, &stage_error)) {
                ticket.discard(ticket.user);
                *out_error = stage_error;
                return false;
            }
        }
        if (probe->block_after_stage) {
            atomic_store(&probe->block_entered, true);
            while (!atomic_load(&probe->block_release)) cmeta_thread_yield();
        }
    }
    if (context->event == NULL) {
        if (probe->fail_initial) {
            *out_error = "initial effect action failure";
            return false;
        }
        return true;
    }
    if (probe->calls == probe->fail_call) {
        *out_error = "effect action failure";
        return false;
    }
    return true;
}

typedef struct microstep_marker_binding {
    microstep_action_probe *probe;
    int marker;
} microstep_marker_binding;

static bool microstep_marked_action(
    void *user, cflow_statechart_action_phase phase,
    cflow_machine_state_id owner, const void *state,
    const cflow_event_view *event, void *out_state,
    cflow_statechart_raise_fn raise_internal, void *raise_user,
    const char **out_error) {
    microstep_marker_binding *binding = (microstep_marker_binding *)user;
    size_t call;
    bool succeeded;
    if (binding == NULL || binding->probe == NULL) return false;
    call = binding->probe->calls;
    succeeded = microstep_action(
        binding->probe, phase, owner, state, event, out_state,
        raise_internal, raise_user, out_error);
    if (binding->probe->calls > call)
        binding->probe->markers[call] = binding->marker;
    return succeeded;
}

static void microstep_fixture_definition(microstep_fixture *fixture,
                                         bool cross_regions,
                                         bool targetless,
                                         cflow_statechart_transition_kind kind) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        MICRO_ROOT, 0u, CFLOW_STATECHART_PARALLEL, 0u};
    fixture->states[1] = (cflow_statechart_state){
        MICRO_LEFT, MICRO_ROOT, CFLOW_STATECHART_COMPOUND, 1u};
    fixture->states[2] = (cflow_statechart_state){
        MICRO_LEFT_INITIAL, MICRO_LEFT, CFLOW_STATECHART_INITIAL, 2u};
    fixture->states[3] = (cflow_statechart_state){
        MICRO_LEFT_NESTED, MICRO_LEFT, CFLOW_STATECHART_COMPOUND, 3u};
    fixture->states[4] = (cflow_statechart_state){
        MICRO_NESTED_INITIAL, MICRO_LEFT_NESTED,
        CFLOW_STATECHART_INITIAL, 4u};
    fixture->states[5] = (cflow_statechart_state){
        MICRO_LEFT_A, MICRO_LEFT_NESTED, CFLOW_STATECHART_ATOMIC, 5u};
    fixture->states[6] = (cflow_statechart_state){
        MICRO_LEFT_B, MICRO_LEFT_NESTED, CFLOW_STATECHART_ATOMIC, 6u};
    fixture->states[7] = (cflow_statechart_state){
        MICRO_RIGHT, MICRO_ROOT, CFLOW_STATECHART_COMPOUND, 7u};
    fixture->states[8] = (cflow_statechart_state){
        MICRO_RIGHT_INITIAL, MICRO_RIGHT, CFLOW_STATECHART_INITIAL, 8u};
    fixture->states[9] = (cflow_statechart_state){
        MICRO_RIGHT_C, MICRO_RIGHT, CFLOW_STATECHART_ATOMIC, 9u};
    fixture->states[10] = (cflow_statechart_state){
        MICRO_RIGHT_D, MICRO_RIGHT, CFLOW_STATECHART_ATOMIC, 10u};
    fixture->events[0] = (cflow_event_type){MICRO_EVENT, &cmeta_type_int};
    fixture->events[1] = (cflow_event_type){
        MICRO_RAISED_EVENT, &cmeta_type_int};
    fixture->transitions[0] = (cflow_statechart_transition){
        1u, MICRO_LEFT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, MICRO_LEFT_NESTED,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        2u, MICRO_NESTED_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, MICRO_LEFT_A,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        3u, MICRO_RIGHT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, MICRO_RIGHT_C,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
    fixture->transitions[3] = (cflow_statechart_transition){
        MICRO_LEFT_TRANSITION, MICRO_LEFT_A,
        CFLOW_STATECHART_TRIGGER_EVENT, MICRO_EVENT, 0u, 0u,
        targetless ? 0u : MICRO_LEFT_B, kind, 0u, 3u};
    fixture->transitions[4] = (cflow_statechart_transition){
        MICRO_RIGHT_TRANSITION, MICRO_RIGHT_C,
        CFLOW_STATECHART_TRIGGER_EVENT, MICRO_EVENT, 0u, 0u,
        MICRO_RIGHT_D, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 4u};
    fixture->executables[0] = (cflow_statechart_executable){
        MICRO_EXECUTABLE, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
    fixture->state_actions[0] = (cflow_statechart_state_action){
        MICRO_LEFT_A, CFLOW_STATECHART_STATE_ACTION_EXIT,
        MICRO_EXECUTABLE, 0u};
    fixture->state_actions[1] = (cflow_statechart_state_action){
        MICRO_LEFT_B, CFLOW_STATECHART_STATE_ACTION_ENTRY,
        MICRO_EXECUTABLE, 0u};
    fixture->state_actions[2] = (cflow_statechart_state_action){
        MICRO_RIGHT_C, CFLOW_STATECHART_STATE_ACTION_EXIT,
        MICRO_EXECUTABLE, 0u};
    fixture->state_actions[3] = (cflow_statechart_state_action){
        MICRO_RIGHT_C, CFLOW_STATECHART_STATE_ACTION_ENTRY,
        MICRO_EXECUTABLE, 0u};
    fixture->state_actions[4] = (cflow_statechart_state_action){
        MICRO_RIGHT_D, CFLOW_STATECHART_STATE_ACTION_ENTRY,
        MICRO_EXECUTABLE, 0u};
    fixture->transition_actions[0] = (cflow_statechart_transition_action){
        MICRO_LEFT_TRANSITION, MICRO_EXECUTABLE, 0u};
    fixture->transition_actions[1] = (cflow_statechart_transition_action){
        MICRO_RIGHT_TRANSITION, MICRO_EXECUTABLE, 0u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 11u, fixture->events, 2u,
        NULL, 0u, fixture->executables, 1u, fixture->transitions,
        cross_regions ? 5u : 4u, fixture->state_actions, 5u,
        fixture->transition_actions, cross_regions ? 2u : 1u};
    fixture->initial_state = 10;
}

static cflow_statechart_instance_status microstep_fixture_init(
    microstep_fixture *fixture, microstep_action_probe *probe,
    size_t internal_event_capacity) {
    cflow_statechart_instance_config config;
    const cflow_statechart_executable_binding binding = {
        MICRO_EXECUTABLE, microstep_action, probe};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    probe->executor = &fixture->executor;
    probe->executor_only = true;
    probe->no_alias = true;
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .executables = &binding,
        .executable_count = 1u,
        .external_event_capacity = 4u,
        .internal_event_capacity = internal_event_capacity,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static cflow_statechart_instance_status microstep_fixture_init_with_binding(
    microstep_fixture *fixture,
    const cflow_statechart_executable_binding *binding,
    size_t binding_count,
    size_t internal_event_capacity,
    size_t executor_capacity) {
    cflow_statechart_instance_config config;
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(executor_capacity == 0u
        ? cflow_executor_serial_init(&fixture->executor)
        : cflow_executor_serial_init_with_capacity(
              &fixture->executor, executor_capacity));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .executables = binding,
        .executable_count = binding_count,
        .external_event_capacity = 4u,
        .internal_event_capacity = internal_event_capacity,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static cflow_statechart_instance_status microstep_fixture_init_with_effects(
    microstep_fixture *fixture, effect_action_probe *probe,
    size_t effect_capacity) {
    cflow_statechart_instance_config config;
    const cflow_statechart_executable_binding binding = {
        .id = MICRO_EXECUTABLE,
        .user = probe,
        .contextual_fn = effect_staging_action};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    probe->instance = &fixture->instance;
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .executables = &binding,
        .executable_count = 1u,
        .external_event_capacity = 4u,
        .internal_event_capacity = 2u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .effect_capacity = effect_capacity,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static void microstep_fixture_destroy(microstep_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

static cflow_admission_status microstep_submit_event(
    microstep_fixture *fixture,
    cflow_statechart_selection_snapshot *selection) {
    static const int payload = 77;
    const cflow_event_view event = {
        MICRO_EVENT, &cmeta_type_int, &payload};
    const cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
    check_equal(cflow_statechart_instance_select_internal(
                    &fixture->instance, &trigger, selection),
                CFLOW_STATECHART_INSTANCE_OK);
    return cflow_statechart_instance_try_microstep_internal(
        &fixture->instance, &trigger, selection);
}

static void check_microstep_unchanged(
    microstep_fixture *fixture,
    const cflow_machine_state_id *expected,
    size_t expected_count) {
    cflow_machine_state_id actual[8] = {0};
    const cmeta_type_desc *state_type = NULL;
    size_t count = 0u;
    uint64_t version = 0u;
    int state = 0;
    check_equal(cflow_statechart_instance_copy_configuration(
                    &fixture->instance, actual, 8u, &count, &version),
                CFLOW_STATECHART_SNAPSHOT_OK);
    check_equal(count, expected_count);
    check_equal(actual, expected,
                expected_count * sizeof(*expected));
    check_equal(version, UINT64_C(1));
    check_true(cflow_statechart_instance_copy_state(
        &fixture->instance, &state_type, &state, sizeof(state)));
    check_equal(state, 10);
}

typedef struct microstep_executor_blocker {
    atomic_bool entered;
    atomic_bool release;
} microstep_executor_blocker;

typedef enum microstep_raise_case {
    MICROSTEP_RAISE_VALID = 0,
    MICROSTEP_RAISE_UNKNOWN,
    MICROSTEP_RAISE_TYPE_MISMATCH
} microstep_raise_case;

typedef struct microstep_raise_probe {
    microstep_raise_case sequence[4];
    bool results[4];
    const char *errors[4];
    size_t count;
} microstep_raise_probe;

static bool microstep_raise_sequence_action(
    void *user, cflow_statechart_action_phase phase,
    cflow_machine_state_id owner, const void *state,
    const cflow_event_view *event, void *out_state,
    cflow_statechart_raise_fn raise_internal, void *raise_user,
    const char **out_error) {
    microstep_raise_probe *probe = (microstep_raise_probe *)user;
    size_t index;
    (void)owner;
    if (probe != NULL && phase == CFLOW_STATECHART_ACTION_ENTRY &&
        event == NULL && state != NULL && out_state != NULL &&
        out_error != NULL) {
        *(int *)out_state = *(const int *)state;
        *out_error = NULL;
        return true;
    }
    if (probe == NULL || state == NULL || event == NULL || out_state == NULL ||
        raise_internal == NULL || out_error == NULL)
        return false;
    *(int *)out_state = *(const int *)state;
    for (index = 0u; index < probe->count; ++index) {
        int int_payload = 100 + (int)index;
        long long_payload = 200 + (long)index;
        cflow_event_view raised = {
            MICRO_RAISED_EVENT, &cmeta_type_int, &int_payload};
        const char *raise_error = NULL;
        if (probe->sequence[index] == MICROSTEP_RAISE_UNKNOWN)
            raised.id = 999999u;
        else if (probe->sequence[index] == MICROSTEP_RAISE_TYPE_MISMATCH) {
            raised.payload_type = &cmeta_type_long;
            raised.payload = &long_payload;
        }
        probe->results[index] = raise_internal(
            raise_user, &raised, &raise_error);
        probe->errors[index] = raise_error;
    }
    *out_error = probe->errors[probe->count - 1u];
    return false;
}

static void microstep_block_executor(void *user) {
    microstep_executor_blocker *blocker =
        (microstep_executor_blocker *)user;
    atomic_store(&blocker->entered, true);
    while (!atomic_load(&blocker->release)) cmeta_thread_yield();
}

static void microstep_noop(void *user) {
    (void)user;
}

static void check_raise_latch_case(
    const microstep_raise_case *sequence, size_t sequence_count,
    const bool *expected_results,
    cflow_statechart_instance_status expected_status,
    const char *expected_error) {
    microstep_fixture fixture;
    microstep_raise_probe probe = {0};
    cflow_statechart_selection_snapshot selection = {0};
    cflow_statechart_microstep_stats stats = {0};
    const cflow_statechart_executable_binding binding = {
        MICRO_EXECUTABLE, microstep_raise_sequence_action, &probe};
    size_t index;
    microstep_fixture_definition(
        &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
    for (index = 0u; index < sequence_count; ++index)
        probe.sequence[index] = sequence[index];
    probe.count = sequence_count;
    check_equal(microstep_fixture_init_with_binding(
                    &fixture, &binding, 1u, 1u, 0u),
                CFLOW_STATECHART_INSTANCE_OK);
    check_equal(microstep_submit_event(&fixture, &selection),
                CFLOW_ADMISSION_ACCEPTED);
    check_true(cflow_executor_wait_idle(&fixture.executor));
    for (index = 0u; index < sequence_count; ++index) {
        check_equal(probe.results[index], expected_results[index]);
        if (expected_results[index])
            check_null(probe.errors[index]);
        else
            check_equal(probe.errors[index], expected_error);
    }
    check_true(cflow_statechart_instance_get_microstep_stats_internal(
        &fixture.instance, &stats));
    check_equal(stats.last_status, expected_status);
    check_equal(stats.internal_pending, (size_t)0u);
    check_equal(cflow_statechart_instance_error(&fixture.instance),
                expected_error);
    microstep_fixture_destroy(&fixture);
}

typedef struct parallel_entry_fixture {
    cflow_statechart_state states[10];
    cflow_event_type events[1];
    cflow_statechart_executable executables[1];
    cflow_statechart_transition transitions[4];
    cflow_statechart_transition_action transition_actions[2];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_statechart_instance instance;
    int initial_state;
} parallel_entry_fixture;

enum {
    PARALLEL_ENTRY_ROOT = 900u,
    PARALLEL_ENTRY_ROOT_INITIAL = 800u,
    PARALLEL_ENTRY_OUTSIDE = 700u,
    PARALLEL_ENTRY_PARALLEL = 600u,
    PARALLEL_ENTRY_FIRST_REGION = 500u,
    PARALLEL_ENTRY_FIRST_INITIAL = 950u,
    PARALLEL_ENTRY_FIRST_LEAF = 400u,
    PARALLEL_ENTRY_SECOND_REGION = 300u,
    PARALLEL_ENTRY_SECOND_INITIAL = 50u,
    PARALLEL_ENTRY_SECOND_LEAF = 200u
};

static void parallel_entry_fixture_init(
    parallel_entry_fixture *fixture, microstep_action_probe *probe) {
    cflow_statechart_instance_config config;
    const cflow_statechart_executable_binding binding = {
        MICRO_EXECUTABLE, microstep_action, probe};
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        PARALLEL_ENTRY_ROOT, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        PARALLEL_ENTRY_ROOT_INITIAL, PARALLEL_ENTRY_ROOT,
        CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        PARALLEL_ENTRY_OUTSIDE, PARALLEL_ENTRY_ROOT,
        CFLOW_STATECHART_ATOMIC, 2u};
    fixture->states[3] = (cflow_statechart_state){
        PARALLEL_ENTRY_PARALLEL, PARALLEL_ENTRY_ROOT,
        CFLOW_STATECHART_PARALLEL, 3u};
    fixture->states[4] = (cflow_statechart_state){
        PARALLEL_ENTRY_FIRST_REGION, PARALLEL_ENTRY_PARALLEL,
        CFLOW_STATECHART_COMPOUND, 4u};
    fixture->states[5] = (cflow_statechart_state){
        PARALLEL_ENTRY_FIRST_INITIAL, PARALLEL_ENTRY_FIRST_REGION,
        CFLOW_STATECHART_INITIAL, 5u};
    fixture->states[6] = (cflow_statechart_state){
        PARALLEL_ENTRY_FIRST_LEAF, PARALLEL_ENTRY_FIRST_REGION,
        CFLOW_STATECHART_ATOMIC, 6u};
    fixture->states[7] = (cflow_statechart_state){
        PARALLEL_ENTRY_SECOND_REGION, PARALLEL_ENTRY_PARALLEL,
        CFLOW_STATECHART_COMPOUND, 7u};
    fixture->states[8] = (cflow_statechart_state){
        PARALLEL_ENTRY_SECOND_INITIAL, PARALLEL_ENTRY_SECOND_REGION,
        CFLOW_STATECHART_INITIAL, 8u};
    fixture->states[9] = (cflow_statechart_state){
        PARALLEL_ENTRY_SECOND_LEAF, PARALLEL_ENTRY_SECOND_REGION,
        CFLOW_STATECHART_ATOMIC, 9u};
    fixture->events[0] = (cflow_event_type){
        MICRO_EVENT, &cmeta_type_int};
    fixture->executables[0] = (cflow_statechart_executable){
        MICRO_EXECUTABLE, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
    fixture->transitions[0] = (cflow_statechart_transition){
        1u, PARALLEL_ENTRY_ROOT_INITIAL,
        CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u,
        PARALLEL_ENTRY_OUTSIDE, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        2u, PARALLEL_ENTRY_OUTSIDE, CFLOW_STATECHART_TRIGGER_EVENT,
        MICRO_EVENT, 0u, 0u, PARALLEL_ENTRY_PARALLEL,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        3u, PARALLEL_ENTRY_FIRST_INITIAL,
        CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u,
        PARALLEL_ENTRY_FIRST_LEAF,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
    fixture->transitions[3] = (cflow_statechart_transition){
        4u, PARALLEL_ENTRY_SECOND_INITIAL,
        CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u,
        PARALLEL_ENTRY_SECOND_LEAF,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 3u};
    fixture->transition_actions[0] =
        (cflow_statechart_transition_action){3u, MICRO_EXECUTABLE, 0u};
    fixture->transition_actions[1] =
        (cflow_statechart_transition_action){4u, MICRO_EXECUTABLE, 0u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 10u, fixture->events, 1u,
        NULL, 0u, fixture->executables, 1u, fixture->transitions, 4u,
        NULL, 0u, fixture->transition_actions, 2u};
    fixture->initial_state = 10;
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    probe->executor = &fixture->executor;
    probe->executor_only = true;
    probe->no_alias = true;
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .executables = &binding,
        .executable_count = 1u,
        .external_event_capacity = 4u,
        .internal_event_capacity = 2u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    check_equal(cflow_statechart_instance_init(&fixture->instance, &config),
                CFLOW_STATECHART_INSTANCE_OK);
}

static void parallel_entry_fixture_destroy(parallel_entry_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

suite("CFlow Statechart ordered atomic microsteps") {
    it("exposes the exact incremental configuration to contextual actions") {
        microstep_fixture fixture;
        configuration_trace_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const cflow_statechart_executable_binding binding = {
            .id = MICRO_EXECUTABLE,
            .user = &probe,
            .contextual_fn = configuration_trace_action};
        const int expected_initial_trace[] = {
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, MICRO_LEFT_INITIAL),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, MICRO_NESTED_INITIAL),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_ENTRY, MICRO_LEFT_A),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, MICRO_RIGHT_INITIAL),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_ENTRY, MICRO_RIGHT_C)};
        const unsigned int expected_initial_masks[] = {
            0x03u, 0x07u, 0x0fu, 0x2fu, 0x6fu};
        const int expected_microstep_trace[] = {
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_EXIT, MICRO_RIGHT_C),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_EXIT, MICRO_LEFT_A),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_TRANSITION, MICRO_LEFT_A),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_TRANSITION, MICRO_RIGHT_C),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_ENTRY, MICRO_LEFT_B),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_ENTRY, MICRO_RIGHT_D)};
        const unsigned int expected_microstep_masks[] = {
            0x6fu, 0x2fu, 0x27u, 0x27u, 0x37u, 0xb7u};
        microstep_fixture_definition(
            &fixture, true, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        fixture.state_actions[5] = (cflow_statechart_state_action){
            MICRO_LEFT_A, CFLOW_STATECHART_STATE_ACTION_ENTRY,
            MICRO_EXECUTABLE, 0u};
        fixture.definition.state_action_count = 6u;
        fixture.transition_actions[2] =
            (cflow_statechart_transition_action){
                1u, MICRO_EXECUTABLE, 0u};
        fixture.transition_actions[3] =
            (cflow_statechart_transition_action){
                2u, MICRO_EXECUTABLE, 0u};
        fixture.transition_actions[4] =
            (cflow_statechart_transition_action){
                3u, MICRO_EXECUTABLE, 0u};
        fixture.definition.transition_action_count = 5u;
        probe.rejected_unknown_and_pseudo = true;
        check_equal(
            microstep_fixture_init_with_binding(
                &fixture, &binding, 1u, 4u, 0u),
            CFLOW_STATECHART_INSTANCE_OK);
        check_equal(probe.calls, (size_t)5u);
        check_equal(probe.trace, expected_initial_trace,
                    sizeof(expected_initial_trace));
        check_equal(probe.active_masks, expected_initial_masks,
                    sizeof(expected_initial_masks));

        probe.calls = 0u;
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)6u);
        check_equal(probe.trace, expected_microstep_trace,
                    sizeof(expected_microstep_trace));
        check_equal(probe.active_masks, expected_microstep_masks,
                    sizeof(expected_microstep_masks));
        check_true(probe.rejected_unknown_and_pseudo);
        microstep_fixture_destroy(&fixture);
    }
    it("uses one effective internal event capacity for sizing and init") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_storage_requirements default_requirements = {0};
        cflow_statechart_storage_requirements one_requirements = {0};
        cflow_statechart_storage_requirements seventeen_requirements = {0};
        cflow_statechart_instance_config config;
        cflow_statechart_executable_binding binding;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        probe.executor = &fixture.executor;
        probe.executor_only = true;
        probe.no_alias = true;
        binding = (cflow_statechart_executable_binding){
            MICRO_EXECUTABLE, microstep_action, &probe};
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .executables = &binding,
            .executable_count = 1u,
            .external_event_capacity = 4u,
            .internal_event_capacity = 1u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .executor = &fixture.executor};

        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart, 4u, 0u, 4u,
                        &default_requirements),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(default_requirements.internal_event_capacity,
                    (size_t)CFLOW_STATECHART_DEFAULT_INTERNAL_EVENT_CAPACITY);
        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart, 4u, 1u, 4u,
                        &one_requirements),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(one_requirements.internal_event_capacity, (size_t)1u);
        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart, 4u, 17u, 4u,
                        &seventeen_requirements),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(seventeen_requirements.internal_event_capacity,
                    (size_t)17u);
        check_true(one_requirements.total_bytes <
                   default_requirements.total_bytes);
        check_true(default_requirements.total_bytes <
                   seventeen_requirements.total_bytes);

        config.internal_event_capacity = 0u;
        config.max_storage_bytes = default_requirements.total_bytes;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(fixture.instance.impl);
        config.internal_event_capacity = 1u;
        config.max_storage_bytes = one_requirements.total_bytes;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        config.internal_event_capacity = 17u;
        config.max_storage_bytes = seventeen_requirements.total_bytes - 1u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_null(fixture.instance.impl);
        config.max_storage_bytes = seventeen_requirements.total_bytes;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_destroy(&fixture.instance),
                    CFLOW_STATECHART_INSTANCE_OK);

        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart, 4u, SIZE_MAX, 4u,
                        &one_requirements),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_equal(cflow_statechart_instance_storage_requirements_internal(
                        &fixture.statechart,
                        4u, (size_t)CFLOW_STATECHART_MAX_INSTANCE_BYTES, 4u,
                        &one_requirements),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        config.internal_event_capacity =
            (size_t)CFLOW_STATECHART_MAX_INSTANCE_BYTES;
        config.max_storage_bytes = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_null(fixture.instance.impl);
        config.internal_event_capacity = SIZE_MAX;
        config.max_storage_bytes = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("runs cross-region exits transitions and entries in exact global order") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int expected_trace[] = {
            300, 700, 10700, 10300, 20690, 20290};
        const int expected_states[] = {10, 11, 12, 13, 14, 15};
        const cflow_machine_state_id expected_configuration[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_B,
            MICRO_RIGHT, MICRO_RIGHT_D};
        cflow_machine_state_id actual[6] = {0};
        const cmeta_type_desc *state_type = NULL;
        cflow_statechart_microstep_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        int state = 0;
        microstep_fixture_definition(
            &fixture, true, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)6u);
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        check_equal(probe.input_states, expected_states,
                    sizeof(expected_states));
        check_true(probe.executor_only);
        check_true(probe.no_alias);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 6u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(actual, expected_configuration,
                    sizeof(expected_configuration));
        check_equal(version, UINT64_C(2));
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &state_type, &state, sizeof(state)));
        check_equal(state, 16);
        check_true(cflow_statechart_instance_get_microstep_stats_internal(
            &fixture.instance, &stats));
        check_equal(stats.accepted, UINT64_C(1));
        check_equal(stats.completed, UINT64_C(1));
        check_equal(stats.failed, UINT64_C(0));
        check_equal(stats.cancelled, UINT64_C(0));
        check_equal(stats.finalized, UINT64_C(1));
        microstep_fixture_destroy(&fixture);
    }

    it("runs a targetless transition action without exit or entry") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int expected_trace[] = {
            microstep_trace_code(CFLOW_STATECHART_ACTION_TRANSITION,
                                 MICRO_LEFT_A)};
        const cflow_machine_state_id expected_configuration[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_A,
            MICRO_RIGHT, MICRO_RIGHT_C};
        cflow_machine_state_id actual[6] = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        microstep_fixture_definition(
            &fixture, false, true, CFLOW_STATECHART_TRANSITION_INTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)1u);
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, actual, 6u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(actual, expected_configuration,
                    sizeof(expected_configuration));
        check_equal(version, UINT64_C(2));
        microstep_fixture_destroy(&fixture);
    }

    it("keeps compound source on internal descendant transition but reenters external siblings") {
        microstep_fixture internal_fixture, external_fixture;
        microstep_action_probe internal_probe = {0}, external_probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int internal_trace[] = {
            microstep_trace_code(CFLOW_STATECHART_ACTION_EXIT, MICRO_LEFT_A),
            microstep_trace_code(CFLOW_STATECHART_ACTION_TRANSITION,
                                 MICRO_LEFT),
            microstep_trace_code(CFLOW_STATECHART_ACTION_ENTRY, MICRO_LEFT_B)};
        const int external_trace[] = {
            microstep_trace_code(CFLOW_STATECHART_ACTION_EXIT, MICRO_RIGHT_C),
            microstep_trace_code(CFLOW_STATECHART_ACTION_EXIT, MICRO_LEFT_A),
            microstep_trace_code(CFLOW_STATECHART_ACTION_TRANSITION,
                                 MICRO_LEFT),
            microstep_trace_code(CFLOW_STATECHART_ACTION_ENTRY, MICRO_LEFT_B),
            microstep_trace_code(CFLOW_STATECHART_ACTION_ENTRY, MICRO_RIGHT_C)};
        const cflow_machine_state_id expected[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_B,
            MICRO_RIGHT, MICRO_RIGHT_C};
        microstep_fixture_definition(
            &internal_fixture, false, false,
            CFLOW_STATECHART_TRANSITION_INTERNAL);
        internal_fixture.transitions[3].source = MICRO_LEFT;
        check_equal(microstep_fixture_init(
                        &internal_fixture, &internal_probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&internal_fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&internal_fixture.executor));
        check_equal(internal_probe.trace, internal_trace,
                    sizeof(internal_trace));
        microstep_fixture_destroy(&internal_fixture);

        microstep_fixture_definition(
            &external_fixture, false, false,
            CFLOW_STATECHART_TRANSITION_EXTERNAL);
        external_fixture.transitions[3].source = MICRO_LEFT;
        check_equal(microstep_fixture_init(
                        &external_fixture, &external_probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&external_fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&external_fixture.executor));
        check_equal(external_probe.trace, external_trace,
                    sizeof(external_trace));
        {
            cflow_machine_state_id actual[6] = {0};
            size_t count = 0u;
            uint64_t version = 0u;
            check_equal(cflow_statechart_instance_copy_configuration(
                            &external_fixture.instance, actual, 6u,
                            &count, &version),
                        CFLOW_STATECHART_SNAPSHOT_OK);
            check_equal(actual, expected, sizeof(expected));
            check_equal(version, UINT64_C(2));
        }
        microstep_fixture_destroy(&external_fixture);
    }

    it("copies raised event payload before callback return and commits once") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        cflow_event_id id = 0u;
        const cmeta_type_desc *type = NULL;
        cflow_statechart_microstep_stats stats = {0};
        int payload = 0;
        probe.raise_call = 1u;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        {
            const int trigger_payload = 77;
            const cflow_event_view event = {
                MICRO_EVENT, &cmeta_type_int, &trigger_payload};
            const cflow_statechart_selection_trigger trigger = {
                CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
            check_equal(cflow_statechart_instance_select_internal(
                            &fixture.instance, &trigger, &selection),
                        CFLOW_STATECHART_INSTANCE_OK);
            check_equal(cflow_statechart_instance_try_microstep_internal(
                            &fixture.instance, &trigger, &selection),
                        CFLOW_ADMISSION_ACCEPTED);
        }
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_internal_event_internal(
                        &fixture.instance, 0u, &id, &type,
                        &payload, sizeof(payload)),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(id, (cflow_event_id)MICRO_RAISED_EVENT);
        check_true(cmeta_type_equal(type, &cmeta_type_int));
        check_equal(payload, 701);
        check_true(cflow_statechart_instance_get_microstep_stats_internal(
            &fixture.instance, &stats));
        check_equal(stats.internal_pending, (size_t)1u);
        microstep_fixture_destroy(&fixture);
    }

    it("owns the trigger and freezes selection while accepted work is pending") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        cflow_statechart_selection_snapshot rejected = {0};
        int payload = 77;
        cflow_event_view event = {MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        probe.block_first = true;
        atomic_init(&probe.block_entered, false);
        atomic_init(&probe.block_release, false);
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&probe.block_entered)) cmeta_thread_yield();
        payload = 99;
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &rejected),
                    CFLOW_STATECHART_INSTANCE_WOULD_BLOCK);
        check_equal(rejected.transition_count, (size_t)0u);
        atomic_store(&probe.block_release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.observed_event_payload, 77);
        microstep_fixture_destroy(&fixture);
    }

    it("binds the selection-time payload before reservation") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        int payload = 77;
        cflow_event_view event = {MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        payload = 99;
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.observed_event_payload, 77);
        microstep_fixture_destroy(&fixture);
    }

    it("rejects an overwritten selection generation") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot old_selection = {0};
        cflow_statechart_selection_snapshot current_selection = {0};
        const int payload = 77;
        const cflow_event_view event = {
            MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        cflow_admission_status admission;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &old_selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &current_selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        admission = cflow_statechart_instance_try_microstep_internal(
            &fixture.instance, &trigger, &old_selection);
        check_equal(admission, CFLOW_ADMISSION_INVALID_ARGUMENT);
        if (admission == CFLOW_ADMISSION_ACCEPTED)
            check_true(cflow_executor_wait_idle(&fixture.executor));
        else {
            check_equal(cflow_statechart_instance_try_microstep_internal(
                            &fixture.instance, &trigger, &current_selection),
                        CFLOW_ADMISSION_ACCEPTED);
            check_true(cflow_executor_wait_idle(&fixture.executor));
        }
        microstep_fixture_destroy(&fixture);
    }

    it("rejects selection reuse after its commit") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int payload = 77;
        const cflow_event_view event = {
            MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        cflow_admission_status admission;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        admission = cflow_statechart_instance_try_microstep_internal(
            &fixture.instance, &trigger, &selection);
        check_equal(admission, CFLOW_ADMISSION_INVALID_ARGUMENT);
        if (admission == CFLOW_ADMISSION_ACCEPTED)
            check_true(cflow_executor_wait_idle(&fixture.executor));
        microstep_fixture_destroy(&fixture);
    }

    it("rejects a selection submitted with a different event ID") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int payload = 77;
        const cflow_event_view selected_event = {
            MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_event_view different_event = {
            MICRO_RAISED_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger selected_trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &selected_event, 0u};
        const cflow_statechart_selection_trigger different_trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &different_event, 0u};
        cflow_admission_status admission;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &selected_trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        admission = cflow_statechart_instance_try_microstep_internal(
            &fixture.instance, &different_trigger, &selection);
        check_equal(admission, CFLOW_ADMISSION_INVALID_ARGUMENT);
        if (admission == CFLOW_ADMISSION_ACCEPTED)
            check_true(cflow_executor_wait_idle(&fixture.executor));
        microstep_fixture_destroy(&fixture);
    }

    it("rejects a selection token from another instance") {
        microstep_fixture first, second;
        microstep_action_probe first_probe = {0}, second_probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int payload = 77;
        const cflow_event_view event = {
            MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        microstep_fixture_definition(
            &first, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        microstep_fixture_definition(
            &second, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&first, &first_probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_fixture_init(&second, &second_probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_select_internal(
                        &first.instance, &trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &second.instance, &trigger, &selection),
                    CFLOW_ADMISSION_INVALID_ARGUMENT);
        microstep_fixture_destroy(&second);
        microstep_fixture_destroy(&first);
    }

    it("retries the same generation after executor FULL rejection") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        microstep_executor_blocker blocker;
        cflow_statechart_selection_snapshot selection = {0};
        const cflow_statechart_executable_binding binding = {
            MICRO_EXECUTABLE, microstep_action, &probe};
        const cflow_executor_task blocking_task = {
            microstep_block_executor, NULL, NULL, &blocker};
        const cflow_executor_task queued_task = {
            microstep_noop, NULL, NULL, NULL};
        const int payload = 77;
        const cflow_event_view event = {
            MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_binding(
                        &fixture, &binding, 1u, 2u, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        probe.executor = &fixture.executor;
        probe.executor_only = true;
        probe.no_alias = true;
        check_equal(cflow_executor_try_post_task(
                        &fixture.executor, &blocking_task),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_executor_try_post_task(
                        &fixture.executor, &queued_task),
                    CFLOW_ADMISSION_ACCEPTED);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_ADMISSION_FULL);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.observed_event_payload, 77);
        microstep_fixture_destroy(&fixture);
    }

    it("runs multiple actions on one owner in row order") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        microstep_marker_binding first = {&probe, 101};
        microstep_marker_binding second = {&probe, 202};
        cflow_statechart_selection_snapshot selection = {0};
        cflow_statechart_executable_binding bindings[2];
        const int expected_trace[] = {
            700, 700, 10700, 20690};
        const int expected_markers[] = {101, 202, 101, 101};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        fixture.executables[1] = (cflow_statechart_executable){
            MICRO_SECOND_EXECUTABLE, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.executable_count = 2u;
        fixture.state_actions[5] = (cflow_statechart_state_action){
            MICRO_LEFT_A, CFLOW_STATECHART_STATE_ACTION_EXIT,
            MICRO_SECOND_EXECUTABLE, 1u};
        fixture.definition.state_action_count = 6u;
        bindings[0] = (cflow_statechart_executable_binding){
            MICRO_EXECUTABLE, microstep_marked_action, &first};
        bindings[1] = (cflow_statechart_executable_binding){
            MICRO_SECOND_EXECUTABLE, microstep_marked_action, &second};
        check_equal(microstep_fixture_init_with_binding(
                        &fixture, bindings, 2u, 2u, 0u),
                    CFLOW_STATECHART_INSTANCE_OK);
        probe.executor = &fixture.executor;
        probe.executor_only = true;
        probe.no_alias = true;
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        check_equal(probe.markers, expected_markers, sizeof(expected_markers));
        check_equal(probe.calls, (size_t)4u);
        microstep_fixture_destroy(&fixture);
    }

    it("runs newly entered parallel defaults in document order") {
        parallel_entry_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const int payload = 77;
        const cflow_event_view event = {
            MICRO_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_selection_trigger trigger = {
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        const int expected_trace[] = {
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL,
                PARALLEL_ENTRY_FIRST_INITIAL),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL,
                PARALLEL_ENTRY_SECOND_INITIAL)};
        parallel_entry_fixture_init(&fixture, &probe);
        check_equal(cflow_statechart_instance_select_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_microstep_internal(
                        &fixture.instance, &trigger, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        check_true(probe.executor_only);
        check_true(probe.no_alias);
        parallel_entry_fixture_destroy(&fixture);
    }

    it("keeps the first raise failure across later raise attempts") {
        const microstep_raise_case unknown_first[] = {
            MICROSTEP_RAISE_UNKNOWN, MICROSTEP_RAISE_TYPE_MISMATCH,
            MICROSTEP_RAISE_VALID};
        const microstep_raise_case mismatch_first[] = {
            MICROSTEP_RAISE_TYPE_MISMATCH, MICROSTEP_RAISE_UNKNOWN,
            MICROSTEP_RAISE_VALID};
        const microstep_raise_case full_first[] = {
            MICROSTEP_RAISE_VALID, MICROSTEP_RAISE_VALID,
            MICROSTEP_RAISE_TYPE_MISMATCH, MICROSTEP_RAISE_UNKNOWN};
        const bool all_failed[] = {false, false, false};
        const bool full_results[] = {true, false, false, false};
        check_raise_latch_case(
            unknown_first, 3u, all_failed,
            CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_INVALID,
            "Statechart internal event is unknown");
        check_raise_latch_case(
            mismatch_first, 3u, all_failed,
            CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_TYPE_MISMATCH,
            "Statechart internal event type mismatch");
        check_raise_latch_case(
            full_first, 4u, full_results,
            CFLOW_STATECHART_INSTANCE_INTERNAL_QUEUE_FULL,
            "Statechart internal event queue is full");
    }

    it("commits a staged effect only after publishing the microstep") {
        microstep_fixture fixture;
        effect_terminal_probe terminals[1] = {0};
        effect_action_probe probe = {
            .terminals = terminals,
            .terminal_capacity = 1u,
            .stage_count = 1u,
            .expected_commit_version = UINT64_C(2),
            .commit_observed_published_configuration = true};
        cflow_statechart_selection_snapshot selection = {0};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_effects(
                        &fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.prepared, (size_t)1u);
        check_equal(probe.committed, (size_t)1u);
        check_equal(probe.discarded, (size_t)0u);
        check_true(probe.commit_observed_published_configuration);
        microstep_fixture_destroy(&fixture);
    }

    it("commits a staged effect from successful initial entry") {
        microstep_fixture fixture;
        effect_terminal_probe terminals[1] = {0};
        effect_action_probe probe = {
            .terminals = terminals,
            .terminal_capacity = 1u,
            .stage_count = 1u,
            .stage_initial = true,
            .commit_observed_published_configuration = true};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_effects(
                        &fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(probe.prepared, (size_t)1u);
        check_equal(probe.committed, (size_t)1u);
        check_equal(probe.discarded, (size_t)0u);
        microstep_fixture_destroy(&fixture);
    }

    it("discards a staged effect when initial entry fails") {
        microstep_fixture fixture;
        effect_terminal_probe terminals[1] = {0};
        effect_action_probe probe = {
            .terminals = terminals,
            .terminal_capacity = 1u,
            .stage_count = 1u,
            .stage_initial = true,
            .fail_initial = true,
            .commit_observed_published_configuration = true};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_effects(
                        &fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_ACTION_FAILED);
        check_equal(probe.prepared, (size_t)1u);
        check_equal(probe.committed, (size_t)0u);
        check_equal(probe.discarded, (size_t)1u);
        microstep_fixture_destroy(&fixture);
    }

    it("discards staged effects when a later action rolls back") {
        microstep_fixture fixture;
        effect_terminal_probe terminals[1] = {0};
        effect_action_probe probe = {
            .terminals = terminals,
            .terminal_capacity = 1u,
            .stage_count = 1u,
            .fail_call = 2u,
            .commit_observed_published_configuration = true};
        cflow_statechart_selection_snapshot selection = {0};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_effects(
                        &fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.prepared, (size_t)1u);
        check_equal(probe.committed, (size_t)0u);
        check_equal(probe.discarded, (size_t)1u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "effect action failure");
        microstep_fixture_destroy(&fixture);
    }

    it("rejects effect capacity plus one and discards every ticket") {
        microstep_fixture fixture;
        effect_terminal_probe terminals[2] = {0};
        effect_action_probe probe = {
            .terminals = terminals,
            .terminal_capacity = 2u,
            .stage_count = 2u,
            .commit_observed_published_configuration = true};
        cflow_statechart_selection_snapshot selection = {0};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_effects(
                        &fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.prepared, (size_t)2u);
        check_equal(probe.committed, (size_t)0u);
        check_equal(probe.discarded, (size_t)2u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart effect journal is full");
        microstep_fixture_destroy(&fixture);
    }

    it("discards a staged effect when cancellation wins before commit") {
        microstep_fixture fixture;
        effect_terminal_probe terminals[1] = {0};
        effect_action_probe probe = {
            .terminals = terminals,
            .terminal_capacity = 1u,
            .stage_count = 1u,
            .block_after_stage = true,
            .commit_observed_published_configuration = true};
        cflow_statechart_selection_snapshot selection = {0};
        atomic_init(&probe.block_entered, false);
        atomic_init(&probe.block_release, false);
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init_with_effects(
                        &fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&probe.block_entered)) cmeta_thread_yield();
        cflow_statechart_instance_cancel(&fixture.instance);
        atomic_store(&probe.block_release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.prepared, (size_t)1u);
        check_equal(probe.committed, (size_t)0u);
        check_equal(probe.discarded, (size_t)1u);
        microstep_fixture_destroy(&fixture);
    }

    it("rolls back exit failure and settles accepted work once") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        cflow_statechart_microstep_stats stats = {0};
        const cflow_machine_state_id expected[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_A,
            MICRO_RIGHT, MICRO_RIGHT_C};
        probe.fail_call = 1u;
        probe.raise_call = 1u;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)1u);
        check_microstep_unchanged(&fixture, expected, 6u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "first action failure");
        check_true(cflow_statechart_instance_get_microstep_stats_internal(
            &fixture.instance, &stats));
        check_equal(stats.accepted, UINT64_C(1));
        check_equal(stats.failed, UINT64_C(1));
        check_equal(stats.completed, UINT64_C(0));
        check_equal(stats.finalized, UINT64_C(1));
        check_equal(stats.internal_pending, (size_t)0u);
        microstep_fixture_destroy(&fixture);
    }

    it("rolls back transition failure before later actions") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const cflow_machine_state_id expected[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_A,
            MICRO_RIGHT, MICRO_RIGHT_C};
        probe.fail_call = 2u;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)2u);
        check_microstep_unchanged(&fixture, expected, 6u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "first action failure");
        microstep_fixture_destroy(&fixture);
    }

    it("rolls back entry failure after staging the target configuration") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        const cflow_machine_state_id expected[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_A,
            MICRO_RIGHT, MICRO_RIGHT_C};
        probe.fail_call = 3u;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)3u);
        check_microstep_unchanged(&fixture, expected, 6u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "first action failure");
        microstep_fixture_destroy(&fixture);
    }

    it("reports staged internal queue full and publishes nothing") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        cflow_statechart_microstep_stats stats = {0};
        const cflow_machine_state_id expected[] = {
            MICRO_ROOT, MICRO_LEFT, MICRO_LEFT_NESTED, MICRO_LEFT_A,
            MICRO_RIGHT, MICRO_RIGHT_C};
        probe.raise_call = 1u;
        probe.raise_twice = true;
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        {
            const int trigger_payload = 77;
            const cflow_event_view event = {
                MICRO_EVENT, &cmeta_type_int, &trigger_payload};
            const cflow_statechart_selection_trigger trigger = {
                CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
            check_equal(cflow_statechart_instance_select_internal(
                            &fixture.instance, &trigger, &selection),
                        CFLOW_STATECHART_INSTANCE_OK);
            check_equal(cflow_statechart_instance_try_microstep_internal(
                            &fixture.instance, &trigger, &selection),
                        CFLOW_ADMISSION_ACCEPTED);
        }
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_microstep_unchanged(&fixture, expected, 6u);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart internal event queue is full");
        check_true(cflow_statechart_instance_get_microstep_stats_internal(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_INTERNAL_QUEUE_FULL);
        check_equal(stats.internal_pending, (size_t)0u);
        microstep_fixture_destroy(&fixture);
    }

    it("cancels and finalizes one accepted queued microstep exactly once") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        microstep_executor_blocker blocker;
        cflow_statechart_selection_snapshot selection = {0};
        cflow_statechart_microstep_stats stats = {0};
        cflow_executor_control control = {0};
        const cflow_executor_task blocking_task = {
            microstep_block_executor, NULL, NULL, &blocker};
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_executor_try_post_task(
                        &fixture.executor, &blocking_task),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(microstep_submit_event(&fixture, &selection),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_as_control(&fixture.executor, &control));
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(probe.calls, (size_t)0u);
        check_true(cflow_statechart_instance_get_microstep_stats_internal(
            &fixture.instance, &stats));
        check_equal(stats.accepted, UINT64_C(1));
        check_equal(stats.completed, UINT64_C(0));
        check_equal(stats.failed, UINT64_C(0));
        check_equal(stats.cancelled, UINT64_C(1));
        check_equal(stats.finalized, UINT64_C(1));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart SerialExecutor cancelled a queued microstep");
        microstep_fixture_destroy(&fixture);
    }

    it("rolls back reservation when executor rejects without callbacks") {
        microstep_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_statechart_selection_snapshot selection = {0};
        cflow_statechart_microstep_stats stats = {0};
        cflow_executor_control control = {0};
        microstep_fixture_definition(
            &fixture, false, false, CFLOW_STATECHART_TRANSITION_EXTERNAL);
        check_equal(microstep_fixture_init(&fixture, &probe, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_executor_as_control(&fixture.executor, &control));
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN));
        check_true(cflow_executor_wait_idle(&fixture.executor));
        {
            const int payload = 77;
            const cflow_event_view event = {
                MICRO_EVENT, &cmeta_type_int, &payload};
            const cflow_statechart_selection_trigger trigger = {
                CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
            check_equal(cflow_statechart_instance_select_internal(
                            &fixture.instance, &trigger, &selection),
                        CFLOW_STATECHART_INSTANCE_OK);
            check_equal(cflow_statechart_instance_try_microstep_internal(
                            &fixture.instance, &trigger, &selection),
                        CFLOW_ADMISSION_CLOSED);
        }
        check_equal(probe.calls, (size_t)0u);
        check_true(cflow_statechart_instance_get_microstep_stats_internal(
            &fixture.instance, &stats));
        check_equal(stats.accepted, UINT64_C(0));
        check_equal(stats.completed, UINT64_C(0));
        check_equal(stats.cancelled, UINT64_C(0));
        check_equal(stats.finalized, UINT64_C(0));
        microstep_fixture_destroy(&fixture);
    }
}

typedef struct history_fixture {
    cflow_statechart_state states[11];
    cflow_event_type events[6];
    cflow_statechart_executable executables[1];
    cflow_statechart_transition transitions[12];
    cflow_statechart_state_action state_actions[1];
    cflow_statechart_transition_action transition_actions[3];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_statechart_instance instance;
    int initial_state;
} history_fixture;

typedef struct history_initial_trace_probe {
    int trace[3];
    size_t calls;
    bool pseudo_states_inactive;
} history_initial_trace_probe;

typedef struct history_chain_failure_probe {
    int trace[2];
    size_t calls;
    size_t prepared;
    size_t committed;
    size_t discarded;
    bool raised;
} history_chain_failure_probe;

enum {
    HISTORY_ROOT = 100u,
    HISTORY_ROOT_INITIAL = 101u,
    HISTORY_PARENT = 200u,
    HISTORY_PARENT_INITIAL = 201u,
    HISTORY_CHILD = 300u,
    HISTORY_CHILD_INITIAL = 301u,
    HISTORY_LEAF_ONE = 310u,
    HISTORY_LEAF_TWO = 311u,
    HISTORY_OUTSIDE = 400u,
    HISTORY_SHALLOW = 500u,
    HISTORY_DEEP = 501u,
    HISTORY_MOVE_EVENT = 20u,
    HISTORY_EXIT_EVENT = 21u,
    HISTORY_SHALLOW_EVENT = 22u,
    HISTORY_DEEP_EVENT = 23u,
    HISTORY_BACK_EVENT = 24u,
    HISTORY_FAIL_EXIT_EVENT = 25u
};

static void history_fixture_definition(history_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        HISTORY_ROOT, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        HISTORY_ROOT_INITIAL, HISTORY_ROOT, CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        HISTORY_PARENT, HISTORY_ROOT, CFLOW_STATECHART_COMPOUND, 2u};
    fixture->states[3] = (cflow_statechart_state){
        HISTORY_PARENT_INITIAL, HISTORY_PARENT,
        CFLOW_STATECHART_INITIAL, 3u};
    fixture->states[4] = (cflow_statechart_state){
        HISTORY_CHILD, HISTORY_PARENT, CFLOW_STATECHART_COMPOUND, 4u};
    fixture->states[5] = (cflow_statechart_state){
        HISTORY_CHILD_INITIAL, HISTORY_CHILD,
        CFLOW_STATECHART_INITIAL, 5u};
    fixture->states[6] = (cflow_statechart_state){
        HISTORY_LEAF_ONE, HISTORY_CHILD, CFLOW_STATECHART_ATOMIC, 6u};
    fixture->states[7] = (cflow_statechart_state){
        HISTORY_LEAF_TWO, HISTORY_CHILD, CFLOW_STATECHART_ATOMIC, 7u};
    fixture->states[8] = (cflow_statechart_state){
        HISTORY_SHALLOW, HISTORY_PARENT,
        CFLOW_STATECHART_HISTORY_SHALLOW, 8u};
    fixture->states[9] = (cflow_statechart_state){
        HISTORY_DEEP, HISTORY_PARENT, CFLOW_STATECHART_HISTORY_DEEP, 9u};
    fixture->states[10] = (cflow_statechart_state){
        HISTORY_OUTSIDE, HISTORY_ROOT, CFLOW_STATECHART_ATOMIC, 10u};
    fixture->events[0] = (cflow_event_type){
        HISTORY_MOVE_EVENT, &cmeta_type_int};
    fixture->events[1] = (cflow_event_type){
        HISTORY_EXIT_EVENT, &cmeta_type_int};
    fixture->events[2] = (cflow_event_type){
        HISTORY_SHALLOW_EVENT, &cmeta_type_int};
    fixture->events[3] = (cflow_event_type){
        HISTORY_DEEP_EVENT, &cmeta_type_int};
    fixture->events[4] = (cflow_event_type){
        HISTORY_BACK_EVENT, &cmeta_type_int};
    fixture->events[5] = (cflow_event_type){
        HISTORY_FAIL_EXIT_EVENT, &cmeta_type_int};
    fixture->transitions[0] = (cflow_statechart_transition){
        1u, HISTORY_ROOT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, HISTORY_PARENT,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        2u, HISTORY_PARENT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, HISTORY_CHILD,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        3u, HISTORY_CHILD_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, HISTORY_LEAF_ONE,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
    fixture->transitions[3] = (cflow_statechart_transition){
        4u, HISTORY_SHALLOW, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, HISTORY_CHILD,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 3u};
    fixture->transitions[4] = (cflow_statechart_transition){
        5u, HISTORY_DEEP, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, HISTORY_CHILD,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 4u};
    fixture->transitions[5] = (cflow_statechart_transition){
        6u, HISTORY_LEAF_ONE, CFLOW_STATECHART_TRIGGER_EVENT,
        HISTORY_MOVE_EVENT, 0u, 0u, HISTORY_LEAF_TWO,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 5u};
    fixture->transitions[6] = (cflow_statechart_transition){
        7u, HISTORY_LEAF_TWO, CFLOW_STATECHART_TRIGGER_EVENT,
        HISTORY_EXIT_EVENT, 0u, 0u, HISTORY_OUTSIDE,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 6u};
    fixture->transitions[7] = (cflow_statechart_transition){
        8u, HISTORY_OUTSIDE, CFLOW_STATECHART_TRIGGER_EVENT,
        HISTORY_SHALLOW_EVENT, 0u, 0u, HISTORY_SHALLOW,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 7u};
    fixture->transitions[8] = (cflow_statechart_transition){
        9u, HISTORY_OUTSIDE, CFLOW_STATECHART_TRIGGER_EVENT,
        HISTORY_DEEP_EVENT, 0u, 0u, HISTORY_DEEP,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 8u};
    fixture->transitions[9] = (cflow_statechart_transition){
        10u, HISTORY_LEAF_TWO, CFLOW_STATECHART_TRIGGER_EVENT,
        HISTORY_BACK_EVENT, 0u, 0u, HISTORY_LEAF_ONE,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 9u};
    fixture->transitions[10] = (cflow_statechart_transition){
        11u, HISTORY_LEAF_ONE, CFLOW_STATECHART_TRIGGER_EVENT,
        HISTORY_FAIL_EXIT_EVENT, 0u, 0u, HISTORY_OUTSIDE,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 10u};
    fixture->executables[0] = (cflow_statechart_executable){
        MICRO_EXECUTABLE, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
    fixture->state_actions[0] = (cflow_statechart_state_action){
        HISTORY_LEAF_ONE, CFLOW_STATECHART_STATE_ACTION_EXIT,
        MICRO_EXECUTABLE, 0u};
    fixture->transition_actions[0] =
        (cflow_statechart_transition_action){
            4u, MICRO_EXECUTABLE, 0u};
    fixture->transition_actions[1] =
        (cflow_statechart_transition_action){
            3u, MICRO_EXECUTABLE, 0u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 11u,
        fixture->events, 6u, NULL, 0u,
        fixture->executables, 1u,
        fixture->transitions, 11u,
        fixture->state_actions, 1u,
        fixture->transition_actions, 2u};
    fixture->initial_state = 17;
}

static void history_fixture_init_with_binding(
    history_fixture *fixture,
    const cflow_statechart_executable_binding *binding) {
    cflow_statechart_instance_config config = {
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .executables = binding,
        .executable_count = 1u,
        .external_event_capacity = 4u,
        .internal_event_capacity = 2u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    check_equal(cflow_statechart_instance_init(&fixture->instance, &config),
                CFLOW_STATECHART_INSTANCE_OK);
}

static void history_fixture_init(history_fixture *fixture,
                                 microstep_action_probe *probe) {
    const cflow_statechart_executable_binding binding = {
        MICRO_EXECUTABLE, microstep_action, probe};
    probe->executor = &fixture->executor;
    probe->executor_only = true;
    probe->no_alias = true;
    history_fixture_init_with_binding(fixture, &binding);
}

static bool history_initial_trace_action(
    void *user, const cflow_statechart_executable_context *context,
    const char **out_error) {
    history_initial_trace_probe *probe =
        (history_initial_trace_probe *)user;
    if (probe == NULL || context == NULL || context->state == NULL ||
        context->out_state == NULL || context->is_active == NULL ||
        context->configuration_user == NULL || out_error == NULL ||
        probe->calls >= sizeof(probe->trace) / sizeof(probe->trace[0]))
        return false;
    probe->trace[probe->calls++] =
        microstep_trace_code(context->phase, context->owner);
    probe->pseudo_states_inactive = probe->pseudo_states_inactive &&
        !context->is_active(
            context->configuration_user, HISTORY_PARENT_INITIAL) &&
        !context->is_active(context->configuration_user, HISTORY_SHALLOW);
    *(int *)context->out_state = *(const int *)context->state;
    *out_error = NULL;
    return true;
}

static void history_chain_effect_commit(void *user) {
    history_chain_failure_probe *probe =
        (history_chain_failure_probe *)user;
    if (probe != NULL) ++probe->committed;
}

static void history_chain_effect_discard(void *user) {
    history_chain_failure_probe *probe =
        (history_chain_failure_probe *)user;
    if (probe != NULL) ++probe->discarded;
}

static bool history_chain_failure_action(
    void *user, const cflow_statechart_executable_context *context,
    const char **out_error) {
    history_chain_failure_probe *probe =
        (history_chain_failure_probe *)user;
    if (probe == NULL || context == NULL || context->event != NULL ||
        context->state == NULL || context->out_state == NULL ||
        out_error == NULL ||
        probe->calls >= sizeof(probe->trace) / sizeof(probe->trace[0]))
        return false;
    probe->trace[probe->calls++] =
        microstep_trace_code(context->phase, context->owner);
    *(int *)context->out_state = *(const int *)context->state + 1;
    if (context->phase == CFLOW_STATECHART_ACTION_INITIAL &&
        context->owner == HISTORY_PARENT_INITIAL) {
        const int payload = 73;
        const cflow_event_view event = {
            HISTORY_MOVE_EVENT, &cmeta_type_int, &payload};
        const cflow_statechart_effect_ticket ticket = {
            history_chain_effect_commit,
            history_chain_effect_discard,
            probe};
        const char *stage_error = NULL;
        ++probe->prepared;
        if (context->raise_internal == NULL ||
            !context->raise_internal(
                context->raise_user, &event, &stage_error)) {
            *out_error = stage_error;
            return false;
        }
        probe->raised = true;
        if (context->stage_effect == NULL ||
            !context->stage_effect(
                context->effect_user, &ticket, &stage_error)) {
            ticket.discard(ticket.user);
            *out_error = stage_error;
            return false;
        }
        *out_error = NULL;
        return true;
    }
    if (context->phase == CFLOW_STATECHART_ACTION_HISTORY &&
        context->owner == HISTORY_SHALLOW) {
        *out_error = "history default action failure";
        return false;
    }
    return false;
}

static void history_fixture_destroy(history_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

static void history_microstep(history_fixture *fixture, cflow_event_id id) {
    const int payload = 1;
    const cflow_event_view event = {id, &cmeta_type_int, &payload};
    const cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
    cflow_statechart_selection_snapshot selection = {0};
    check_equal(cflow_statechart_instance_select_internal(
                    &fixture->instance, &trigger, &selection),
                CFLOW_STATECHART_INSTANCE_OK);
    check_equal(selection.transition_count, (size_t)1u);
    check_equal(cflow_statechart_instance_try_microstep_internal(
                    &fixture->instance, &trigger, &selection),
                CFLOW_ADMISSION_ACCEPTED);
    check_true(cflow_executor_wait_idle(&fixture->executor));
}

static void check_history_configuration(
    history_fixture *fixture,
    const cflow_machine_state_id *expected,
    size_t expected_count,
    uint64_t expected_version) {
    cflow_machine_state_id actual[5] = {0};
    size_t count = 0u;
    uint64_t version = 0u;
    check_equal(cflow_statechart_instance_copy_configuration(
                    &fixture->instance, actual, 5u, &count, &version),
                CFLOW_STATECHART_SNAPSHOT_OK);
    check_equal(count, expected_count);
    check_equal(actual, expected, expected_count * sizeof(*expected));
    check_equal(version, expected_version);
}

suite("CFlow Statechart history restoration") {
    it("resolves an initial target through unset sibling history") {
        history_fixture fixture;
        history_initial_trace_probe probe = {
            .pseudo_states_inactive = true};
        const cflow_statechart_executable_binding binding = {
            .id = MICRO_EXECUTABLE,
            .user = &probe,
            .contextual_fn = history_initial_trace_action};
        const cflow_machine_state_id expected_configuration[] = {
            HISTORY_ROOT, HISTORY_PARENT, HISTORY_CHILD, HISTORY_LEAF_ONE};
        const int expected_trace[] = {
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, HISTORY_PARENT_INITIAL),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_HISTORY, HISTORY_SHALLOW),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, HISTORY_CHILD_INITIAL)};
        history_fixture_definition(&fixture);
        fixture.transitions[1].target = HISTORY_SHALLOW;
        fixture.transition_actions[0] =
            (cflow_statechart_transition_action){
                2u, MICRO_EXECUTABLE, 0u};
        fixture.transition_actions[1] =
            (cflow_statechart_transition_action){
                4u, MICRO_EXECUTABLE, 0u};
        fixture.transition_actions[2] =
            (cflow_statechart_transition_action){
                3u, MICRO_EXECUTABLE, 0u};
        fixture.definition.transition_action_count = 3u;
        history_fixture_init_with_binding(&fixture, &binding);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_history_configuration(
            &fixture, expected_configuration, 4u, UINT64_C(1));
        check_equal(probe.calls, (size_t)3u);
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        check_true(probe.pseudo_states_inactive);
        history_fixture_destroy(&fixture);
    }

    it("discards an initial history chain when its second action fails") {
        history_fixture fixture;
        history_chain_failure_probe probe = {0};
        const cflow_statechart_executable_binding binding = {
            .id = MICRO_EXECUTABLE,
            .user = &probe,
            .contextual_fn = history_chain_failure_action};
        cflow_statechart_instance_config config;
        const int expected_trace[] = {
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, HISTORY_PARENT_INITIAL),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_HISTORY, HISTORY_SHALLOW)};
        history_fixture_definition(&fixture);
        fixture.transitions[1].target = HISTORY_SHALLOW;
        fixture.transition_actions[0] =
            (cflow_statechart_transition_action){
                2u, MICRO_EXECUTABLE, 0u};
        fixture.transition_actions[1] =
            (cflow_statechart_transition_action){
                4u, MICRO_EXECUTABLE, 0u};
        fixture.definition.transition_action_count = 2u;
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .executables = &binding,
            .executable_count = 1u,
            .external_event_capacity = 4u,
            .internal_event_capacity = 2u,
            .completion_capacity = 4u,
            .microstep_limit = 64u,
            .effect_capacity = 1u,
            .executor = &fixture.executor};
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_ACTION_FAILED);
        check_null(fixture.instance.impl);
        check_equal(probe.calls, (size_t)2u);
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        check_true(probe.raised);
        check_equal(probe.prepared, (size_t)1u);
        check_equal(probe.committed, (size_t)0u);
        check_equal(probe.discarded, (size_t)1u);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("uses the declared history default when the slot is unset") {
        history_fixture fixture;
        microstep_action_probe probe = {0};
        const cflow_machine_state_id expected[] = {
            HISTORY_ROOT, HISTORY_PARENT, HISTORY_CHILD, HISTORY_LEAF_ONE};
        const int expected_trace[] = {
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_HISTORY, HISTORY_SHALLOW),
            microstep_trace_code(
                CFLOW_STATECHART_ACTION_INITIAL, HISTORY_CHILD_INITIAL)};
        history_fixture_definition(&fixture);
        fixture.transitions[0].target = HISTORY_OUTSIDE;
        history_fixture_init(&fixture, &probe);
        history_microstep(&fixture, HISTORY_SHALLOW_EVENT);
        check_history_configuration(&fixture, expected, 4u, UINT64_C(2));
        check_equal(probe.trace, expected_trace, sizeof(expected_trace));
        history_fixture_destroy(&fixture);
    }

    it("restores shallow history child then enters its default leaf") {
        history_fixture fixture;
        microstep_action_probe probe = {0};
        const cflow_machine_state_id expected[] = {
            HISTORY_ROOT, HISTORY_PARENT, HISTORY_CHILD, HISTORY_LEAF_ONE};
        history_fixture_definition(&fixture);
        history_fixture_init(&fixture, &probe);
        history_microstep(&fixture, HISTORY_MOVE_EVENT);
        history_microstep(&fixture, HISTORY_EXIT_EVENT);
        history_microstep(&fixture, HISTORY_SHALLOW_EVENT);
        check_history_configuration(&fixture, expected, 4u, UINT64_C(4));
        history_fixture_destroy(&fixture);
    }

    it("restores deep history leaf and all of its real ancestors") {
        history_fixture fixture;
        microstep_action_probe probe = {0};
        const cflow_machine_state_id expected[] = {
            HISTORY_ROOT, HISTORY_PARENT, HISTORY_CHILD, HISTORY_LEAF_TWO};
        history_fixture_definition(&fixture);
        history_fixture_init(&fixture, &probe);
        history_microstep(&fixture, HISTORY_MOVE_EVENT);
        history_microstep(&fixture, HISTORY_EXIT_EVENT);
        history_microstep(&fixture, HISTORY_DEEP_EVENT);
        check_history_configuration(&fixture, expected, 4u, UINT64_C(4));
        history_fixture_destroy(&fixture);
    }

    it("does not publish a staged history overwrite when an exit action fails") {
        history_fixture fixture;
        microstep_action_probe probe = {0};
        cflow_machine_state_id before[1] = {0}, after[1] = {0};
        size_t before_count = 0u, after_count = 0u;
        history_fixture_definition(&fixture);
        history_fixture_init(&fixture, &probe);
        history_microstep(&fixture, HISTORY_MOVE_EVENT);
        history_microstep(&fixture, HISTORY_EXIT_EVENT);
        check_equal(cflow_statechart_instance_copy_history_internal(
                        &fixture.instance, HISTORY_DEEP,
                        before, 1u, &before_count),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(before_count, (size_t)1u);
        check_equal(before[0], (cflow_machine_state_id)HISTORY_LEAF_TWO);
        history_microstep(&fixture, HISTORY_DEEP_EVENT);
        history_microstep(&fixture, HISTORY_BACK_EVENT);
        probe.calls = 0u;
        probe.fail_call = 1u;
        history_microstep(&fixture, HISTORY_FAIL_EXIT_EVENT);
        check_equal(cflow_statechart_instance_copy_history_internal(
                        &fixture.instance, HISTORY_DEEP,
                        after, 1u, &after_count),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(after_count, (size_t)1u);
        check_equal(after[0], (cflow_machine_state_id)HISTORY_LEAF_TWO);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "first action failure");
        history_fixture_destroy(&fixture);
    }
}

typedef struct rtc_fixture rtc_fixture;

typedef struct rtc_effect_probe {
    rtc_fixture *fixture;
    int marker;
} rtc_effect_probe;

struct rtc_fixture {
    cflow_statechart_state states[8];
    cflow_event_type events[3];
    cflow_statechart_executable executables[1];
    cflow_statechart_transition transitions[8];
    cflow_statechart_state_action state_actions[2];
    cflow_statechart_transition_action actions[5];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_statechart_instance instance;
    int initial_state;
    int trace[12];
    size_t trace_count;
    bool raise_twice;
    bool raise_on_final_entry;
    bool close_during_action;
    bool cancel_during_action;
    bool block_transition_action;
    atomic_bool transition_action_entered;
    atomic_bool transition_action_release;
    bool queue_cancel_after_commit;
    bool raise_on_other;
    bool fail_action;
    struct microstep_executor_blocker *guard_blocker;
    cflow_statechart_guard guards[1];
    cflow_statechart_instance_hooks hooks;
    const cflow_statechart_instance_hooks *hooks_override;
    size_t stable_hook_calls;
    size_t preprocess_hook_calls;
    uint64_t observed_origin_token;
    uint64_t observed_origin_tokens[64];
    uint64_t observed_configuration_version;
    bool observed_a_active;
    bool observed_d_active;
    bool enqueue_other_once;
    bool drop_tagged_external;
    bool fail_stable_hook;
    bool use_contextual_action;
    bool stage_exit_effect;
    bool fail_exit_action;
    size_t exit_effect_commits;
    size_t exit_effect_discards;
    bool exit_effect_observed_terminal;
    bool raise_tagged_internal;
    uint64_t tagged_internal_token;
    size_t event_hook_calls;
    cflow_statechart_observed_event_kind observed_event_kinds[8];
    uint64_t observed_event_tokens[8];
    cflow_statechart_external_settlement settlements[64];
    size_t settlement_count;
    bool settlement_stats_read_succeeded;
    runtime_shared_executor_probe *settlement_blocker;
    uint64_t blocked_settlement_token;
    atomic_int settlement_callbacks_active;
    atomic_bool settlement_callback_overlap;
    bool cancel_on_first_settlement;
    size_t effect_capacity;
    size_t host_transaction_calls;
    size_t host_transaction_activate_call;
    cflow_statechart_host_result host_transaction_result;
    bool host_transaction_write_state;
    int host_transaction_state;
    bool host_transaction_raise_event;
    bool host_transaction_stage_effects;
    bool host_transaction_stage_invalid_effect;
    bool host_transaction_cancel;
    bool host_transaction_raise_succeeded;
    bool host_transaction_effect_succeeded;
    rtc_effect_probe host_transaction_effects[2];
    int host_transaction_effect_trace[4];
    size_t host_transaction_effect_trace_count;
    bool host_transaction_effect_observed_published;
};

typedef struct rtc_producer_context {
    cflow_statechart_instance *instance;
    size_t count;
    atomic_int *failures;
    atomic_uint_fast64_t *next_token;
} rtc_producer_context;

typedef struct rtc_stats_poller_context {
    cflow_statechart_instance *instance;
    atomic_bool *stop;
    atomic_int *polls;
    atomic_int *violations;
} rtc_stats_poller_context;

enum {
    RTC_ROOT = 100u, RTC_INITIAL = 101u, RTC_A = 110u, RTC_B = 120u,
    RTC_C = 130u, RTC_FINAL = 140u, RTC_D = 150u, RTC_E = 160u,
    RTC_GO = 10u, RTC_NEXT = 11u, RTC_OTHER = 12u, RTC_EXEC = 500u,
    RTC_QUEUE_GUARD = 501u
};

static void rtc_host_effect_commit(void *user) {
    rtc_effect_probe *probe = (rtc_effect_probe *)user;
    rtc_fixture *fixture = probe != NULL ? probe->fixture : NULL;
    const cmeta_type_desc *type = NULL;
    int state = 0;
    if (fixture == NULL || fixture->host_transaction_effect_trace_count >= 4u)
        return;
    fixture->host_transaction_effect_trace[
        fixture->host_transaction_effect_trace_count++] = probe->marker;
    if (fixture->instance.impl != NULL &&
        cflow_statechart_instance_copy_state(
            &fixture->instance, &type, &state, sizeof(state)) &&
        cmeta_type_equal(type, &cmeta_type_int) &&
        state == fixture->host_transaction_state)
        fixture->host_transaction_effect_observed_published = true;
}

static void rtc_host_effect_discard(void *user) {
    rtc_effect_probe *probe = (rtc_effect_probe *)user;
    rtc_fixture *fixture = probe != NULL ? probe->fixture : NULL;
    if (fixture == NULL || fixture->host_transaction_effect_trace_count >= 4u)
        return;
    fixture->host_transaction_effect_trace[
        fixture->host_transaction_effect_trace_count++] = -probe->marker;
}

static cflow_statechart_host_result rtc_host_transaction(
    void *user, cflow_statechart_host_context *context,
    const char **out_error) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    const int payload = 1;
    const cflow_event_view other = {
        RTC_OTHER, &cmeta_type_int, &payload};
    const cflow_statechart_host_phase phase =
        cflow_statechart_host_context_phase(context);
    const cflow_statechart_observed_event *event;
    size_t index;
    if (fixture == NULL || context == NULL || out_error == NULL)
        return CFLOW_STATECHART_HOST_FATAL;
    *out_error = NULL;
    if (phase == CFLOW_STATECHART_HOST_PREPARE_QUIESCENCE) {
        cflow_statechart_effect_ticket ticket;
        int *staged_state;
        ++fixture->stable_hook_calls;
        ++fixture->host_transaction_calls;
        fixture->observed_configuration_version =
            cflow_statechart_host_context_configuration_version(context);
        fixture->observed_a_active =
            cflow_statechart_host_context_is_active(context, RTC_A);
        fixture->observed_d_active =
            cflow_statechart_host_context_is_active(context, RTC_D);
        if (fixture->fail_stable_hook) {
            *out_error = "deliberate host quiescence failure";
            return CFLOW_STATECHART_HOST_FATAL;
        }
        if (fixture->enqueue_other_once) {
            fixture->enqueue_other_once = false;
            if (!cflow_statechart_host_context_raise_internal(
                    context, &other, 0u, out_error))
                return CFLOW_STATECHART_HOST_FATAL;
        }
        if (fixture->host_transaction_calls !=
            fixture->host_transaction_activate_call)
            return CFLOW_STATECHART_HOST_CONTINUE;
        if (fixture->host_transaction_write_state) {
            staged_state = (int *)cflow_statechart_host_context_edit_state(
                context, out_error);
            if (staged_state == NULL)
                return CFLOW_STATECHART_HOST_FATAL;
            *staged_state = fixture->host_transaction_state;
        }
        if (fixture->host_transaction_raise_event) {
            fixture->host_transaction_raise_succeeded =
                cflow_statechart_host_context_raise_internal(
                    context, &other, 0u, out_error);
            if (!fixture->host_transaction_raise_succeeded)
                return CFLOW_STATECHART_HOST_FATAL;
        }
        if (fixture->host_transaction_stage_invalid_effect) {
            ticket = (cflow_statechart_effect_ticket){0};
            fixture->host_transaction_effect_succeeded =
                cflow_statechart_host_context_stage_effect(
                    context, &ticket, out_error);
            if (!fixture->host_transaction_effect_succeeded)
                return CFLOW_STATECHART_HOST_FATAL;
        }
        if (fixture->host_transaction_stage_effects) {
            fixture->host_transaction_effect_succeeded = true;
            for (index = 0u; index < 2u; ++index) {
                fixture->host_transaction_effects[index] =
                    (rtc_effect_probe){fixture, (int)index + 1};
                ticket = (cflow_statechart_effect_ticket){
                    rtc_host_effect_commit,
                    rtc_host_effect_discard,
                    &fixture->host_transaction_effects[index]};
                if (!cflow_statechart_host_context_stage_effect(
                        context, &ticket, out_error)) {
                    fixture->host_transaction_effect_succeeded = false;
                    return CFLOW_STATECHART_HOST_FATAL;
                }
            }
        }
        if (fixture->host_transaction_cancel)
            cflow_statechart_instance_cancel(&fixture->instance);
        if (fixture->host_transaction_result ==
            CFLOW_STATECHART_HOST_FATAL)
            *out_error = "deliberate host transaction failure";
        return fixture->host_transaction_result;
    }
    if (phase != CFLOW_STATECHART_HOST_PREPARE_TRIGGER) {
        *out_error = "unexpected host transaction phase";
        return CFLOW_STATECHART_HOST_FATAL;
    }
    event = cflow_statechart_host_context_trigger(context);
    if (event == NULL) {
        *out_error = "host transaction trigger is unavailable";
        return CFLOW_STATECHART_HOST_FATAL;
    }
    index = fixture->event_hook_calls++;
    if (index < 8u) {
        fixture->observed_event_kinds[index] = event->kind;
        fixture->observed_event_tokens[index] = event->origin_token;
    }
    if (event->kind == CFLOW_STATECHART_OBSERVED_EXTERNAL) {
        if (fixture->preprocess_hook_calls < 64u)
            fixture->observed_origin_tokens[
                fixture->preprocess_hook_calls] = event->origin_token;
        ++fixture->preprocess_hook_calls;
        fixture->observed_origin_token = event->origin_token;
        fixture->observed_configuration_version =
            cflow_statechart_host_context_configuration_version(context);
        if (fixture->drop_tagged_external &&
            event->origin_token != UINT64_C(0))
            return CFLOW_STATECHART_HOST_DROP;
    }
    return CFLOW_STATECHART_HOST_CONTINUE;
}

static void rtc_external_settlement(
    void *user, const cflow_statechart_external_settlement *settlement) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    cflow_statechart_instance_stats stats = {0};
    if (fixture == NULL || settlement == NULL ||
        fixture->settlement_count >= 64u)
        return;
    if (atomic_fetch_add(&fixture->settlement_callbacks_active, 1) != 0)
        atomic_store(&fixture->settlement_callback_overlap, true);
    if (fixture->settlement_blocker != NULL &&
        (fixture->blocked_settlement_token == UINT64_C(0) ||
         fixture->blocked_settlement_token == settlement->origin_token)) {
        atomic_store(
            &fixture->settlement_blocker->blocker_entered, true);
        while (!atomic_load(
                   &fixture->settlement_blocker->blocker_release))
            cmeta_thread_yield();
    }
    fixture->settlement_stats_read_succeeded =
        cflow_statechart_instance_get_stats(&fixture->instance, &stats);
    fixture->settlements[fixture->settlement_count++] = *settlement;
    if (fixture->cancel_on_first_settlement &&
        fixture->settlement_count == 1u)
        cflow_statechart_instance_cancel(&fixture->instance);
    atomic_fetch_sub(&fixture->settlement_callbacks_active, 1);
}

static void rtc_cancel_instance(void *user) {
    cflow_statechart_instance_cancel((cflow_statechart_instance *)user);
}

static bool rtc_action(void *user, cflow_statechart_action_phase phase,
                       cflow_machine_state_id owner, const void *state,
                       const cflow_event_view *event, void *out_state,
                       cflow_statechart_raise_fn raise_internal,
                       void *raise_user, const char **out_error) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    if (fixture == NULL || state == NULL || out_state == NULL ||
        out_error == NULL || fixture->trace_count >= 12u)
        return false;
    fixture->trace[fixture->trace_count++] = (int)owner;
    *(int *)out_state = *(const int *)state + 1;
    *out_error = NULL;
    if (fixture->fail_action) {
        *out_error = "deliberate RTC action failure";
        return false;
    }
    if (fixture->raise_on_final_entry &&
        phase == CFLOW_STATECHART_ACTION_ENTRY && owner == RTC_FINAL) {
        const int payload = 77;
        const cflow_event_view raised = {
            RTC_NEXT, &cmeta_type_int, &payload};
        if (raise_internal == NULL ||
            !raise_internal(raise_user, &raised, out_error))
            return false;
    }
    if (phase == CFLOW_STATECHART_ACTION_TRANSITION && event != NULL &&
        (event->id == RTC_GO ||
         (fixture->raise_on_other && event->id == RTC_OTHER))) {
        const int payload = 77;
        const cflow_event_view raised = {
            RTC_NEXT, &cmeta_type_int, &payload};
        if (raise_internal == NULL ||
            !raise_internal(raise_user, &raised, out_error))
            return false;
        if (fixture->raise_twice &&
            !raise_internal(raise_user, &raised, out_error))
            return false;
        if (event->id == RTC_GO && fixture->close_during_action)
            cflow_statechart_instance_close(&fixture->instance);
        if (event->id == RTC_GO && fixture->cancel_during_action)
            cflow_statechart_instance_cancel(&fixture->instance);
        if (event->id == RTC_GO && fixture->block_transition_action) {
            atomic_store(&fixture->transition_action_entered, true);
            while (!atomic_load(&fixture->transition_action_release))
                cmeta_thread_yield();
        }
        if (event->id == RTC_GO && fixture->queue_cancel_after_commit &&
            cflow_executor_try_post(&fixture->executor, rtc_cancel_instance,
                                    &fixture->instance) !=
                CFLOW_ADMISSION_ACCEPTED)
            return false;
    }
    return true;
}

static void rtc_exit_effect_commit(void *user) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    cflow_machine_state_id states[2] = {0};
    cflow_statechart_instance_stats stats = {0};
    size_t state_count = 0u;
    uint64_t version = 0u;
    if (fixture == NULL) return;
    ++fixture->exit_effect_commits;
    fixture->exit_effect_observed_terminal =
        fixture->exit_effect_observed_terminal &&
        cflow_statechart_instance_copy_configuration(
            &fixture->instance, states, 2u, &state_count, &version) ==
            CFLOW_STATECHART_SNAPSHOT_OK &&
        state_count == 0u &&
        cflow_statechart_instance_get_stats(&fixture->instance, &stats) &&
        stats.done && stats.cancelled;
}

static void rtc_exit_effect_discard(void *user) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    if (fixture != NULL) ++fixture->exit_effect_discards;
}

static bool rtc_contextual_action(
    void *user, const cflow_statechart_executable_context *context,
    const char **out_error) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    if (fixture == NULL || context == NULL || context->state == NULL ||
        context->out_state == NULL || out_error == NULL ||
        fixture->trace_count >= 12u)
        return false;
    fixture->trace[fixture->trace_count++] = (int)context->owner;
    *(int *)context->out_state = *(const int *)context->state + 1;
    *out_error = NULL;
    if (fixture->stage_exit_effect &&
        context->phase == CFLOW_STATECHART_ACTION_EXIT) {
        const cflow_statechart_effect_ticket ticket = {
            rtc_exit_effect_commit, rtc_exit_effect_discard, fixture};
        if (context->stage_effect == NULL ||
            !context->stage_effect(
                context->effect_user, &ticket, out_error)) {
            ticket.discard(ticket.user);
            return false;
        }
    }
    if (fixture->fail_exit_action &&
        context->phase == CFLOW_STATECHART_ACTION_EXIT &&
        context->owner == RTC_ROOT) {
        *out_error = "deliberate controlled exit failure";
        return false;
    }
    if (fixture->raise_tagged_internal &&
        context->phase == CFLOW_STATECHART_ACTION_TRANSITION &&
        context->event != NULL && context->event->id == RTC_GO) {
        const int payload = 77;
        const cflow_event_view raised = {
            RTC_NEXT, &cmeta_type_int, &payload};
        return context->raise_internal_tagged != NULL &&
            context->raise_internal_tagged(
                context->raise_user, &raised,
                fixture->tagged_internal_token, out_error);
    }
    return true;
}

static bool rtc_queue_microstep_guard(void *user, const void *state,
                                      const cflow_event_view *event,
                                      bool *out_enabled,
                                      const char **out_error) {
    rtc_fixture *fixture = (rtc_fixture *)user;
    (void)state;
    (void)event;
    if (fixture == NULL || fixture->guard_blocker == NULL ||
        out_enabled == NULL || out_error == NULL)
        return false;
    *out_error = NULL;
    *out_enabled = cflow_executor_try_post(
        &fixture->executor, microstep_block_executor,
        fixture->guard_blocker) == CFLOW_ADMISSION_ACCEPTED;
    return *out_enabled;
}

static void rtc_producer(void *user) {
    rtc_producer_context *context = (rtc_producer_context *)user;
    const int payload = 1;
    const cflow_event_view event = {
        RTC_OTHER, &cmeta_type_int, &payload};
    size_t index;
    for (index = 0u; index < context->count; ++index) {
        const uint64_t origin_token = (uint64_t)atomic_fetch_add(
            context->next_token, UINT64_C(1));
        if (cflow_statechart_instance_try_send_tagged(
                context->instance, &event, origin_token) != CFLOW_MAILBOX_OK)
            atomic_fetch_add(context->failures, 1);
    }
}

static void rtc_stats_poller(void *user) {
    rtc_stats_poller_context *context = (rtc_stats_poller_context *)user;
    while (!atomic_load(context->stop)) {
        cflow_statechart_instance_stats stats = {0};
        atomic_fetch_add(context->polls, 1);
        if (!cflow_statechart_instance_get_stats(context->instance, &stats) ||
            stats.external_accepted !=
                cflow_statechart_external_identity_sum_internal(
                    stats.external_completed, stats.external_failed,
                    stats.external_cancelled,
                    (uint64_t)stats.external_pending,
                    (uint64_t)stats.external_in_flight))
            atomic_fetch_add(context->violations, 1);
        cmeta_thread_yield();
    }
}

static void rtc_definition(rtc_fixture *fixture,
                           bool root_completion_transition,
                           bool eventless_cycle) {
    size_t transition_count = root_completion_transition ? 7u : 6u;
    size_t action_count = root_completion_transition ? 5u : 4u;
    memset(fixture, 0, sizeof(*fixture));
    atomic_init(&fixture->settlement_callbacks_active, 0);
    atomic_init(&fixture->settlement_callback_overlap, false);
    fixture->states[0] = (cflow_statechart_state){
        RTC_ROOT, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        RTC_INITIAL, RTC_ROOT, CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        RTC_A, RTC_ROOT, CFLOW_STATECHART_ATOMIC, 2u};
    fixture->states[3] = (cflow_statechart_state){
        RTC_B, RTC_ROOT, CFLOW_STATECHART_ATOMIC, 3u};
    fixture->states[4] = (cflow_statechart_state){
        RTC_C, RTC_ROOT, CFLOW_STATECHART_ATOMIC, 4u};
    fixture->states[5] = (cflow_statechart_state){
        RTC_FINAL, RTC_ROOT, CFLOW_STATECHART_FINAL, 5u};
    fixture->states[6] = (cflow_statechart_state){
        RTC_D, RTC_ROOT, CFLOW_STATECHART_ATOMIC, 6u};
    fixture->states[7] = (cflow_statechart_state){
        RTC_E, RTC_ROOT, CFLOW_STATECHART_ATOMIC, 7u};
    fixture->events[0] = (cflow_event_type){RTC_GO, &cmeta_type_int};
    fixture->events[1] = (cflow_event_type){RTC_NEXT, &cmeta_type_int};
    fixture->events[2] = (cflow_event_type){RTC_OTHER, &cmeta_type_int};
    fixture->executables[0] = (cflow_statechart_executable){
        RTC_EXEC, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
    fixture->transitions[0] = (cflow_statechart_transition){
        1u, RTC_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, RTC_A, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        2u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENT,
        RTC_GO, 0u, 0u, RTC_B, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        3u, RTC_B, CFLOW_STATECHART_TRIGGER_EVENT,
        RTC_NEXT, 0u, 0u, RTC_C, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
    fixture->transitions[3] = (cflow_statechart_transition){
        4u, RTC_C, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, eventless_cycle ? 0u : RTC_FINAL,
        eventless_cycle ? CFLOW_STATECHART_TRANSITION_INTERNAL
                        : CFLOW_STATECHART_TRANSITION_EXTERNAL,
        0u, 3u};
    fixture->transitions[4] = (cflow_statechart_transition){
        5u, RTC_D, CFLOW_STATECHART_TRIGGER_EVENT,
        RTC_OTHER, 0u, 0u, RTC_E, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 4u};
    fixture->transitions[5] = (cflow_statechart_transition){
        6u, RTC_E, CFLOW_STATECHART_TRIGGER_EVENT,
        RTC_OTHER, 0u, 0u, RTC_E, CFLOW_STATECHART_TRANSITION_EXTERNAL, 1u, 5u};
    fixture->transitions[6] = (cflow_statechart_transition){
        7u, RTC_ROOT, CFLOW_STATECHART_TRIGGER_COMPLETION,
        0u, RTC_ROOT, 0u, RTC_D,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 6u};
    fixture->actions[0] = (cflow_statechart_transition_action){
        2u, RTC_EXEC, 0u};
    fixture->actions[1] = (cflow_statechart_transition_action){
        3u, RTC_EXEC, 0u};
    fixture->actions[2] = (cflow_statechart_transition_action){
        4u, RTC_EXEC, 0u};
    fixture->actions[3] = (cflow_statechart_transition_action){
        5u, RTC_EXEC, 0u};
    fixture->actions[4] = (cflow_statechart_transition_action){
        7u, RTC_EXEC, 0u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 8u, fixture->events, 3u,
        NULL, 0u, fixture->executables, 1u, fixture->transitions,
        transition_count, NULL, 0u, fixture->actions, action_count};
    fixture->initial_state = 0;
}

static cflow_statechart_instance_status rtc_init_with_external(
    rtc_fixture *fixture, size_t external_capacity,
    size_t adapter_internal_capacity, size_t internal_capacity,
    size_t completion_capacity, size_t microstep_limit,
    size_t executor_capacity) {
    const cflow_statechart_executable_binding binding = {
        .id = RTC_EXEC,
        .fn = fixture->use_contextual_action ? NULL : rtc_action,
        .user = fixture,
        .contextual_fn = fixture->use_contextual_action
            ? rtc_contextual_action : NULL};
    const cflow_statechart_guard_binding guard_binding = {
        RTC_QUEUE_GUARD, rtc_queue_microstep_guard, fixture};
    cflow_statechart_instance_config config = {
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .executables = &binding,
        .executable_count = 1u,
        .guards = fixture->guard_blocker != NULL ? &guard_binding : NULL,
        .guard_count = fixture->guard_blocker != NULL ? 1u : 0u,
        .external_event_capacity = external_capacity,
        .adapter_internal_event_capacity = adapter_internal_capacity,
        .internal_event_capacity = internal_capacity,
        .completion_capacity = completion_capacity,
        .microstep_limit = microstep_limit,
        .executor = &fixture->executor,
        .effect_capacity = fixture->effect_capacity,
        .hooks = fixture->hooks_override != NULL
            ? fixture->hooks_override
            : (fixture->hooks.abi_version != 0u
                ? &fixture->hooks : NULL),
        .hook_user = fixture};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init_with_capacity(
        &fixture->executor, executor_capacity));
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

typedef struct completion_reentry_fixture {
    cflow_statechart_state states[3];
    cflow_statechart_guard guards[1];
    cflow_statechart_executable executables[1];
    cflow_statechart_transition transitions[2];
    cflow_statechart_transition_action actions[1];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_statechart_instance instance;
    int initial_state;
    size_t enabled_count;
    size_t guard_calls;
    cflow_machine_state_id trace[4];
    size_t trace_count;
} completion_reentry_fixture;

enum {
    REENTRY_ROOT = 900u, REENTRY_INITIAL = 901u, REENTRY_FINAL_ONE = 902u,
    REENTRY_FINAL_TWO = 903u, REENTRY_GUARD = 910u, REENTRY_EXEC = 911u
};

static bool completion_reentry_guard(void *user, const void *state,
                                     const cflow_event_view *event,
                                     bool *out_enabled,
                                     const char **out_error) {
    completion_reentry_fixture *fixture =
        (completion_reentry_fixture *)user;
    (void)state;
    if (fixture == NULL || event != NULL || out_enabled == NULL ||
        out_error == NULL)
        return false;
    ++fixture->guard_calls;
    *out_enabled = fixture->guard_calls <= fixture->enabled_count;
    *out_error = NULL;
    return true;
}

static bool completion_reentry_action(
    void *user, cflow_statechart_action_phase phase,
    cflow_machine_state_id owner, const void *state,
    const cflow_event_view *event, void *out_state,
    cflow_statechart_raise_fn raise_internal, void *raise_user,
    const char **out_error) {
    completion_reentry_fixture *fixture =
        (completion_reentry_fixture *)user;
    (void)raise_internal;
    (void)raise_user;
    if (fixture == NULL || phase != CFLOW_STATECHART_ACTION_TRANSITION ||
        event != NULL || state == NULL || out_state == NULL ||
        out_error == NULL || fixture->trace_count >= 4u)
        return false;
    fixture->trace[fixture->trace_count++] = owner;
    *(int *)out_state = *(const int *)state + 1;
    *out_error = NULL;
    return true;
}

static cflow_statechart_instance_status completion_reentry_init(
    completion_reentry_fixture *fixture,
    cflow_statechart_state_kind root_kind, size_t completion_capacity,
    size_t enabled_count) {
    const cflow_statechart_guard_binding guard_binding = {
        REENTRY_GUARD, completion_reentry_guard, fixture};
    const cflow_statechart_executable_binding executable_binding = {
        REENTRY_EXEC, completion_reentry_action, fixture};
    cflow_statechart_instance_config config;
    memset(fixture, 0, sizeof(*fixture));
    fixture->enabled_count = enabled_count;
    fixture->states[0] = (cflow_statechart_state){
        REENTRY_ROOT, 0u, root_kind, 0u};
    if (root_kind == CFLOW_STATECHART_COMPOUND) {
        fixture->states[1] = (cflow_statechart_state){
            REENTRY_INITIAL, REENTRY_ROOT, CFLOW_STATECHART_INITIAL, 1u};
        fixture->states[2] = (cflow_statechart_state){
            REENTRY_FINAL_ONE, REENTRY_ROOT, CFLOW_STATECHART_FINAL, 2u};
        fixture->transitions[0] = (cflow_statechart_transition){
            1u, REENTRY_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
            0u, 0u, 0u, REENTRY_FINAL_ONE,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    } else {
        fixture->states[1] = (cflow_statechart_state){
            REENTRY_FINAL_ONE, REENTRY_ROOT, CFLOW_STATECHART_FINAL, 1u};
        fixture->states[2] = (cflow_statechart_state){
            REENTRY_FINAL_TWO, REENTRY_ROOT, CFLOW_STATECHART_FINAL, 2u};
    }
    fixture->guards[0] = (cflow_statechart_guard){
        REENTRY_GUARD, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
    fixture->executables[0] = (cflow_statechart_executable){
        REENTRY_EXEC, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
    fixture->transitions[1] = (cflow_statechart_transition){
        2u, REENTRY_ROOT, CFLOW_STATECHART_TRIGGER_COMPLETION,
        0u, REENTRY_ROOT, REENTRY_GUARD, REENTRY_ROOT,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->actions[0] = (cflow_statechart_transition_action){
        2u, REENTRY_EXEC, 0u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 3u, NULL, 0u,
        fixture->guards, 1u, fixture->executables, 1u,
        root_kind == CFLOW_STATECHART_COMPOUND
            ? fixture->transitions : fixture->transitions + 1u,
        root_kind == CFLOW_STATECHART_COMPOUND ? 2u : 1u,
        NULL, 0u, fixture->actions, 1u};
    check_equal(cflow_statechart_build(
                    &fixture->statechart, &fixture->definition),
                CFLOW_STATECHART_OK);
    check_true(cflow_executor_serial_init(&fixture->executor));
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .guards = &guard_binding,
        .guard_count = 1u,
        .executables = &executable_binding,
        .executable_count = 1u,
        .external_event_capacity = 1u,
        .internal_event_capacity = 1u,
        .completion_capacity = completion_capacity,
        .microstep_limit = 16u,
        .executor = &fixture->executor};
    return cflow_statechart_instance_init(&fixture->instance, &config);
}

static void completion_reentry_destroy(
    completion_reentry_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

static cflow_statechart_instance_status rtc_init(
    rtc_fixture *fixture, size_t internal_capacity,
    size_t completion_capacity, size_t microstep_limit,
    size_t executor_capacity) {
    return rtc_init_with_external(
        fixture, 4u, 0u, internal_capacity, completion_capacity,
        microstep_limit, executor_capacity);
}

static void rtc_destroy(rtc_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

static void rtc_add_other_transition_to_d(rtc_fixture *fixture) {
    fixture->transitions[7] = (cflow_statechart_transition){
        8u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENT,
        RTC_OTHER, 0u, 0u, RTC_D,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 7u};
    fixture->definition.transition_count = 8u;
}

static void rtc_send_other(rtc_fixture *fixture) {
    const int payload = 1;
    const cflow_event_view other = {
        RTC_OTHER, &cmeta_type_int, &payload};
    check_equal(cflow_statechart_instance_try_send(
                    &fixture->instance, &other),
                CFLOW_MAILBOX_OK);
    check_true(cflow_executor_wait_idle(&fixture->executor));
}

static void check_explicit_control_survives_driver_cancel(bool cancel) {
    rtc_fixture fixture;
    microstep_executor_blocker blocker;
    cflow_executor_control control = {0};
    const int payload = 1;
    const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
    cflow_statechart_instance_stats stats = {0};
    rtc_definition(&fixture, true, false);
    check_equal(rtc_init(&fixture, 4u, 4u, 16u, 2u),
                CFLOW_STATECHART_INSTANCE_OK);
    atomic_init(&blocker.entered, false);
    atomic_init(&blocker.release, false);
    check_equal(cflow_executor_try_post(
                    &fixture.executor, microstep_block_executor, &blocker),
                CFLOW_ADMISSION_ACCEPTED);
    while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
    check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                CFLOW_MAILBOX_OK);
    if (cancel)
        cflow_statechart_instance_cancel(&fixture.instance);
    else
        cflow_statechart_instance_close(&fixture.instance);
    check_true(cflow_executor_as_control(&fixture.executor, &control));
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    atomic_store(&blocker.release, true);
    check_true(cflow_executor_wait_idle(&fixture.executor));
    check_true(cflow_statechart_instance_get_stats(&fixture.instance, &stats));
    check_equal(stats.last_status, CFLOW_STATECHART_INSTANCE_OK);
    check_false(stats.errored);
    check_equal(stats.external_accepted, UINT64_C(1));
    check_equal(stats.external_cancelled, UINT64_C(1));
    check_equal(stats.external_failed, UINT64_C(0));
    check_equal(stats.cancelled, cancel);
    rtc_destroy(&fixture);
}

static void check_control_wins_before_external_receive(bool cancel) {
    rtc_fixture fixture;
    microstep_executor_blocker receive_blocker;
    const int payload = 1;
    const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
    cflow_statechart_instance_stats stats = {0};
    const cflow_statechart_instance_test_hooks hooks = {
        .before_external_receive = microstep_block_executor,
        .user = &receive_blocker};
    rtc_definition(&fixture, true, false);
    check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                CFLOW_STATECHART_INSTANCE_OK);
    atomic_init(&receive_blocker.entered, false);
    atomic_init(&receive_blocker.release, false);
    check_true(cflow_statechart_instance_set_test_hooks_internal(
        &fixture.instance, &hooks));
    check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                CFLOW_MAILBOX_OK);
    while (!atomic_load(&receive_blocker.entered)) cmeta_thread_yield();
    if (cancel)
        cflow_statechart_instance_cancel(&fixture.instance);
    else
        cflow_statechart_instance_close(&fixture.instance);
    check_true(cflow_statechart_instance_get_stats(&fixture.instance, &stats));
    check_equal(stats.external_accepted, UINT64_C(1));
    check_equal(stats.external_pending, (size_t)0u);
    check_equal(stats.external_in_flight, (size_t)0u);
    check_equal(stats.external_cancelled, UINT64_C(1));
    check_equal(stats.external_completed, UINT64_C(0));
    check_equal(stats.external_failed, UINT64_C(0));
    check_equal(stats.cancelled, cancel);
    if (cancel)
        cflow_statechart_instance_close(&fixture.instance);
    else
        cflow_statechart_instance_cancel(&fixture.instance);
    atomic_store(&receive_blocker.release, true);
    check_true(cflow_executor_wait_idle(&fixture.executor));
    check_true(cflow_statechart_instance_get_stats(&fixture.instance, &stats));
    check_equal(stats.external_accepted, UINT64_C(1));
    check_equal(stats.external_pending, (size_t)0u);
    check_equal(stats.external_in_flight, (size_t)0u);
    check_equal(stats.external_cancelled, UINT64_C(1));
    check_equal(stats.cancelled, cancel);
    rtc_destroy(&fixture);
}

suite("CFlow Statechart public run-to-completion runtime") {
    it("saturates the public accounting identity without intermediate wrap") {
        check_equal(cflow_statechart_external_identity_sum_internal(
                        UINT64_MAX - UINT64_C(2), UINT64_C(1),
                        UINT64_C(1), UINT64_C(0), UINT64_C(0)),
                    UINT64_MAX);
        check_equal(cflow_statechart_external_identity_sum_internal(
                        UINT64_MAX - UINT64_C(1), UINT64_C(0),
                        UINT64_C(0), UINT64_C(1), UINT64_C(1)),
                    UINT64_MAX);
        check_equal(cflow_statechart_external_identity_sum_internal(
                        UINT64_C(3), UINT64_C(5), UINT64_C(7),
                        UINT64_C(11), UINT64_C(13)),
                    UINT64_C(39));
    }

    it("runs active exit actions transactionally before controlled cancellation") {
        rtc_fixture fixture;
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        const int expected_trace[] = {RTC_A, RTC_ROOT};
        const cmeta_type_desc *state_type = NULL;
        size_t state_count = 0u;
        uint64_t version = 0u;
        int state = 0;
        rtc_definition(&fixture, true, false);
        fixture.state_actions[0] = (cflow_statechart_state_action){
            RTC_A, CFLOW_STATECHART_STATE_ACTION_EXIT, RTC_EXEC, 0u};
        fixture.state_actions[1] = (cflow_statechart_state_action){
            RTC_ROOT, CFLOW_STATECHART_STATE_ACTION_EXIT, RTC_EXEC, 0u};
        fixture.definition.state_actions = fixture.state_actions;
        fixture.definition.state_action_count = 2u;
        fixture.use_contextual_action = true;
        fixture.stage_exit_effect = true;
        fixture.exit_effect_observed_terminal = true;
        fixture.effect_capacity = 2u;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        cflow_statechart_instance_request_exit(&fixture.instance);
        cflow_statechart_instance_request_exit(&fixture.instance);
        cflow_statechart_instance_close(&fixture.instance);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u,
                        &state_count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(state_count, (size_t)0u);
        check_equal(version, UINT64_C(2));
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_equal(fixture.exit_effect_commits, (size_t)2u);
        check_equal(fixture.exit_effect_discards, (size_t)0u);
        check_true(fixture.exit_effect_observed_terminal);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &state_type, &state, sizeof(state)));
        check_equal(state, 2);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.closed);
        check_true(stats.cancelled);
        check_true(stats.done);
        check_false(stats.errored);
        {
            const int payload = 1;
            const cflow_event_view go = {
                RTC_GO, &cmeta_type_int, &payload};
            check_equal(cflow_statechart_instance_try_send(
                            &fixture.instance, &go),
                        CFLOW_MAILBOX_CANCELLED);
        }
        rtc_destroy(&fixture);
    }

    it("preserves a committing microstep before controlled exit") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const int expected_trace[] = {RTC_A, RTC_B, RTC_ROOT};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        const cmeta_type_desc *state_type = NULL;
        size_t state_count = 0u;
        uint64_t version = 0u;
        int state = 0;
        rtc_definition(&fixture, true, false);
        fixture.state_actions[0] = (cflow_statechart_state_action){
            RTC_B, CFLOW_STATECHART_STATE_ACTION_EXIT, RTC_EXEC, 0u};
        fixture.state_actions[1] = (cflow_statechart_state_action){
            RTC_ROOT, CFLOW_STATECHART_STATE_ACTION_EXIT, RTC_EXEC, 0u};
        fixture.definition.state_actions = fixture.state_actions;
        fixture.definition.state_action_count = 2u;
        fixture.block_transition_action = true;
        atomic_init(&fixture.transition_action_entered, false);
        atomic_init(&fixture.transition_action_release, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&fixture.transition_action_entered))
            cmeta_thread_yield();
        cflow_statechart_instance_request_exit(&fixture.instance);
        atomic_store(&fixture.transition_action_release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u,
                        &state_count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(state_count, (size_t)0u);
        check_equal(version, UINT64_C(3));
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &state_type, &state, sizeof(state)));
        check_equal(state, 3);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.internal_pending, (size_t)0u);
        check_true(stats.cancelled);
        check_true(stats.done);
        rtc_destroy(&fixture);
    }

    it("rolls back controlled exit when an exit action fails") {
        rtc_fixture fixture;
        const int expected_trace[] = {RTC_A, RTC_ROOT};
        const cflow_machine_state_id expected_states[] = {
            RTC_ROOT, RTC_A};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        const cmeta_type_desc *state_type = NULL;
        size_t state_count = 0u;
        uint64_t version = 0u;
        int state = -1;
        rtc_definition(&fixture, true, false);
        fixture.state_actions[0] = (cflow_statechart_state_action){
            RTC_A, CFLOW_STATECHART_STATE_ACTION_EXIT, RTC_EXEC, 0u};
        fixture.state_actions[1] = (cflow_statechart_state_action){
            RTC_ROOT, CFLOW_STATECHART_STATE_ACTION_EXIT, RTC_EXEC, 0u};
        fixture.definition.state_actions = fixture.state_actions;
        fixture.definition.state_action_count = 2u;
        fixture.use_contextual_action = true;
        fixture.stage_exit_effect = true;
        fixture.fail_exit_action = true;
        fixture.effect_capacity = 2u;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        cflow_statechart_instance_request_exit(&fixture.instance);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u,
                        &state_count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected_states, sizeof(expected_states));
        check_equal(version, UINT64_C(1));
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_equal(fixture.exit_effect_commits, (size_t)0u);
        check_equal(fixture.exit_effect_discards, (size_t)2u);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &state_type, &state, sizeof(state)));
        check_equal(state, 0);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "deliberate controlled exit failure");
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.closed);
        check_true(stats.done);
        check_true(stats.errored);
        check_false(stats.cancelled);
        rtc_destroy(&fixture);
    }

    it("reissues compound completion once for each immediate reentry") {
        completion_reentry_fixture fixture;
        const cflow_machine_state_id expected[] = {
            REENTRY_ROOT, REENTRY_ROOT, REENTRY_ROOT};
        check_equal(completion_reentry_init(
                        &fixture, CFLOW_STATECHART_COMPOUND, 2u, 3u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(fixture.trace_count, (size_t)3u);
        check_equal(fixture.trace, expected, sizeof(expected));
        check_equal(fixture.guard_calls, (size_t)4u);
        completion_reentry_destroy(&fixture);
    }

    it("reissues parallel completion once for each immediate reentry") {
        completion_reentry_fixture fixture;
        const cflow_machine_state_id expected[] = {
            REENTRY_ROOT, REENTRY_ROOT, REENTRY_ROOT};
        check_equal(completion_reentry_init(
                        &fixture, CFLOW_STATECHART_PARALLEL, 1u, 6u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(fixture.trace_count, (size_t)3u);
        check_equal(fixture.trace, expected, sizeof(expected));
        check_equal(fixture.guard_calls, (size_t)8u);
        completion_reentry_destroy(&fixture);
    }

    it("wraps internal FIFO capacities one and two across macrosteps") {
        const size_t capacities[] = {1u, 2u};
        size_t capacity_index;
        for (capacity_index = 0u; capacity_index < 2u; ++capacity_index) {
            rtc_fixture fixture;
            const int payload = 1;
            const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
            const cflow_event_view other = {
                RTC_OTHER, &cmeta_type_int, &payload};
            cflow_statechart_instance_stats stats = {0};
            size_t index;
            rtc_definition(&fixture, true, false);
            fixture.raise_on_other = true;
            fixture.actions[3].transition = 6u;
            check_equal(rtc_init(
                            &fixture, capacities[capacity_index], 2u,
                            16u, 4u),
                        CFLOW_STATECHART_INSTANCE_OK);
            check_equal(cflow_statechart_instance_try_send(
                            &fixture.instance, &go),
                        CFLOW_MAILBOX_OK);
            check_true(cflow_executor_wait_idle(&fixture.executor));
            for (index = 0u; index < 4u; ++index) {
                check_equal(cflow_statechart_instance_try_send(
                                &fixture.instance, &other),
                            CFLOW_MAILBOX_OK);
                check_true(cflow_executor_wait_idle(&fixture.executor));
            }
            check_true(cflow_statechart_instance_get_stats(
                &fixture.instance, &stats));
            check_equal(stats.external_completed, UINT64_C(5));
            check_equal(stats.internal_pending, (size_t)0u);
            check_false(stats.errored);
            rtc_destroy(&fixture);
        }
    }
    it("rejects every zero runtime bound before publishing an instance") {
        runtime_fixture fixture;
        cflow_statechart_instance_config config;
        nested_compound_fixture(&fixture);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 1u,
            .internal_event_capacity = 1u,
            .completion_capacity = 1u,
            .microstep_limit = 8u,
            .executor = &fixture.executor};

        config.external_event_capacity = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        config.external_event_capacity = 1u;
        config.internal_event_capacity = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        config.internal_event_capacity = 1u;
        config.completion_capacity = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        config.completion_capacity = 1u;
        config.microstep_limit = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("copies external events and reports exact bounded admission outcomes") {
        runtime_fixture fixture;
        cflow_statechart_instance_config config;
        const int payload = 7;
        const long wrong_payload = 7;
        cflow_event_view event;
        nested_compound_fixture(&fixture);
        fixture.events[0] = (cflow_event_type){100u, &cmeta_type_int};
        fixture.definition.events = fixture.events;
        fixture.definition.event_count = 1u;
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 1u,
            .internal_event_capacity = 1u,
            .completion_capacity = 1u,
            .microstep_limit = 8u,
            .executor = &fixture.executor};
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));

        event = (cflow_event_view){100u, &cmeta_type_long, &wrong_payload};
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_TYPE_MISMATCH);
        event = (cflow_event_view){999u, &cmeta_type_int, &payload};
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        event = (cflow_event_view){100u, &cmeta_type_int, &payload};
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_null(cflow_statechart_instance_error(&fixture.instance));
        cflow_statechart_instance_close(&fixture.instance);
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_CLOSED);
        runtime_fixture_destroy(&fixture);
    }

    it("terminates a root FINAL without synthesizing a completion row") {
        const cflow_statechart_state states[] = {{
            RTC_ROOT, 0u, CFLOW_STATECHART_FINAL, 0u}};
        const cflow_statechart_definition definition = {
            &cmeta_type_int, states, 1u, NULL, 0u, NULL, 0u,
            NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u};
        const int initial_state = 0;
        cflow_statechart statechart = {0};
        cflow_executor executor = {0};
        cflow_statechart_instance instance = {0};
        cflow_statechart_instance_stats stats = {0};
        cflow_statechart_instance_config config;
        check_equal(cflow_statechart_build(&statechart, &definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&executor));
        config = (cflow_statechart_instance_config){
            .statechart = &statechart,
            .initial_state = &initial_state,
            .external_event_capacity = 1u,
            .internal_event_capacity = 1u,
            .completion_capacity = 1u,
            .microstep_limit = 1u,
            .executor = &executor};
        check_equal(cflow_statechart_instance_init(&instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_statechart_instance_get_stats(&instance, &stats));
        check_true(stats.done);
        check_false(stats.errored);
        check_equal(stats.completion_pending, (size_t)0u);
        check_equal(cflow_statechart_instance_destroy(&instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        cflow_executor_destroy(&executor);
        cflow_statechart_destroy(&statechart);
    }

    it("stabilizes an eventless transition before init returns") {
        rtc_fixture fixture;
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_D};
        cflow_machine_state_id states[2] = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        fixture.transitions[7] = (cflow_statechart_transition){
            8u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENTLESS,
            0u, 0u, 0u, RTC_D,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 7u};
        fixture.definition.transition_count = 8u;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_equal(version, UINT64_C(2));
        rtc_destroy(&fixture);
    }

    it("calls the stable hook only after the published macrostep is stable") {
        rtc_fixture fixture;
        rtc_definition(&fixture, true, false);
        fixture.transitions[7] = (cflow_statechart_transition){
            8u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENTLESS,
            0u, 0u, 0u, RTC_D,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 7u};
        fixture.definition.transition_count = 8u;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(fixture.stable_hook_calls, (size_t)1u);
        check_false(fixture.observed_a_active);
        check_true(fixture.observed_d_active);
        check_equal(fixture.observed_configuration_version, UINT64_C(2));
        rtc_destroy(&fixture);
    }

    it("drains a stable-hook internal Event before exposing quiescence") {
        rtc_fixture fixture;
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_D};
        cflow_machine_state_id states[2] = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        fixture.transitions[7] = (cflow_statechart_transition){
            8u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENT,
            RTC_OTHER, 0u, 0u, RTC_D,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 7u};
        fixture.definition.transition_count = 8u;
        fixture.enqueue_other_once = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init_with_external(
                        &fixture, 4u, 1u, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_equal(version, UINT64_C(2));
        check_equal(fixture.stable_hook_calls, (size_t)2u);
        rtc_destroy(&fixture);
    }

    it("preprocesses FIFO-aligned tagged external Events before selection") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, false);
        fixture.transitions[7] = (cflow_statechart_transition){
            8u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENT,
            RTC_OTHER, 0u, 0u, RTC_D,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 7u};
        fixture.definition.transition_count = 8u;
        fixture.drop_tagged_external = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(77)),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &other),
                    CFLOW_MAILBOX_OK);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.preprocess_hook_calls, (size_t)2u);
        check_equal(fixture.observed_origin_tokens[0], UINT64_C(77));
        check_equal(fixture.observed_origin_tokens[1], UINT64_C(0));
        check_equal(fixture.observed_origin_token, UINT64_C(0));
        check_equal(cflow_statechart_instance_current_state(
                        &fixture.instance), RTC_D);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_completed, UINT64_C(2));
        rtc_destroy(&fixture);
    }

    it("reports one tagged settlement after macrostep quiescence") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(77)),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.settlement_count, (size_t)1u);
        check_true(fixture.settlement_stats_read_succeeded);
        check_equal(fixture.settlements[0].origin_token, UINT64_C(77));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_COMPLETED);
        check_equal(fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(fixture.settlements[0].configuration_version,
                    UINT64_C(1));
        check_null(fixture.settlements[0].error);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(0)),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.settlement_count, (size_t)1u);
        rtc_destroy(&fixture);
    }

    it("distinguishes dropped and failed tagged settlements") {
        rtc_fixture dropped_fixture;
        rtc_fixture failed_fixture;
        const int payload = 1;
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};

        rtc_definition(&dropped_fixture, true, false);
        dropped_fixture.drop_tagged_external = true;
        dropped_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction,
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&dropped_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &dropped_fixture.instance, &other, UINT64_C(81)),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&dropped_fixture.executor));
        check_equal(dropped_fixture.settlement_count, (size_t)1u);
        check_equal(dropped_fixture.settlements[0].origin_token,
                    UINT64_C(81));
        check_equal(dropped_fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_DROPPED);
        check_equal(dropped_fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_OK);
        check_null(dropped_fixture.settlements[0].error);
        rtc_destroy(&dropped_fixture);

        rtc_definition(&failed_fixture, true, false);
        failed_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction,
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&failed_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        failed_fixture.fail_stable_hook = true;
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &failed_fixture.instance, &other, UINT64_C(82)),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&failed_fixture.executor));
        check_equal(failed_fixture.settlement_count, (size_t)1u);
        check_equal(failed_fixture.settlements[0].origin_token,
                    UINT64_C(82));
        check_equal(failed_fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_FAILED);
        check_equal(failed_fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_HOOK_FAILED);
        check_equal(failed_fixture.settlements[0].error,
                    "deliberate host quiescence failure");
        rtc_destroy(&failed_fixture);
    }

    it("preserves tagged settlement FIFO while the token ring wraps") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        size_t index;
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init_with_external(
                        &fixture, 2u, 0u, 4u, 16u, 4u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        for (index = 0u; index < 5u; ++index) {
            check_equal(cflow_statechart_instance_try_send_tagged(
                            &fixture.instance, &other,
                            UINT64_C(601) + (uint64_t)index),
                        CFLOW_MAILBOX_OK);
            check_true(cflow_executor_wait_idle(&fixture.executor));
        }
        check_equal(fixture.settlement_count, (size_t)5u);
        for (index = 0u; index < fixture.settlement_count; ++index) {
            check_equal(fixture.settlements[index].origin_token,
                        UINT64_C(601) + (uint64_t)index);
            check_equal(fixture.settlements[index].kind,
                        CFLOW_STATECHART_EXTERNAL_SETTLED_COMPLETED);
        }
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(runtime_wait_flag(&blocker.entered));
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(606)),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(607)),
                    CFLOW_MAILBOX_OK);
        cflow_statechart_instance_cancel(&fixture.instance);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.settlement_count, (size_t)7u);
        check_equal(fixture.settlements[5].origin_token, UINT64_C(606));
        check_equal(fixture.settlements[6].origin_token, UINT64_C(607));
        check_equal(fixture.settlements[5].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        check_equal(fixture.settlements[6].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        rtc_destroy(&fixture);
    }

    it("serializes reentrant cancellation from a settlement callback") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        rtc_definition(&fixture, true, false);
        fixture.cancel_on_first_settlement = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction,
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init_with_external(
                        &fixture, 2u, 0u, 4u, 16u, 4u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(runtime_wait_flag(&blocker.entered));
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(701)),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(702)),
                    CFLOW_MAILBOX_OK);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.settlement_count, (size_t)2u);
        check_false(atomic_load(&fixture.settlement_callback_overlap));
        check_equal(fixture.settlements[0].origin_token, UINT64_C(701));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_COMPLETED);
        check_equal(fixture.settlements[1].origin_token, UINT64_C(702));
        check_equal(fixture.settlements[1].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        rtc_destroy(&fixture);
    }

    it("observes a transactional tagged internal Event with its source token") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        size_t index;
        bool found = false;
        rtc_definition(&fixture, false, false);
        fixture.use_contextual_action = true;
        fixture.raise_tagged_internal = true;
        fixture.tagged_internal_token = UINT64_C(91);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        for (index = 0u; index < fixture.event_hook_calls && index < 8u;
             ++index) {
            if (fixture.observed_event_kinds[index] ==
                    CFLOW_STATECHART_OBSERVED_INTERNAL &&
                fixture.observed_event_tokens[index] == UINT64_C(91)) {
                found = true;
                break;
            }
        }
        check_true(found);
        rtc_destroy(&fixture);
    }

    it("avoids a state copy for a no-op host quiescence") {
        rtc_fixture fixture;
        const cmeta_type_desc *type = NULL;
        cflow_statechart_instance_stats stats = {0};
        int state = -1;
        rtc_definition(&fixture, true, false);
        fixture.host_transaction_activate_call = 2u;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(fixture.host_transaction_calls, (size_t)1u);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &state, sizeof(state)));
        check_equal(state, 0);
        check_true(cmeta_type_equal(type, &cmeta_type_int));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.configuration_version, UINT64_C(1));
        check_equal(stats.internal_pending, (size_t)0u);
        rtc_destroy(&fixture);
    }

    it("commits host state Events and effects as one ordered transaction") {
        rtc_fixture fixture;
        const cmeta_type_desc *type = NULL;
        cflow_statechart_instance_stats stats = {0};
        int state = -1;
        rtc_definition(&fixture, true, false);
        rtc_add_other_transition_to_d(&fixture);
        fixture.effect_capacity = 2u;
        fixture.host_transaction_activate_call = 2u;
        fixture.host_transaction_result = CFLOW_STATECHART_HOST_CONTINUE;
        fixture.host_transaction_write_state = true;
        fixture.host_transaction_state = 41;
        fixture.host_transaction_raise_event = true;
        fixture.host_transaction_stage_effects = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);

        rtc_send_other(&fixture);
        check_true(fixture.host_transaction_raise_succeeded);
        check_true(fixture.host_transaction_effect_succeeded);
        check_equal(fixture.host_transaction_effect_trace_count,
                    (size_t)2u);
        check_equal(fixture.host_transaction_effect_trace[0], 1);
        check_equal(fixture.host_transaction_effect_trace[1], 2);
        check_true(fixture.host_transaction_effect_observed_published);
        check_equal(cflow_statechart_instance_current_state(
                        &fixture.instance), RTC_E);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &state, sizeof(state)));
        check_equal(state, 42);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.configuration_version, UINT64_C(4));
        check_equal(stats.internal_pending, (size_t)0u);
        rtc_destroy(&fixture);
    }

    it("rolls back FATAL host work and discards effects in order") {
        rtc_fixture fixture;
        const cmeta_type_desc *type = NULL;
        cflow_statechart_instance_stats stats = {0};
        int state = -1;
        rtc_definition(&fixture, true, false);
        rtc_add_other_transition_to_d(&fixture);
        fixture.effect_capacity = 2u;
        fixture.host_transaction_activate_call = 2u;
        fixture.host_transaction_result = CFLOW_STATECHART_HOST_FATAL;
        fixture.host_transaction_write_state = true;
        fixture.host_transaction_state = 99;
        fixture.host_transaction_raise_event = true;
        fixture.host_transaction_stage_effects = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);

        rtc_send_other(&fixture);
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "deliberate host transaction failure");
        check_equal(fixture.host_transaction_effect_trace_count,
                    (size_t)2u);
        check_equal(fixture.host_transaction_effect_trace[0], -1);
        check_equal(fixture.host_transaction_effect_trace[1], -2);
        check_equal(cflow_statechart_instance_current_state(
                        &fixture.instance), RTC_D);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &state, sizeof(state)));
        check_equal(state, 0);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.configuration_version, UINT64_C(2));
        check_equal(stats.internal_pending, (size_t)0u);
        rtc_destroy(&fixture);
    }

    it("rejects an invalid host-transaction effect ticket") {
        rtc_fixture fixture;
        rtc_definition(&fixture, true, false);
        fixture.effect_capacity = 1u;
        fixture.host_transaction_activate_call = 1u;
        fixture.host_transaction_result = CFLOW_STATECHART_HOST_CONTINUE;
        fixture.host_transaction_stage_invalid_effect = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_ACTION_FAILED);
        check_false(fixture.host_transaction_effect_succeeded);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("discards accepted host effects when the journal fills") {
        rtc_fixture fixture;
        rtc_definition(&fixture, true, false);
        fixture.effect_capacity = 1u;
        fixture.host_transaction_activate_call = 1u;
        fixture.host_transaction_result = CFLOW_STATECHART_HOST_CONTINUE;
        fixture.host_transaction_stage_effects = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_EFFECT_JOURNAL_FULL);
        check_false(fixture.host_transaction_effect_succeeded);
        check_equal(fixture.host_transaction_effect_trace_count,
                    (size_t)1u);
        check_equal(fixture.host_transaction_effect_trace[0], -1);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("lets cancellation beat a prepared host transaction") {
        rtc_fixture fixture;
        const cmeta_type_desc *type = NULL;
        cflow_statechart_instance_stats stats = {0};
        int state = -1;
        rtc_definition(&fixture, true, false);
        rtc_add_other_transition_to_d(&fixture);
        fixture.effect_capacity = 2u;
        fixture.host_transaction_activate_call = 2u;
        fixture.host_transaction_result = CFLOW_STATECHART_HOST_CONTINUE;
        fixture.host_transaction_write_state = true;
        fixture.host_transaction_state = 77;
        fixture.host_transaction_raise_event = true;
        fixture.host_transaction_stage_effects = true;
        fixture.host_transaction_cancel = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);

        rtc_send_other(&fixture);
        check_null(cflow_statechart_instance_error(&fixture.instance));
        check_equal(fixture.host_transaction_effect_trace_count,
                    (size_t)2u);
        check_equal(fixture.host_transaction_effect_trace[0], -1);
        check_equal(fixture.host_transaction_effect_trace[1], -2);
        check_equal(cflow_statechart_instance_current_state(
                        &fixture.instance), RTC_D);
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &type, &state, sizeof(state)));
        check_equal(state, 0);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.cancelled);
        check_equal(stats.configuration_version, UINT64_C(2));
        check_equal(stats.internal_pending, (size_t)0u);
        rtc_destroy(&fixture);
    }

    it("rejects incompatible runtime hook ABI shapes") {
        rtc_fixture version_fixture;
        rtc_fixture size_fixture;
        rtc_fixture missing_callback_fixture;
        rtc_fixture v5_size_fixture;
        rtc_fixture v5_missing_callbacks_fixture;
        rtc_definition(&version_fixture, true, false);
        version_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5 + 1u,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&version_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(version_fixture.instance.impl);
        cflow_executor_destroy(&version_fixture.executor);
        cflow_statechart_destroy(&version_fixture.statechart);

        rtc_definition(&size_fixture, true, false);
        size_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = offsetof(
                cflow_statechart_instance_hooks,
                on_external_settlement) - 1u,
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&size_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(size_fixture.instance.impl);
        cflow_executor_destroy(&size_fixture.executor);
        cflow_statechart_destroy(&size_fixture.statechart);

        rtc_definition(&missing_callback_fixture, true, false);
        missing_callback_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks)};
        check_equal(rtc_init(&missing_callback_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(missing_callback_fixture.instance.impl);
        cflow_executor_destroy(&missing_callback_fixture.executor);
        cflow_statechart_destroy(&missing_callback_fixture.statechart);

        rtc_definition(&v5_size_fixture, true, false);
        v5_size_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks) - 1u,
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&v5_size_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(v5_size_fixture.instance.impl);
        cflow_executor_destroy(&v5_size_fixture.executor);
        cflow_statechart_destroy(&v5_size_fixture.statechart);

        rtc_definition(&v5_missing_callbacks_fixture, true, false);
        v5_missing_callbacks_fixture.hooks =
            (cflow_statechart_instance_hooks){
                .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
                .struct_size = sizeof(cflow_statechart_instance_hooks)};
        check_equal(rtc_init(
                        &v5_missing_callbacks_fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(v5_missing_callbacks_fixture.instance.impl);
        cflow_executor_destroy(&v5_missing_callbacks_fixture.executor);
        cflow_statechart_destroy(&v5_missing_callbacks_fixture.statechart);
    }

    it("accepts the exact legacy V4 hook prefix") {
        rtc_fixture fixture;
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = offsetof(
                cflow_statechart_instance_hooks,
                on_external_settlement),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(fixture.stable_hook_calls, (size_t)1u);
        rtc_destroy(&fixture);
    }

    it("rejects every pre-transaction runtime hook ABI") {
        rtc_fixture fixture;
        cflow_statechart_instance_status status;
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4 - 1u,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = managed_host_transaction};
        status = rtc_init(&fixture, 4u, 4u, 16u, 4u);
        if (status == CFLOW_STATECHART_INSTANCE_OK) {
            rtc_destroy(&fixture);
        } else {
            cflow_executor_destroy(&fixture.executor);
            cflow_statechart_destroy(&fixture.statechart);
        }
        check_equal(status, CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
    }

    it("fails initialization explicitly when host quiescence fails") {
        rtc_fixture fixture;
        rtc_definition(&fixture, true, false);
        fixture.fail_stable_hook = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_HOOK_FAILED);
        check_null(fixture.instance.impl);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("returns external FULL while one admitted event waits to run") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init_with_external(
                        &fixture, 1u, 0u, 4u, 4u, 16u, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_FULL);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        rtc_destroy(&fixture);
    }

    it("processes adapter-internal ingress before an admitted external Event") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view external = {
            RTC_OTHER, &cmeta_type_int, &payload};
        const cflow_event_view adapter_internal = {
            RTC_GO, &cmeta_type_int, &payload};
        const int expected_trace[] = {RTC_A, RTC_B, RTC_C};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, false, false);
        fixture.transitions[6] = (cflow_statechart_transition){
            8u, RTC_A, CFLOW_STATECHART_TRIGGER_EVENT,
            RTC_OTHER, 0u, 0u, RTC_D,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 6u};
        fixture.actions[4] = (cflow_statechart_transition_action){
            8u, RTC_EXEC, 0u};
        fixture.definition.transition_count = 7u;
        fixture.definition.transition_action_count = 5u;
        check_equal(rtc_init_with_external(
                        &fixture, 4u, 1u, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &external),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send_internal(
                        &fixture.instance, &adapter_internal),
                    CFLOW_MAILBOX_OK);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.adapter_internal_accepted, UINT64_C(1));
        check_equal(stats.adapter_internal_pending, (size_t)0u);
        check_true(stats.done);
        rtc_destroy(&fixture);
    }

    it("validates and bounds adapter-internal ingress") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const bool bool_payload = false;
        const cflow_event_view valid = {
            RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view unknown = {
            999u, &cmeta_type_int, &payload};
        const cflow_event_view mismatch = {
            RTC_GO, &cmeta_type_bool, &bool_payload};
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init_with_external(
                        &fixture, 4u, 1u, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send_internal(
                        &fixture.instance, &unknown),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_equal(cflow_statechart_instance_try_send_internal(
                        &fixture.instance, &mismatch),
                    CFLOW_MAILBOX_TYPE_MISMATCH);
        check_equal(cflow_statechart_instance_try_send_internal(
                        &fixture.instance, &valid),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send_internal(
                        &fixture.instance, &valid),
                    CFLOW_MAILBOX_FULL);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        rtc_destroy(&fixture);
    }

    it("rejects adapter-internal ingress after close") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view event = {
            RTC_GO, &cmeta_type_int, &payload};
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init_with_external(
                        &fixture, 4u, 1u, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        cflow_statechart_instance_close(&fixture.instance);
        check_equal(cflow_statechart_instance_try_send_internal(
                        &fixture.instance, &event),
                    CFLOW_MAILBOX_CLOSED);
        rtc_destroy(&fixture);
    }

    it("finishes eventless internal and root completion before next external") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        const int expected_trace[] = {
            RTC_A, RTC_B, RTC_C, RTC_ROOT, RTC_D};
        const cflow_machine_state_id expected_states[] = {RTC_ROOT, RTC_E};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        int state = 0;
        const cmeta_type_desc *state_type = NULL;
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &other),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_equal(fixture.trace_count, (size_t)5u);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected_states, sizeof(expected_states));
        check_true(cflow_statechart_instance_copy_state(
            &fixture.instance, &state_type, &state, sizeof(state)));
        check_equal(state, 5);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.external_completed, UINT64_C(2));
        check_equal(stats.external_failed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(0));
        check_equal(stats.external_pending, (size_t)0u);
        check_equal(stats.external_in_flight, (size_t)0u);
        check_equal(stats.microsteps, UINT64_C(5));
        check_equal(stats.macrosteps, UINT64_C(3));
        check_false(stats.done);
        rtc_destroy(&fixture);
    }

    it("processes an unhandled root completion then terminates") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const int expected_trace[] = {RTC_A, RTC_B, RTC_C};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, false, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.done);
        check_false(stats.errored);
        check_equal(stats.external_completed, UINT64_C(1));
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_CLOSED);
        rtc_destroy(&fixture);
    }

    it("drains nested completion transitions before root termination") {
        enum {
            ROOT = 1000u,
            ROOT_INITIAL = 1001u,
            FIRST = 1010u,
            FIRST_INITIAL = 1011u,
            FIRST_FINAL = 1012u,
            SECOND = 1020u,
            SECOND_INITIAL = 1021u,
            SECOND_FINAL = RTC_FINAL,
            PASS = 1030u,
            FAIL = 1040u
        };
        rtc_fixture action_fixture = {0};
        const cflow_statechart_state states[] = {
            {ROOT, 0u, CFLOW_STATECHART_COMPOUND, 0u},
            {ROOT_INITIAL, ROOT, CFLOW_STATECHART_INITIAL, 1u},
            {FIRST, ROOT, CFLOW_STATECHART_COMPOUND, 2u},
            {FIRST_INITIAL, FIRST, CFLOW_STATECHART_INITIAL, 3u},
            {FIRST_FINAL, FIRST, CFLOW_STATECHART_FINAL, 4u},
            {SECOND, ROOT, CFLOW_STATECHART_COMPOUND, 5u},
            {SECOND_INITIAL, SECOND, CFLOW_STATECHART_INITIAL, 6u},
            {SECOND_FINAL, SECOND, CFLOW_STATECHART_FINAL, 7u},
            {PASS, ROOT, CFLOW_STATECHART_FINAL, 8u},
            {FAIL, ROOT, CFLOW_STATECHART_ATOMIC, 9u}};
        const cflow_event_type events[] = {
            {RTC_NEXT, &cmeta_type_int}};
        const cflow_statechart_executable executables[] = {{
            RTC_EXEC, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS}};
        const cflow_statechart_transition transitions[] = {
            {1u, ROOT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
             0u, 0u, 0u, FIRST,
             CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u},
            {2u, FIRST_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
             0u, 0u, 0u, FIRST_FINAL,
             CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u},
            {3u, FIRST, CFLOW_STATECHART_TRIGGER_COMPLETION,
             0u, FIRST, 0u, SECOND,
             CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u},
            {4u, SECOND_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
             0u, 0u, 0u, SECOND_FINAL,
             CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 3u},
            {5u, SECOND, CFLOW_STATECHART_TRIGGER_COMPLETION,
             0u, SECOND, 0u, PASS,
             CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 4u},
            {6u, ROOT, CFLOW_STATECHART_TRIGGER_EVENT,
             RTC_NEXT, 0u, 0u, FAIL,
             CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 5u}};
        const cflow_statechart_state_action state_actions[] = {{
            SECOND_FINAL, CFLOW_STATECHART_STATE_ACTION_ENTRY,
            RTC_EXEC, 0u}};
        const cflow_statechart_definition definition = {
            &cmeta_type_int, states, sizeof(states) / sizeof(states[0]),
            events, sizeof(events) / sizeof(events[0]),
            NULL, 0u,
            executables, sizeof(executables) / sizeof(executables[0]),
            transitions, sizeof(transitions) / sizeof(transitions[0]),
            state_actions,
            sizeof(state_actions) / sizeof(state_actions[0]),
            NULL, 0u};
        const cflow_statechart_executable_binding binding = {
            .id = RTC_EXEC,
            .fn = rtc_action,
            .user = &action_fixture};
        const cflow_machine_state_id expected[] = {ROOT, PASS};
        const int initial_state = 0;
        cflow_statechart statechart = {0};
        cflow_executor executor = {0};
        cflow_statechart_instance instance = {0};
        cflow_statechart_instance_stats stats = {0};
        cflow_statechart_instance_config config;
        cflow_machine_state_id active[3] = {0};
        size_t active_count = 0u;
        uint64_t version = 0u;

        action_fixture.raise_on_final_entry = true;
        action_fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(cflow_statechart_build(&statechart, &definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&executor));
        config = (cflow_statechart_instance_config){
            .statechart = &statechart,
            .initial_state = &initial_state,
            .executables = &binding,
            .executable_count = 1u,
            .external_event_capacity = 1u,
            .internal_event_capacity = 1u,
            .completion_capacity = 2u,
            .microstep_limit = 8u,
            .executor = &executor,
            .hooks = &action_fixture.hooks,
            .hook_user = &action_fixture};
        check_equal(cflow_statechart_instance_init(&instance, &config),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_executor_wait_idle(&executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &instance, active, 3u, &active_count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(active_count, (size_t)2u);
        check_equal(active, expected, sizeof(expected));
        check_true(cflow_statechart_instance_get_stats(&instance, &stats));
        check_true(stats.done);
        check_false(stats.errored);
        check_equal(stats.completion_pending, (size_t)0u);
        check_equal(action_fixture.event_hook_calls, (size_t)3u);
        check_equal(action_fixture.observed_event_kinds[0],
                    CFLOW_STATECHART_OBSERVED_COMPLETION);
        check_equal(action_fixture.observed_event_kinds[1],
                    CFLOW_STATECHART_OBSERVED_COMPLETION);
        check_equal(action_fixture.observed_event_kinds[2],
                    CFLOW_STATECHART_OBSERVED_COMPLETION);
        check_equal(cflow_statechart_instance_destroy(&instance),
                    CFLOW_STATECHART_INSTANCE_OK);
        cflow_executor_destroy(&executor);
        cflow_statechart_destroy(&statechart);
    }

    it("preserves controlled exit accepted before root completion settles") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view go = {
            RTC_GO, &cmeta_type_int, &payload};
        const cflow_statechart_state_action state_actions[] = {{
            RTC_FINAL, CFLOW_STATECHART_STATE_ACTION_EXIT,
            RTC_EXEC, 0u}};
        const cflow_statechart_instance_test_hooks hooks = {
            .before_root_completion_settle = microstep_block_executor,
            .user = &blocker};
        const int expected_trace[] = {
            RTC_A, RTC_B, RTC_C, RTC_FINAL};
        cflow_statechart_instance_stats stats = {0};
        cflow_machine_state_id states[2] = {0};
        size_t state_count = 0u;
        uint64_t version = 0u;

        rtc_definition(&fixture, false, false);
        fixture.definition.state_actions = state_actions;
        fixture.definition.state_action_count = 1u;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_true(cflow_statechart_instance_set_test_hooks_internal(
            &fixture.instance, &hooks));
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        cflow_statechart_instance_request_exit(&fixture.instance);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u,
                        &state_count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(state_count, (size_t)0u);
        check_equal(fixture.trace, expected_trace, sizeof(expected_trace));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.done);
        check_true(stats.cancelled);
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(1));
        rtc_destroy(&fixture);
    }

    it("halts before selecting an internal event raised by root completion") {
        rtc_fixture fixture;
        const cflow_statechart_state_action state_actions[] = {{
            RTC_FINAL, CFLOW_STATECHART_STATE_ACTION_ENTRY, RTC_EXEC, 0u}};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, false, false);
        fixture.transitions[0].target = RTC_FINAL;
        fixture.definition.state_actions = state_actions;
        fixture.definition.state_action_count = 1u;
        fixture.raise_on_final_entry = true;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.done);
        check_false(stats.errored);
        check_equal(fixture.event_hook_calls, (size_t)1u);
        check_equal(fixture.observed_event_kinds[0],
                    CFLOW_STATECHART_OBSERVED_COMPLETION);
        rtc_destroy(&fixture);
    }

    it("fails one external macrostep when an eventless cycle reaches its bound") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, true);
        check_equal(rtc_init(&fixture, 4u, 4u, 4u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart macrostep microstep limit exceeded");
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_MICROSTEP_LIMIT_EXCEEDED);
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.external_completed, UINT64_C(0));
        check_true(stats.done);
        rtc_destroy(&fixture);
    }

    it("rolls back and fails the external event when internal raise is full") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        cflow_machine_state_id states[2] = {0};
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_A};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        fixture.raise_twice = true;
        check_equal(rtc_init(&fixture, 1u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart internal event queue is full");
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.internal_pending, (size_t)0u);
        rtc_destroy(&fixture);
    }

    it("rolls back when one microstep would overflow completion rows") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_machine_state_id expected[] = {
            RTC_ROOT, RTC_B, RTC_C};
        cflow_machine_state_id states[3] = {0};
        cflow_statechart_instance_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, false, false);
        fixture.states[3].kind = CFLOW_STATECHART_COMPOUND;
        fixture.states[4].parent = RTC_B;
        fixture.states[4].document_order = 5u;
        fixture.states[5].parent = RTC_B;
        fixture.states[5].document_order = 6u;
        fixture.states[6] = (cflow_statechart_state){
            RTC_D, RTC_B, CFLOW_STATECHART_INITIAL, 4u};
        fixture.states[7].document_order = 7u;
        fixture.transitions[1].target = RTC_C;
        fixture.transitions[2].source = RTC_C;
        fixture.transitions[2].target = RTC_FINAL;
        fixture.transitions[3] = (cflow_statechart_transition){
            4u, RTC_D, CFLOW_STATECHART_TRIGGER_EVENTLESS,
            0u, 0u, 0u, RTC_C,
            CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 3u};
        fixture.definition.transition_count = 4u;
        fixture.definition.transition_action_count = 2u;
        check_equal(rtc_init(&fixture, 4u, 1u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "Statechart completion queue is full");
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 3u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_COMPLETION_QUEUE_FULL);
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.completion_pending, (size_t)0u);
        rtc_destroy(&fixture);
    }

    it("lets a reentrant close preserve the winning commit and clear queues") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_B};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        fixture.close_during_action = true;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &other),
                    CFLOW_MAILBOX_OK);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_completed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.internal_pending, (size_t)0u);
        check_equal(stats.completion_pending, (size_t)0u);
        check_true(stats.closed);
        check_true(stats.done);
        check_false(stats.errored);
        rtc_destroy(&fixture);
    }

    it("discards a microstep when cancel linearizes before commit") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_A};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        fixture.cancel_during_action = true;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.external_failed, UINT64_C(0));
        check_true(stats.cancelled);
        check_true(stats.done);
        rtc_destroy(&fixture);
    }

    it("keeps a committed macrostep visible when cancel arrives afterward") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_D};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        cflow_statechart_instance_cancel(&fixture.instance);
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_completed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(0));
        check_true(stats.cancelled);
        rtc_destroy(&fixture);
    }

    it("keeps a committed microstep visible but cancels its unsettled macrostep") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_machine_state_id expected[] = {RTC_ROOT, RTC_B};
        cflow_machine_state_id states[2] = {0};
        cflow_statechart_instance_stats stats = {0};
        size_t count = 0u;
        uint64_t version = 0u;
        rtc_definition(&fixture, true, false);
        fixture.queue_cancel_after_commit = true;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_statechart_instance_copy_configuration(
                        &fixture.instance, states, 2u, &count, &version),
                    CFLOW_STATECHART_SNAPSHOT_OK);
        check_equal(states, expected, sizeof(expected));
        check_equal(version, UINT64_C(2));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.external_failed, UINT64_C(0));
        check_true(stats.cancelled);
        rtc_destroy(&fixture);
    }

    it("keeps mailbox OK but settles failed when executor is closed") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_executor_shutdown(&fixture.executor));
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_failed, UINT64_C(1));
        rtc_destroy(&fixture);
    }

    it("keeps clean terminal completion as the winner over later control") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats before = {0}, after = {0};
        rtc_definition(&fixture, false, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &before));
        check_true(before.done);
        check_false(before.cancelled);
        check_false(before.errored);
        cflow_statechart_instance_cancel(&fixture.instance);
        cflow_statechart_instance_close(&fixture.instance);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &after));
        check_equal(after.cancelled, before.cancelled);
        check_equal(after.last_status, before.last_status);
        check_equal(after.external_completed, before.external_completed);
        check_equal(after.external_failed, before.external_failed);
        check_equal(after.external_cancelled, before.external_cancelled);
        rtc_destroy(&fixture);
    }

    it("keeps action failure as the winner over later control") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats before = {0}, after = {0};
        rtc_definition(&fixture, true, false);
        fixture.fail_action = true;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &before));
        check_true(before.done);
        check_true(before.errored);
        check_equal(before.external_failed, UINT64_C(1));
        check_equal(cflow_statechart_instance_error(&fixture.instance),
                    "deliberate RTC action failure");
        cflow_statechart_instance_cancel(&fixture.instance);
        cflow_statechart_instance_close(&fixture.instance);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &after));
        check_true(after.errored);
        check_false(after.cancelled);
        check_equal(after.last_status, before.last_status);
        check_equal(after.external_failed, UINT64_C(1));
        check_equal(after.external_cancelled, UINT64_C(0));
        rtc_destroy(&fixture);
    }

    it("keeps executor failure as the winner over later control") {
        rtc_fixture fixture;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats before = {0}, after = {0};
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_true(cflow_executor_shutdown(&fixture.executor));
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &before));
        check_true(before.done);
        check_true(before.errored);
        check_equal(before.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(before.external_failed, UINT64_C(1));
        cflow_statechart_instance_cancel(&fixture.instance);
        cflow_statechart_instance_close(&fixture.instance);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &after));
        check_true(after.errored);
        check_false(after.cancelled);
        check_equal(after.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(after.external_failed, UINT64_C(1));
        check_equal(after.external_cancelled, UINT64_C(0));
        rtc_destroy(&fixture);
    }

    it("fails the oldest accepted event when shutdown cancels its driver") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        cflow_executor_control control = {0};
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, false);
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 2u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_as_control(&fixture.executor, &control));
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(0));
        rtc_destroy(&fixture);
    }

    it("does not overwrite explicit close when its queued driver is cancelled") {
        check_explicit_control_survives_driver_cancel(false);
    }

    it("does not overwrite explicit cancel when its queued driver is cancelled") {
        check_explicit_control_survives_driver_cancel(true);
    }

    it("does not dequeue after close wins before external receive") {
        check_control_wins_before_external_receive(false);
    }

    it("does not dequeue after cancel wins before external receive") {
        check_control_wins_before_external_receive(true);
    }

    it("settles close when its reserved microstep post is rejected") {
        rtc_fixture fixture;
        microstep_executor_blocker post_blocker;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        cflow_statechart_instance_test_hooks hooks;
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&post_blocker.entered, false);
        atomic_init(&post_blocker.release, false);
        hooks = (cflow_statechart_instance_test_hooks){
            .before_microstep_post = microstep_block_executor,
            .user = &post_blocker};
        check_true(cflow_statechart_instance_set_test_hooks_internal(
            &fixture.instance, &hooks));
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &go, UINT64_C(301)),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&post_blocker.entered)) cmeta_thread_yield();
        cflow_statechart_instance_close(&fixture.instance);
        check_true(cflow_executor_shutdown(&fixture.executor));
        atomic_store(&post_blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_true(stats.closed);
        check_true(stats.done);
        check_false(stats.cancelled);
        check_false(stats.errored);
        check_equal(stats.last_status, CFLOW_STATECHART_INSTANCE_OK);
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(stats.external_failed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.external_in_flight, (size_t)0u);
        check_equal(fixture.settlement_count, (size_t)1u);
        check_equal(fixture.settlements[0].origin_token, UINT64_C(301));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        check_equal(fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_TASK_CANCELLED);
        rtc_destroy(&fixture);
    }

    it("fails an accepted macrostep when shutdown cancels its queued microstep") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        cflow_executor_control control = {0};
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, false);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        fixture.guard_blocker = &blocker;
        fixture.guards[0] = (cflow_statechart_guard){
            RTC_QUEUE_GUARD, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        fixture.transitions[1].guard = RTC_QUEUE_GUARD;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &go, UINT64_C(401)),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(402)),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_as_control(&fixture.executor, &control));
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(fixture.settlement_count, (size_t)2u);
        check_equal(fixture.settlements[0].origin_token, UINT64_C(401));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_FAILED);
        check_equal(fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_not_null(fixture.settlements[0].error);
        check_equal(fixture.settlements[1].origin_token, UINT64_C(402));
        check_equal(fixture.settlements[1].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        check_equal(fixture.settlements[1].status,
                    CFLOW_STATECHART_INSTANCE_TASK_CANCELLED);
        check_null(fixture.settlements[1].error);
        rtc_destroy(&fixture);
    }

    it("keeps executor microstep cancellation over control before finalize") {
        rtc_fixture fixture;
        microstep_executor_blocker worker_blocker, cancel_blocker;
        cflow_executor_control control = {0};
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        cflow_statechart_instance_test_hooks hooks;
        rtc_definition(&fixture, true, false);
        atomic_init(&worker_blocker.entered, false);
        atomic_init(&worker_blocker.release, false);
        atomic_init(&cancel_blocker.entered, false);
        atomic_init(&cancel_blocker.release, false);
        fixture.guard_blocker = &worker_blocker;
        fixture.guards[0] = (cflow_statechart_guard){
            RTC_QUEUE_GUARD, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        fixture.transitions[1].guard = RTC_QUEUE_GUARD;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        hooks = (cflow_statechart_instance_test_hooks){
            .after_microstep_cancel = microstep_block_executor,
            .user = &cancel_blocker};
        check_true(cflow_statechart_instance_set_test_hooks_internal(
            &fixture.instance, &hooks));
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&worker_blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &other),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_as_control(&fixture.executor, &control));
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        atomic_store(&worker_blocker.release, true);
        while (!atomic_load(&cancel_blocker.entered)) cmeta_thread_yield();
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_true(stats.errored);
        check_false(stats.cancelled);
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(1));
        check_equal(stats.external_pending, (size_t)0u);
        check_equal(stats.external_in_flight, (size_t)0u);
        cflow_statechart_instance_cancel(&fixture.instance);
        cflow_statechart_instance_close(&fixture.instance);
        atomic_store(&cancel_blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_true(stats.errored);
        check_false(stats.cancelled);
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(1));
        rtc_destroy(&fixture);
    }

    it("reports deterministic pending transfer settle and bulk-cancel snapshots") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, false);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        fixture.guard_blocker = &blocker;
        fixture.guards[0] = (cflow_statechart_guard){
            RTC_QUEUE_GUARD, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        fixture.transitions[1].guard = RTC_QUEUE_GUARD;
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_pending, (size_t)0u);
        check_equal(stats.external_in_flight, (size_t)1u);
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(cflow_statechart_instance_try_send(
                        &fixture.instance, &other),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.external_pending, (size_t)1u);
        check_equal(stats.external_in_flight, (size_t)1u);
        cflow_statechart_instance_cancel(&fixture.instance);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.external_pending, (size_t)0u);
        check_equal(stats.external_in_flight, (size_t)1u);
        check_equal(stats.external_cancelled, UINT64_C(1));
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted, UINT64_C(2));
        check_equal(stats.external_pending, (size_t)0u);
        check_equal(stats.external_in_flight, (size_t)0u);
        check_equal(stats.external_cancelled, UINT64_C(2));
        check_true(stats.done);
        check_true(stats.cancelled);
        rtc_destroy(&fixture);
    }

    it("settles in-flight and queued tagged cancellation in FIFO order") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        rtc_definition(&fixture, true, false);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        fixture.guard_blocker = &blocker;
        fixture.guards[0] = (cflow_statechart_guard){
            RTC_QUEUE_GUARD, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        fixture.transitions[1].guard = RTC_QUEUE_GUARD;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &go, UINT64_C(201)),
                    CFLOW_MAILBOX_OK);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(202)),
                    CFLOW_MAILBOX_OK);
        cflow_statechart_instance_cancel(&fixture.instance);
        check_equal(fixture.settlement_count, (size_t)0u);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(fixture.settlement_count, (size_t)2u);
        check_equal(fixture.settlements[0].origin_token, UINT64_C(201));
        check_equal(fixture.settlements[1].origin_token, UINT64_C(202));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        check_equal(fixture.settlements[1].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED);
        check_equal(fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_TASK_CANCELLED);
        check_equal(fixture.settlements[1].status,
                    CFLOW_STATECHART_INSTANCE_TASK_CANCELLED);
        check_null(fixture.settlements[0].error);
        check_null(fixture.settlements[1].error);
        rtc_destroy(&fixture);
    }

    it("keeps instance storage alive through a terminal settlement callback") {
        rtc_fixture fixture;
        microstep_executor_blocker microstep_blocker;
        runtime_shared_executor_probe settlement_blocker;
        cmeta_thread_t destroy_thread = NULL;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        bool destroy_returned_during_callback;
        size_t wait_attempt;
        rtc_definition(&fixture, true, false);
        memset(&settlement_blocker, 0, sizeof(settlement_blocker));
        atomic_init(&microstep_blocker.entered, false);
        atomic_init(&microstep_blocker.release, false);
        atomic_init(&settlement_blocker.blocker_entered, false);
        atomic_init(&settlement_blocker.blocker_release, false);
        atomic_init(&settlement_blocker.destroy_started, false);
        atomic_init(&settlement_blocker.destroy_done, false);
        atomic_init(&settlement_blocker.destroy_status, -1);
        settlement_blocker.instance = &fixture.instance;
        fixture.guard_blocker = &microstep_blocker;
        fixture.settlement_blocker = &settlement_blocker;
        fixture.guards[0] = (cflow_statechart_guard){
            RTC_QUEUE_GUARD, &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS};
        fixture.definition.guards = fixture.guards;
        fixture.definition.guard_count = 1u;
        fixture.transitions[1].guard = RTC_QUEUE_GUARD;
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &go, UINT64_C(211)),
                    CFLOW_MAILBOX_OK);
        check_true(runtime_wait_flag(&microstep_blocker.entered));
        cflow_statechart_instance_cancel(&fixture.instance);
        atomic_store(&microstep_blocker.release, true);
        check_true(runtime_wait_flag(&settlement_blocker.blocker_entered));
        check_equal(cmeta_thread_create(
                        &destroy_thread,
                        runtime_destroy_on_shared_executor,
                        &settlement_blocker),
                    SALTS_OK);
        check_true(runtime_wait_flag(&settlement_blocker.destroy_started));
        for (wait_attempt = 0u; wait_attempt < 20u &&
             !atomic_load(&settlement_blocker.destroy_done);
             ++wait_attempt)
            cmeta_sleep_ms(1u);
        destroy_returned_during_callback =
            atomic_load(&settlement_blocker.destroy_done);
        check_false(destroy_returned_during_callback);
        atomic_store(&settlement_blocker.blocker_release, true);
        check_equal(cmeta_thread_join(&destroy_thread), SALTS_OK);
        check_equal(atomic_load(&settlement_blocker.destroy_status),
                    (int)CFLOW_STATECHART_INSTANCE_OK);
        check_null(fixture.instance.impl);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("keeps reserved repost storage alive through settlement delivery") {
        rtc_fixture fixture;
        microstep_executor_blocker worker_blocker, repost_blocker;
        runtime_shared_executor_probe settlement_blocker;
        cflow_statechart_instance_test_hooks test_hooks;
        cflow_executor_control control = {0};
        cmeta_thread_t destroy_thread = NULL;
        const int payload = 1;
        const cflow_event_view other = {
            RTC_OTHER, &cmeta_type_int, &payload};
        bool destroy_returned_during_callback;
        size_t wait_attempt;
        rtc_definition(&fixture, true, false);
        memset(&settlement_blocker, 0, sizeof(settlement_blocker));
        atomic_init(&worker_blocker.entered, false);
        atomic_init(&worker_blocker.release, false);
        atomic_init(&repost_blocker.entered, false);
        atomic_init(&repost_blocker.release, false);
        atomic_init(&settlement_blocker.blocker_entered, false);
        atomic_init(&settlement_blocker.blocker_release, false);
        atomic_init(&settlement_blocker.destroy_started, false);
        atomic_init(&settlement_blocker.destroy_done, false);
        atomic_init(&settlement_blocker.destroy_status, -1);
        settlement_blocker.instance = &fixture.instance;
        fixture.settlement_blocker = &settlement_blocker;
        fixture.blocked_settlement_token = UINT64_C(802);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init_with_external(
                        &fixture, 2u, 0u, 4u, 16u, 4u, 4u),
                    CFLOW_STATECHART_INSTANCE_OK);
        test_hooks = (cflow_statechart_instance_test_hooks){
            .before_driver_repost = microstep_block_executor,
            .user = &repost_blocker};
        check_true(cflow_statechart_instance_set_test_hooks_internal(
            &fixture.instance, &test_hooks));
        check_equal(cflow_executor_try_post(
                        &fixture.executor,
                        microstep_block_executor, &worker_blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(runtime_wait_flag(&worker_blocker.entered));
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(801)),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &other, UINT64_C(802)),
                    CFLOW_MAILBOX_OK);
        atomic_store(&worker_blocker.release, true);
        check_true(runtime_wait_flag(&repost_blocker.entered));
        check_true(cflow_executor_as_control(&fixture.executor, &control));
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        atomic_store(&repost_blocker.release, true);
        check_true(runtime_wait_flag(&settlement_blocker.blocker_entered));
        check_equal(cmeta_thread_create(
                        &destroy_thread,
                        runtime_destroy_on_shared_executor,
                        &settlement_blocker),
                    SALTS_OK);
        check_true(runtime_wait_flag(&settlement_blocker.destroy_started));
        for (wait_attempt = 0u; wait_attempt < 20u &&
             !atomic_load(&settlement_blocker.destroy_done);
             ++wait_attempt)
            cmeta_sleep_ms(1u);
        destroy_returned_during_callback =
            atomic_load(&settlement_blocker.destroy_done);
        check_false(destroy_returned_during_callback);
        atomic_store(&settlement_blocker.blocker_release, true);
        check_equal(cmeta_thread_join(&destroy_thread), SALTS_OK);
        check_equal(atomic_load(&settlement_blocker.destroy_status),
                    (int)CFLOW_STATECHART_INSTANCE_OK);
        check_null(fixture.instance.impl);
        check_equal(fixture.settlement_count, (size_t)2u);
        check_equal(fixture.settlements[0].origin_token, UINT64_C(801));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_COMPLETED);
        check_equal(fixture.settlements[1].origin_token, UINT64_C(802));
        check_equal(fixture.settlements[1].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_FAILED);
        check_equal(fixture.settlements[1].status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("keeps mailbox OK but settles failed when executor post is full") {
        rtc_fixture fixture;
        microstep_executor_blocker blocker;
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init(&fixture, 4u, 4u, 16u, 1u),
                    CFLOW_STATECHART_INSTANCE_OK);
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor, microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        check_equal(cflow_executor_try_post(
                        &fixture.executor, microstep_noop, NULL),
                    CFLOW_ADMISSION_ACCEPTED);
        check_equal(cflow_statechart_instance_try_send_tagged(
                        &fixture.instance, &go, UINT64_C(501)),
                    CFLOW_MAILBOX_OK);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.last_status, CFLOW_STATECHART_INSTANCE_EXECUTOR_FULL);
        check_equal(stats.external_accepted, UINT64_C(1));
        check_equal(stats.external_failed, UINT64_C(1));
        check_equal(stats.external_cancelled, UINT64_C(0));
        check_equal(stats.external_completed, UINT64_C(0));
        check_equal(stats.external_accepted,
                    stats.external_completed + stats.external_failed +
                        stats.external_cancelled +
                        (uint64_t)stats.external_pending +
                        (uint64_t)stats.external_in_flight);
        check_equal(fixture.settlement_count, (size_t)1u);
        check_equal(fixture.settlements[0].origin_token, UINT64_C(501));
        check_equal(fixture.settlements[0].kind,
                    CFLOW_STATECHART_EXTERNAL_SETTLED_FAILED);
        check_equal(fixture.settlements[0].status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_FULL);
        check_not_null(fixture.settlements[0].error);
        rtc_destroy(&fixture);
    }

    it("serializes all events admitted by concurrent producers") {
        enum {
            PRODUCER_COUNT = 4,
            EVENTS_PER_PRODUCER = 8,
            TOTAL_OTHER_EVENTS = PRODUCER_COUNT * EVENTS_PER_PRODUCER
        };
        rtc_fixture fixture;
        rtc_producer_context context;
        cmeta_thread_t producers[PRODUCER_COUNT] = {0};
        cmeta_thread_t poller = {0};
        atomic_int failures;
        atomic_int polls;
        atomic_int violations;
        atomic_bool stop;
        atomic_uint_fast64_t next_token;
        bool seen_tokens[TOTAL_OTHER_EVENTS + 1u] = {false};
        const int payload = 1;
        const cflow_event_view go = {RTC_GO, &cmeta_type_int, &payload};
        cflow_statechart_instance_stats stats = {0};
        rtc_stats_poller_context poller_context;
        size_t index;
        rtc_definition(&fixture, true, false);
        fixture.hooks = (cflow_statechart_instance_hooks){
            .abi_version = CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5,
            .struct_size = sizeof(cflow_statechart_instance_hooks),
            .on_host_transaction = rtc_host_transaction,
            .on_external_settlement = rtc_external_settlement};
        check_equal(rtc_init_with_external(
                        &fixture, TOTAL_OTHER_EVENTS, 0u, 4u, 4u,
                        16u, 8u),
                    CFLOW_STATECHART_INSTANCE_OK);
        check_equal(cflow_statechart_instance_try_send(&fixture.instance, &go),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        atomic_init(&failures, 0);
        atomic_init(&polls, 0);
        atomic_init(&violations, 0);
        atomic_init(&stop, false);
        atomic_init(&next_token, UINT64_C(1));
        context = (rtc_producer_context){
            &fixture.instance, EVENTS_PER_PRODUCER, &failures,
            &next_token};
        poller_context = (rtc_stats_poller_context){
            &fixture.instance, &stop, &polls, &violations};
        check_equal(cmeta_thread_create(
            &poller, rtc_stats_poller, &poller_context), 0);
        while (atomic_load(&polls) == 0) cmeta_thread_yield();
        for (index = 0u; index < PRODUCER_COUNT; ++index)
            check_equal(cmeta_thread_create(
                &producers[index], rtc_producer, &context), 0);
        for (index = 0u; index < PRODUCER_COUNT; ++index)
            check_equal(cmeta_thread_join(&producers[index]), 0);
        check_equal(atomic_load(&failures), 0);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        atomic_store(&stop, true);
        check_equal(cmeta_thread_join(&poller), 0);
        check_true(atomic_load(&polls) > 0);
        check_equal(atomic_load(&violations), 0);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.external_accepted,
                    UINT64_C(1) + (uint64_t)TOTAL_OTHER_EVENTS);
        check_equal(stats.external_completed, stats.external_accepted);
        check_equal(stats.external_failed, UINT64_C(0));
        check_equal(stats.external_cancelled, UINT64_C(0));
        check_equal(stats.external_pending, (size_t)0u);
        check_equal(stats.external_in_flight, (size_t)0u);
        check_equal(fixture.settlement_count,
                    (size_t)TOTAL_OTHER_EVENTS);
        check_false(atomic_load(&fixture.settlement_callback_overlap));
        check_equal(fixture.preprocess_hook_calls,
                    (size_t)TOTAL_OTHER_EVENTS + 1u);
        check_equal(fixture.observed_origin_tokens[0], UINT64_C(0));
        for (index = 0u; index < fixture.settlement_count; ++index) {
            const uint64_t token =
                fixture.settlements[index].origin_token;
            check_equal(token,
                        fixture.observed_origin_tokens[index + 1u]);
            check_true(token >= UINT64_C(1));
            check_true(token <= (uint64_t)TOTAL_OTHER_EVENTS);
            check_false(seen_tokens[token]);
            seen_tokens[token] = true;
            check_equal(fixture.settlements[index].kind,
                        CFLOW_STATECHART_EXTERNAL_SETTLED_COMPLETED);
        }
        rtc_destroy(&fixture);
    }
}

typedef struct statechart_timer_fixture {
    cflow_statechart_state states[11];
    cflow_event_type events[2];
    cflow_statechart_transition transitions[5];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_clock clock;
    cflow_statechart_instance instance;
    int initial_state;
} statechart_timer_fixture;

enum {
    TIMER_SC_ROOT = 1000u,
    TIMER_SC_INITIAL = 900u,
    TIMER_SC_PARALLEL = 800u,
    TIMER_SC_LEFT = 700u,
    TIMER_SC_LEFT_INITIAL = 600u,
    TIMER_SC_LEFT_A = 500u,
    TIMER_SC_LEFT_B = 400u,
    TIMER_SC_RIGHT = 300u,
    TIMER_SC_RIGHT_INITIAL = 200u,
    TIMER_SC_RIGHT_A = 100u,
    TIMER_SC_OUTSIDE = 50u,
    TIMER_SC_LEFT_EVENT = 10u,
    TIMER_SC_OUT_EVENT = 11u
};

static void statechart_timer_definition(statechart_timer_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        TIMER_SC_ROOT, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        TIMER_SC_INITIAL, TIMER_SC_ROOT, CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        TIMER_SC_PARALLEL, TIMER_SC_ROOT, CFLOW_STATECHART_PARALLEL, 2u};
    fixture->states[3] = (cflow_statechart_state){
        TIMER_SC_LEFT, TIMER_SC_PARALLEL, CFLOW_STATECHART_COMPOUND, 3u};
    fixture->states[4] = (cflow_statechart_state){
        TIMER_SC_LEFT_INITIAL, TIMER_SC_LEFT,
        CFLOW_STATECHART_INITIAL, 4u};
    fixture->states[5] = (cflow_statechart_state){
        TIMER_SC_LEFT_A, TIMER_SC_LEFT, CFLOW_STATECHART_ATOMIC, 5u};
    fixture->states[6] = (cflow_statechart_state){
        TIMER_SC_LEFT_B, TIMER_SC_LEFT, CFLOW_STATECHART_ATOMIC, 6u};
    fixture->states[7] = (cflow_statechart_state){
        TIMER_SC_RIGHT, TIMER_SC_PARALLEL, CFLOW_STATECHART_COMPOUND, 7u};
    fixture->states[8] = (cflow_statechart_state){
        TIMER_SC_RIGHT_INITIAL, TIMER_SC_RIGHT,
        CFLOW_STATECHART_INITIAL, 8u};
    fixture->states[9] = (cflow_statechart_state){
        TIMER_SC_RIGHT_A, TIMER_SC_RIGHT, CFLOW_STATECHART_ATOMIC, 9u};
    fixture->states[10] = (cflow_statechart_state){
        TIMER_SC_OUTSIDE, TIMER_SC_ROOT, CFLOW_STATECHART_ATOMIC, 10u};
    fixture->events[0] = (cflow_event_type){
        TIMER_SC_LEFT_EVENT, &cmeta_type_int};
    fixture->events[1] = (cflow_event_type){
        TIMER_SC_OUT_EVENT, &cmeta_type_int};
    fixture->transitions[0] = (cflow_statechart_transition){
        1u, TIMER_SC_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, TIMER_SC_PARALLEL,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        2u, TIMER_SC_LEFT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, TIMER_SC_LEFT_A,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        3u, TIMER_SC_RIGHT_INITIAL, CFLOW_STATECHART_TRIGGER_EVENTLESS,
        0u, 0u, 0u, TIMER_SC_RIGHT_A,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
    fixture->transitions[3] = (cflow_statechart_transition){
        4u, TIMER_SC_LEFT_A, CFLOW_STATECHART_TRIGGER_EVENT,
        TIMER_SC_LEFT_EVENT, 0u, 0u, TIMER_SC_LEFT_B,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 3u};
    fixture->transitions[4] = (cflow_statechart_transition){
        5u, TIMER_SC_PARALLEL, CFLOW_STATECHART_TRIGGER_EVENT,
        TIMER_SC_OUT_EVENT, 0u, 0u, TIMER_SC_OUTSIDE,
        CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 4u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int, fixture->states, 11u, fixture->events, 2u,
        NULL, 0u, NULL, 0u, fixture->transitions, 5u,
        NULL, 0u, NULL, 0u};
    fixture->initial_state = 1;
}

static bool statechart_timer_fixture_init(
    statechart_timer_fixture *fixture,
    size_t external_capacity,
    size_t timer_capacity) {
    cflow_statechart_instance_config config;
    statechart_timer_definition(fixture);
    if (cflow_statechart_build(&fixture->statechart, &fixture->definition) !=
            CFLOW_STATECHART_OK ||
        !cflow_executor_serial_init(&fixture->executor) ||
        !cflow_clock_virtual_init(&fixture->clock, (cflow_instant){0u}))
        return false;
    config = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .external_event_capacity = external_capacity,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 64u,
        .executor = &fixture->executor,
        .clock = &fixture->clock,
        .timer_capacity = timer_capacity};
    return cflow_statechart_instance_init(&fixture->instance, &config) ==
        CFLOW_STATECHART_INSTANCE_OK;
}

static void statechart_timer_fixture_destroy(
    statechart_timer_fixture *fixture) {
    check_equal(cflow_statechart_instance_destroy(&fixture->instance),
                CFLOW_STATECHART_INSTANCE_OK);
    cflow_clock_destroy(&fixture->clock);
    cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

static void statechart_timer_send(
    statechart_timer_fixture *fixture, cflow_event_id id) {
    const int payload = 1;
    const cflow_event_view event = {id, &cmeta_type_int, &payload};
    check_equal(cflow_statechart_instance_try_send(&fixture->instance, &event),
                CFLOW_MAILBOX_OK);
    check_true(cflow_executor_wait_idle(&fixture->executor));
}

static cflow_timer_event_schedule_result statechart_timer_schedule(
    statechart_timer_fixture *fixture,
    cflow_machine_state_id scope,
    cflow_event_id event_id,
    uint64_t deadline) {
    const int payload = 1;
    const cflow_event_view event = {
        event_id, &cmeta_type_int, &payload};
    return cflow_statechart_instance_try_schedule_at(
        &fixture->instance, scope, (cflow_deadline){deadline}, &event);
}

suite("CFlow Statechart configuration-scoped timers") {
    it("rejects partial timer configuration and capacity overflow") {
        statechart_timer_fixture fixture;
        cflow_statechart_instance_config config;
        statechart_timer_definition(&fixture);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        check_true(cflow_clock_virtual_init(
            &fixture.clock, (cflow_instant){0u}));
        config = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 2u,
            .internal_event_capacity = 2u,
            .completion_capacity = 2u,
            .microstep_limit = 16u,
            .executor = &fixture.executor,
            .timer_capacity = 1u};
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        config.clock = &fixture.clock;
        config.timer_capacity = 0u;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        config.timer_capacity = SIZE_MAX;
        check_equal(cflow_statechart_instance_init(&fixture.instance, &config),
                    CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED);
        cflow_clock_destroy(&fixture.clock);
        cflow_executor_destroy(&fixture.executor);
        cflow_statechart_destroy(&fixture.statechart);
    }

    it("cancels exactly exited active scopes before publication") {
        statechart_timer_fixture fixture;
        cflow_statechart_instance_stats stats = {0};
        cflow_timer_event_schedule_result delayed_result;
        const int payload = 1;
        const cflow_event_view delayed = {
            TIMER_SC_LEFT_EVENT, &cmeta_type_int, &payload};
        const cflow_machine_state_id scopes[] = {
            TIMER_SC_ROOT, TIMER_SC_PARALLEL, TIMER_SC_LEFT,
            TIMER_SC_LEFT_A, TIMER_SC_RIGHT, TIMER_SC_RIGHT_A};
        size_t index;
        check_true(statechart_timer_fixture_init(&fixture, 4u, 8u));
        for (index = 0u; index < sizeof(scopes) / sizeof(scopes[0]); ++index)
            check_equal(statechart_timer_schedule(
                            &fixture, scopes[index], TIMER_SC_LEFT_EVENT,
                            UINT64_C(100)).status,
                        CFLOW_TIMER_EVENT_OK);

        statechart_timer_send(&fixture, TIMER_SC_LEFT_EVENT);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.timers.pending, (size_t)5u);
        check_equal(stats.timers.cancelled, UINT64_C(1));
        check_equal(statechart_timer_schedule(
                        &fixture, TIMER_SC_LEFT_A, TIMER_SC_LEFT_EVENT,
                        UINT64_C(100)).status,
                    CFLOW_TIMER_EVENT_INVALID_ARGUMENT);
        delayed_result = cflow_statechart_instance_try_schedule_after(
            &fixture.instance, TIMER_SC_LEFT_B,
            (cflow_duration){UINT64_C(100)}, &delayed);
        check_equal(delayed_result.status, CFLOW_TIMER_EVENT_OK);

        statechart_timer_send(&fixture, TIMER_SC_OUT_EVENT);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.timers.scheduled, UINT64_C(7));
        check_equal(stats.timers.pending, (size_t)1u);
        check_equal(stats.timers.cancelled, UINT64_C(6));
        check_equal(cflow_statechart_instance_run_one_ready_timer(
                        &fixture.instance).status,
                    CFLOW_TIMER_EVENT_FIRE_NOT_READY);
        statechart_timer_fixture_destroy(&fixture);
    }

    it("preserves equal-deadline FIFO mailbox FULL and FIRE_WON") {
        statechart_timer_fixture fixture;
        microstep_executor_blocker blocker;
        cflow_timer_event_schedule_result first, second, claimed;
        cflow_timer_event_fire_result fired;
        cflow_timer_event_claim claim = {0};
        check_true(statechart_timer_fixture_init(&fixture, 1u, 3u));
        atomic_init(&blocker.entered, false);
        atomic_init(&blocker.release, false);
        check_equal(cflow_executor_try_post(
                        &fixture.executor, microstep_block_executor, &blocker),
                    CFLOW_ADMISSION_ACCEPTED);
        while (!atomic_load(&blocker.entered)) cmeta_thread_yield();
        first = statechart_timer_schedule(
            &fixture, TIMER_SC_ROOT, TIMER_SC_LEFT_EVENT, 0u);
        second = statechart_timer_schedule(
            &fixture, TIMER_SC_ROOT, TIMER_SC_LEFT_EVENT, 0u);
        check_equal(first.status, CFLOW_TIMER_EVENT_OK);
        check_equal(second.status, CFLOW_TIMER_EVENT_OK);
        fired = cflow_statechart_instance_run_one_ready_timer(
            &fixture.instance);
        check_equal(fired.status, CFLOW_TIMER_EVENT_FIRE_DELIVERED);
        check_equal(fired.timer_id, first.timer_id);
        fired = cflow_statechart_instance_run_one_ready_timer(
            &fixture.instance);
        check_equal(fired.status, CFLOW_TIMER_EVENT_FIRE_MAILBOX_REJECTED);
        check_equal(fired.mailbox_status, CFLOW_MAILBOX_FULL);
        check_equal(fired.timer_id, second.timer_id);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));

        claimed = statechart_timer_schedule(
            &fixture, TIMER_SC_ROOT, TIMER_SC_OUT_EVENT, 0u);
        check_equal(claimed.status, CFLOW_TIMER_EVENT_OK);
        check_true(cflow_statechart_instance_claim_timer_internal(
            &fixture.instance, &claim, &fired));
        check_equal(cflow_statechart_instance_cancel_timer(
                        &fixture.instance, claimed.timer_id),
                    CFLOW_TIMER_EVENT_FIRE_WON);
        fired = cflow_timer_event_queue_commit_claim(&claim);
        check_equal(fired.status, CFLOW_TIMER_EVENT_FIRE_DELIVERED);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        statechart_timer_fixture_destroy(&fixture);
    }

    it("closes and cancels all pending timers with the terminal winner") {
        statechart_timer_fixture fixture;
        cflow_statechart_instance_stats stats = {0};
        check_true(statechart_timer_fixture_init(&fixture, 2u, 2u));
        check_equal(statechart_timer_schedule(
                        &fixture, TIMER_SC_ROOT, TIMER_SC_LEFT_EVENT,
                        UINT64_C(100)).status,
                    CFLOW_TIMER_EVENT_OK);
        cflow_statechart_instance_close(&fixture.instance);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.timers.cancelled, UINT64_C(1));
        check_true(stats.timers.closed);
        check_equal(statechart_timer_schedule(
                        &fixture, TIMER_SC_ROOT, TIMER_SC_LEFT_EVENT,
                        UINT64_C(100)).status,
                    CFLOW_TIMER_EVENT_CLOSED);
        statechart_timer_fixture_destroy(&fixture);

        check_true(statechart_timer_fixture_init(&fixture, 2u, 2u));
        check_equal(statechart_timer_schedule(
                        &fixture, TIMER_SC_ROOT, TIMER_SC_LEFT_EVENT,
                        UINT64_C(100)).status,
                    CFLOW_TIMER_EVENT_OK);
        cflow_statechart_instance_cancel(&fixture.instance);
        check_true(cflow_statechart_instance_get_stats(
            &fixture.instance, &stats));
        check_equal(stats.timers.cancelled, UINT64_C(1));
        check_true(stats.timers.closed);
        statechart_timer_fixture_destroy(&fixture);
    }
}
