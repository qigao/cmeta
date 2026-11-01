#include <cflow/actor.h>
#include "cflow_ace_active_object_pattern.h"

#include <clock.h>
#include <thread.h>

#include "tinytest.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
    ACTOR_EDGE_EVENT_TYPES = 4,
    ACTOR_EDGE_OBSERVATIONS = 256,
    ACTOR_TEST_TIMEOUT_MS = 5000
};

typedef struct actor_probe {
    atomic_int action_calls;
    atomic_int values;
    atomic_int errors;
    atomic_int dones;
    atomic_long last_value;
} actor_probe;

typedef struct actor_blocker {
    atomic_bool entered;
    atomic_bool release;
} actor_blocker;

typedef struct actor_fixture {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    cflow_actor actor;
    cflow_machine_action_binding action_binding;
    actor_probe probe;
    int initial_state;
} actor_fixture;

typedef struct actor_edge_probe {
    cflow_actor *actor;
    const void *expected_owner_thread;
    atomic_bool wrong_owner_thread;
    atomic_bool wrong_sink_owner_thread;
    cflow_actor_ref *self_ref;
    actor_blocker *blocker;
    actor_blocker *done_blocker;
    atomic_bool block_done;
    atomic_bool block_action;
    atomic_bool fail_guard;
    atomic_bool fail_action;
    atomic_bool self_send;
    atomic_bool self_sent;
    atomic_bool stop_on_value;
    atomic_bool stop_requested;
    atomic_bool reject_value;
    /* Borrowed typed Strategy, live through actor destruction. */
    ace_actor_strategy *ace_strategy;
    atomic_int action_calls;
    atomic_int values;
    atomic_int errors;
    atomic_int dones;
    atomic_int self_send_status;
    atomic_int stop_status;
    atomic_int seen[ACTOR_EDGE_OBSERVATIONS];
    atomic_int pair_seen[ACTOR_EDGE_EVENT_TYPES][ACTOR_EDGE_OBSERVATIONS];
    int observations[ACTOR_EDGE_OBSERVATIONS];
} actor_edge_probe;

typedef struct actor_edge_action_context {
    actor_edge_probe *probe;
    cflow_event_id event_id;
} actor_edge_action_context;

typedef struct actor_edge_fixture {
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    cflow_actor actor;
    cflow_machine_guard_binding guard_bindings[ACTOR_EDGE_EVENT_TYPES];
    cflow_machine_action_binding action_bindings[ACTOR_EDGE_EVENT_TYPES];
    actor_edge_action_context action_contexts[ACTOR_EDGE_EVENT_TYPES];
    actor_edge_probe probe;
    int initial_state;
} actor_edge_fixture;

typedef struct actor_sender_context {
    const cflow_actor_ref *ref;
    cflow_event_id event_id;
    int first_payload;
    int count;
    atomic_bool *go;
    atomic_int *attempted;
    atomic_int accepted;
    atomic_int full;
    atomic_int stopping;
    atomic_int stopped;
    atomic_int failed;
    atomic_int stale;
    atomic_int unexpected;
    atomic_bool completed;
} actor_sender_context;

typedef struct actor_wait_stats_context {
    cflow_actor *actor;
    atomic_bool started;
    atomic_bool completed;
    cflow_actor_state state;
    bool stats_valid;
    cflow_actor_stats stats;
} actor_wait_stats_context;

typedef struct actor_destroy_context {
    cflow_actor *actor;
    atomic_bool *returned;
    atomic_bool *started;
} actor_destroy_context;

static bool wait_until_true(atomic_bool *value) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        if (atomic_load(value)) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(value);
}

static bool wait_until_at_least(atomic_int *value, int expected) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        if (atomic_load(value) >= expected) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(value) >= expected;
}

static bool wait_actor_state(const cflow_actor *actor,
                             cflow_actor_state expected) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        if (cflow_actor_current_state(actor) == expected) return true;
        cmeta_sleep_ms(1u);
    }
    return cflow_actor_current_state(actor) == expected;
}

static bool wait_actor_terminal(const cflow_actor *actor) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        const cflow_actor_state state = cflow_actor_current_state(actor);
        if (state == CFLOW_ACTOR_STATE_STOPPED ||
            state == CFLOW_ACTOR_STATE_FAILED)
            return true;
        cmeta_sleep_ms(1u);
    }
    return cflow_actor_current_state(actor) == CFLOW_ACTOR_STATE_STOPPED ||
           cflow_actor_current_state(actor) == CFLOW_ACTOR_STATE_FAILED;
}

static void block_scheduler(void *user) {
    actor_blocker *blocker = (actor_blocker *)user;
    const uint64_t started = cmeta_monotonic_ms();
    atomic_store(&blocker->entered, true);
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS &&
           !atomic_load(&blocker->release))
        cmeta_sleep_ms(1u);
}

static void actor_scheduler_slot(void *user) {
    (void)user;
}

static bool actor_edge_guard(void *user,
                             const void *state,
                             const void *event,
                             bool *out_enabled,
                             const char **out_error) {
    actor_edge_probe *probe = (actor_edge_probe *)user;
    if (probe == NULL || state == NULL || event == NULL ||
        out_enabled == NULL || out_error == NULL)
        return false;
    if (atomic_load(&probe->fail_guard)) {
        *out_error = "actor edge guard failure";
        return false;
    }
    *out_enabled = true;
    *out_error = NULL;
    return true;
}

static bool actor_edge_action(void *user,
                              const void *state,
                              const void *event,
                              void *out_target_state,
                              void *out_observation,
                              const char **out_error) {
    actor_edge_action_context *context =
        (actor_edge_action_context *)user;
    actor_edge_probe *probe = context != NULL ? context->probe : NULL;
    const int payload = event != NULL ? *(const int *)event : -1;
    if (probe == NULL || state == NULL || event == NULL ||
        out_target_state == NULL || out_observation == NULL ||
        out_error == NULL)
        return false;
    if (probe->expected_owner_thread != NULL &&
        probe->expected_owner_thread != cmeta_thread_current_token())
        atomic_store(&probe->wrong_owner_thread, true);
    atomic_fetch_add(&probe->action_calls, 1);
    if (atomic_load(&probe->block_action) && probe->blocker != NULL) {
        const uint64_t started = cmeta_monotonic_ms();
        atomic_store(&probe->blocker->entered, true);
        while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS &&
               !atomic_load(&probe->blocker->release))
            cmeta_sleep_ms(1u);
        if (!atomic_load(&probe->blocker->release)) {
            *out_error = "actor edge action timed out";
            return false;
        }
    }
    if (atomic_load(&probe->fail_action)) {
        *out_error = "actor edge action failure";
        return false;
    }
    if (context->event_id >= 100u &&
        context->event_id < 100u + ACTOR_EDGE_EVENT_TYPES &&
        payload >= 0 && payload < ACTOR_EDGE_OBSERVATIONS) {
        atomic_fetch_add(
            &probe->pair_seen[context->event_id - 100u][payload], 1);
    }
    if (atomic_load(&probe->self_send) && payload == 1 &&
        !atomic_exchange(&probe->self_sent, true)) {
        const int nested_payload = 2;
        const cflow_event_view nested = {
            101u, &cmeta_type_int, &nested_payload};
        atomic_store(&probe->self_send_status,
                     (int)cflow_actor_ref_try_send(probe->self_ref, &nested));
    }
    *(int *)out_target_state = *(const int *)state + 1;
    *(int *)out_observation = probe->ace_strategy != NULL
        ? ace_actor_strategy_transform(probe->ace_strategy, payload)
        : payload;
    *out_error = NULL;
    return true;
}

static bool actor_edge_on_value(void *user,
                                const cmeta_type_desc *type,
                                const void *value) {
    actor_edge_probe *probe = (actor_edge_probe *)user;
    int index;
    int payload;
    if (probe == NULL || !cmeta_type_equal(type, &cmeta_type_int) ||
        value == NULL)
        return false;
    payload = *(const int *)value;
    if (probe->expected_owner_thread != NULL &&
        probe->expected_owner_thread != cmeta_thread_current_token())
        atomic_store(&probe->wrong_sink_owner_thread, true);
    index = atomic_load(&probe->values);
    if (index >= 0 && index < ACTOR_EDGE_OBSERVATIONS)
        probe->observations[index] = payload;
    if (payload >= 0 && payload < ACTOR_EDGE_OBSERVATIONS)
        atomic_fetch_add(&probe->seen[payload], 1);
    atomic_fetch_add(&probe->values, 1);
    if (atomic_load(&probe->stop_on_value) &&
        !atomic_exchange(&probe->stop_requested, true)) {
        atomic_store(&probe->stop_status,
                     (int)cflow_actor_request_stop(probe->actor));
    }
    return !atomic_load(&probe->reject_value);
}

static void actor_edge_on_error(void *user, const char *message) {
    actor_edge_probe *probe = (actor_edge_probe *)user;
    if (probe != NULL && message != NULL)
        atomic_fetch_add(&probe->errors, 1);
}

static void actor_edge_on_done(void *user) {
    actor_edge_probe *probe = (actor_edge_probe *)user;
    if (probe == NULL) return;
    if (atomic_load(&probe->block_done) && probe->done_blocker != NULL) {
        const uint64_t started = cmeta_monotonic_ms();
        atomic_store(&probe->done_blocker->entered, true);
        while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS &&
               !atomic_load(&probe->done_blocker->release))
            cmeta_sleep_ms(1u);
    }
    atomic_fetch_add(&probe->dones, 1);
}

static bool actor_edge_fixture_init_with_mode(
    actor_edge_fixture *fixture,
    size_t mailbox_capacity,
    size_t scheduler_ready_capacity,
    size_t scheduler_timer_capacity,
    bool owner_executor, bool owner_scheduler) {
    cflow_machine_state states[1];
    cflow_event_type events[ACTOR_EDGE_EVENT_TYPES];
    cflow_machine_guard guards[ACTOR_EDGE_EVENT_TYPES];
    cflow_machine_action actions[ACTOR_EDGE_EVENT_TYPES];
    cflow_machine_transition transitions[ACTOR_EDGE_EVENT_TYPES];
    cflow_machine_definition definition;
    cflow_actor_config config = {0};
    size_t index;

    *fixture = (actor_edge_fixture){0};
    fixture->initial_state = 0;
    states[0] = (cflow_machine_state){
        10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    for (index = 0u; index < ACTOR_EDGE_EVENT_TYPES; ++index) {
        const cflow_event_id event_id = (cflow_event_id)(100u + index);
        const cflow_machine_guard_id guard_id =
            (cflow_machine_guard_id)(200u + index);
        const cflow_machine_action_id action_id =
            (cflow_machine_action_id)(300u + index);
        events[index] = (cflow_event_type){event_id, &cmeta_type_int};
        guards[index] = (cflow_machine_guard){
            guard_id, &cmeta_type_int, event_id, &cmeta_type_int,
            CMETA_EFFECT_PURE,
            CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
        actions[index] = (cflow_machine_action){
            action_id, &cmeta_type_int, event_id, &cmeta_type_int,
            &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
            CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
            CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_int, 0u};
        transitions[index] = (cflow_machine_transition){
            10u, event_id, guard_id, action_id, 10u, 1u};
        fixture->guard_bindings[index] = (cflow_machine_guard_binding){
            guard_id, actor_edge_guard, &fixture->probe};
        fixture->action_contexts[index] = (actor_edge_action_context){
            &fixture->probe, event_id};
        fixture->action_bindings[index] = (cflow_machine_action_binding){
            action_id, actor_edge_action, &fixture->action_contexts[index]};
    }
    definition = (cflow_machine_definition){
        states, 1u, 10u,
        events, ACTOR_EDGE_EVENT_TYPES,
        guards, ACTOR_EDGE_EVENT_TYPES,
        actions, ACTOR_EDGE_EVENT_TYPES,
        transitions, ACTOR_EDGE_EVENT_TYPES};
    if (cflow_machine_build(&fixture->machine, &definition) !=
        CFLOW_MACHINE_OK)
        return false;
    if (owner_executor) {
        if (!cflow_executor_owner_init_with_capacity(
                &fixture->executor, 64u, NULL, NULL)) return false;
    } else {
        if (!cflow_executor_serial_init(&fixture->executor)) return false;
    }
    if (owner_scheduler) {
        if (!cflow_scheduler_owner_bind(&fixture->scheduler,
                                         &fixture->executor, 32u))
            return false;
    } else {
        if (!cflow_scheduler_worker_init_with_capacity(
                &fixture->scheduler, 1u, scheduler_ready_capacity,
                scheduler_timer_capacity))
            return false;
    }
    fixture->probe.actor = &fixture->actor;
    atomic_init(&fixture->probe.self_send_status,
                (int)CFLOW_ACTOR_SEND_INVALID_ARGUMENT);
    atomic_init(&fixture->probe.stop_status, (int)CFLOW_ACTOR_FAILED);
    config.machine = (cflow_machine_instance_config){
        &fixture->machine,
        &fixture->initial_state,
        &cmeta_type_int,
        fixture->guard_bindings,
        ACTOR_EDGE_EVENT_TYPES,
        fixture->action_bindings,
        ACTOR_EDGE_EVENT_TYPES,
        mailbox_capacity,
        &fixture->executor};
    config.scheduler = &fixture->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        actor_edge_on_value,
        actor_edge_on_error,
        actor_edge_on_done,
        &fixture->probe};
    return cflow_actor_init(&fixture->actor, &config).status == CFLOW_ACTOR_OK;
}

static bool actor_edge_fixture_init_with_scheduler_capacity(
    actor_edge_fixture *fixture, size_t mailbox_capacity,
    size_t ready_capacity, size_t timer_capacity) {
    return actor_edge_fixture_init_with_mode(
        fixture, mailbox_capacity, ready_capacity, timer_capacity, false, false);
}

static bool actor_edge_fixture_init_owner(
    actor_edge_fixture *fixture, size_t mailbox_capacity) {
    return actor_edge_fixture_init_with_mode(
        fixture, mailbox_capacity, CFLOW_EXECUTOR_DEFAULT_CAPACITY,
        CFLOW_TIMER_DEFAULT_CAPACITY, true, false);
}

static bool actor_edge_fixture_init_full_owner(
    actor_edge_fixture *fixture, size_t mailbox_capacity) {
    return actor_edge_fixture_init_with_mode(
        fixture, mailbox_capacity, 32u, CFLOW_TIMER_DEFAULT_CAPACITY,
        true, true);
}

static bool actor_edge_fixture_init(actor_edge_fixture *fixture,
                                    size_t mailbox_capacity) {
    return actor_edge_fixture_init_with_scheduler_capacity(
        fixture, mailbox_capacity, CFLOW_EXECUTOR_DEFAULT_CAPACITY,
        CFLOW_TIMER_DEFAULT_CAPACITY);
}

static void actor_edge_fixture_destroy(actor_edge_fixture *fixture) {
    cflow_actor_destroy(&fixture->actor);
    if (cflow_scheduler_valid(&fixture->scheduler))
        cflow_scheduler_destroy(&fixture->scheduler);
    if (cflow_executor_valid(&fixture->executor))
        cflow_executor_destroy(&fixture->executor);
    cflow_machine_destroy(&fixture->machine);
}

static void actor_sender(void *user) {
    actor_sender_context *context = (actor_sender_context *)user;
    int index;
    if (context == NULL) return;
    if (context->go != NULL && !wait_until_true(context->go)) {
        atomic_fetch_add(&context->unexpected, 1);
        atomic_store(&context->completed, true);
        return;
    }
    for (index = 0; index < context->count; ++index) {
        const int payload = context->first_payload + index;
        const cflow_event_view event = {
            context->event_id, &cmeta_type_int, &payload};
        const cflow_actor_send_status status =
            cflow_actor_ref_try_send(context->ref, &event);
        if (context->attempted != NULL)
            atomic_fetch_add(context->attempted, 1);
        switch (status) {
            case CFLOW_ACTOR_SEND_ACCEPTED:
                atomic_fetch_add(&context->accepted, 1);
                break;
            case CFLOW_ACTOR_SEND_FULL:
                atomic_fetch_add(&context->full, 1);
                break;
            case CFLOW_ACTOR_SEND_STOPPING:
                atomic_fetch_add(&context->stopping, 1);
                break;
            case CFLOW_ACTOR_SEND_STOPPED:
                atomic_fetch_add(&context->stopped, 1);
                break;
            case CFLOW_ACTOR_SEND_FAILED:
                atomic_fetch_add(&context->failed, 1);
                break;
            case CFLOW_ACTOR_SEND_STALE:
                atomic_fetch_add(&context->stale, 1);
                break;
            default:
                atomic_fetch_add(&context->unexpected, 1);
                break;
        }
    }
    atomic_store(&context->completed, true);
}

static void actor_wait_and_snapshot(void *user) {
    actor_wait_stats_context *context = (actor_wait_stats_context *)user;
    if (context == NULL) return;
    atomic_store(&context->started, true);
    context->state = cflow_actor_wait(context->actor);
    context->stats_valid = cflow_actor_get_stats(
        context->actor, &context->stats);
    atomic_store(&context->completed, true);
}

static void actor_destroy_owner(void *user) {
    actor_destroy_context *context = (actor_destroy_context *)user;
    if (context == NULL) return;
    if (context->started != NULL) atomic_store(context->started, true);
    cflow_actor_destroy(context->actor);
    atomic_store(context->returned, true);
}

static bool actor_sender_wait_and_join(actor_sender_context *context,
                                       cmeta_thread_t *thread) {
    const bool completed = wait_until_true(&context->completed);
    check_true(completed);
    if (!completed) abort();
    return cmeta_thread_join(thread) == 0;
}

static bool actor_action(void *user,
                         const void *state,
                         const void *event,
                         void *out_target_state,
                         void *out_observation,
                         const char **out_error) {
    actor_probe *probe = (actor_probe *)user;
    if (probe == NULL || state == NULL || event == NULL ||
        out_target_state == NULL || out_observation == NULL ||
        out_error == NULL)
        return false;
    *(long *)out_target_state = (long)*(const int *)state +
                                (*(const bool *)event ? 1L : 0L);
    *(long *)out_observation = 42L;
    *out_error = NULL;
    atomic_fetch_add(&probe->action_calls, 1);
    return true;
}

static bool actor_on_value(void *user,
                           const cmeta_type_desc *type,
                           const void *value) {
    actor_probe *probe = (actor_probe *)user;
    if (probe == NULL || !cmeta_type_equal(type, &cmeta_type_long) ||
        value == NULL)
        return false;
    atomic_store(&probe->last_value, *(const long *)value);
    atomic_fetch_add(&probe->values, 1);
    return true;
}

static void actor_on_error(void *user, const char *message) {
    actor_probe *probe = (actor_probe *)user;
    if (probe != NULL && message != NULL)
        atomic_fetch_add(&probe->errors, 1);
}

static void actor_on_done(void *user) {
    actor_probe *probe = (actor_probe *)user;
    if (probe != NULL) atomic_fetch_add(&probe->dones, 1);
}

static cflow_machine_definition actor_definition(
    cflow_machine_state *states,
    cflow_event_type *events,
    cflow_machine_action *actions,
    cflow_machine_transition *transitions,
    cflow_machine_state_kind target_kind) {
    states[0] = (cflow_machine_state){
        10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    states[1] = (cflow_machine_state){
        20u, &cmeta_type_long, target_kind};
    events[0] = (cflow_event_type){100u, &cmeta_type_bool};
    actions[0] = (cflow_machine_action){
        300u, &cmeta_type_int, 100u, &cmeta_type_bool,
        &cmeta_type_long, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
        CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_long, 0u};
    transitions[0] = (cflow_machine_transition){
        10u, 100u, 0u, 300u, 20u, 1u};
    return (cflow_machine_definition){
        states, 2u, 10u,
        events, 1u,
        NULL, 0u,
        actions, 1u,
        transitions, 1u};
}

static bool actor_fixture_init_with_target_kind(
    actor_fixture *fixture, cflow_machine_state_kind target_kind) {
    cflow_machine_state states[2];
    cflow_event_type events[1];
    cflow_machine_action actions[1];
    cflow_machine_transition transitions[1];
    const cflow_machine_definition definition = actor_definition(
        states, events, actions, transitions, target_kind);
    cflow_actor_config config = {0};

    *fixture = (actor_fixture){0};
    fixture->initial_state = 7;
    fixture->action_binding = (cflow_machine_action_binding){
        300u, actor_action, &fixture->probe};
    if (cflow_machine_build(&fixture->machine, &definition) !=
        CFLOW_MACHINE_OK)
        return false;
    if (!cflow_executor_serial_init(&fixture->executor)) return false;
    if (!cflow_scheduler_worker_init(&fixture->scheduler, 1u)) return false;

    config.machine = (cflow_machine_instance_config){
        &fixture->machine,
        &fixture->initial_state,
        &cmeta_type_long,
        NULL,
        0u,
        &fixture->action_binding,
        1u,
        1u,
        &fixture->executor};
    config.scheduler = &fixture->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        actor_on_value, actor_on_error, actor_on_done, &fixture->probe};
    return cflow_actor_init(&fixture->actor, &config).status == CFLOW_ACTOR_OK;
}

static bool actor_fixture_init(actor_fixture *fixture) {
    return actor_fixture_init_with_target_kind(
        fixture, CFLOW_MACHINE_STATE_ACTIVE);
}

static void actor_fixture_destroy(actor_fixture *fixture) {
    cflow_actor_destroy(&fixture->actor);
    if (cflow_scheduler_valid(&fixture->scheduler))
        cflow_scheduler_destroy(&fixture->scheduler);
    if (cflow_executor_valid(&fixture->executor))
        cflow_executor_destroy(&fixture->executor);
    cflow_machine_destroy(&fixture->machine);
}

suite("CFlow Actor lifecycle") {
    it("runs Machine transitions on the CNet-compatible host owner without a new worker") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int first = 13;
        const int second = 14;
        const cflow_event_view ev1 = {100u, &cmeta_type_int, &first};
        const cflow_event_view ev2 = {101u, &cmeta_type_int, &second};
        const uint64_t deadline = cmeta_monotonic_ms() + ACTOR_TEST_TIMEOUT_MS;

        check_true(actor_edge_fixture_init_owner(&fixture, 4u));
        fixture.probe.expected_owner_thread = cmeta_thread_current_token();
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &ev1),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_ref_try_send(&ref, &ev2),
                    CFLOW_ACTOR_SEND_ACCEPTED);

        while (atomic_load(&fixture.probe.values) < 2 &&
               cmeta_monotonic_ms() < deadline) {
            /* Host drives only a bounded quantum, then returns to I/O. */
            if (!cflow_executor_run_one(&fixture.executor))
                cmeta_sleep_ms(1u);
        }
        check_equal(atomic_load(&fixture.probe.values), 2);
        check_equal(atomic_load(&fixture.probe.action_calls), 2);
        check_false(atomic_load(&fixture.probe.wrong_owner_thread));
        check_equal(fixture.probe.observations[0], first);
        check_equal(fixture.probe.observations[1], second);

        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        while ((cflow_actor_current_state(&fixture.actor) !=
                    CFLOW_ACTOR_STATE_STOPPED ||
                cflow_executor_pending(&fixture.executor) != 0u) &&
               cmeta_monotonic_ms() < deadline) {
            if (!cflow_executor_run_one(&fixture.executor))
                cmeta_sleep_ms(1u);
        }
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        check_equal(cflow_executor_pending(&fixture.executor), (size_t)0u);
        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("uses one owner lane for Machine transitions and Subscription callbacks") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int first = 31;
        const int second = 32;
        const cflow_event_view ev1 = {100u, &cmeta_type_int, &first};
        const cflow_event_view ev2 = {101u, &cmeta_type_int, &second};
        const uint64_t deadline = cmeta_monotonic_ms() + ACTOR_TEST_TIMEOUT_MS;

        check_true(actor_edge_fixture_init_full_owner(&fixture, 4u));
        fixture.probe.expected_owner_thread = cmeta_thread_current_token();
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &ev1),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_ref_try_send(&ref, &ev2),
                    CFLOW_ACTOR_SEND_ACCEPTED);

        while (atomic_load(&fixture.probe.values) < 2 &&
               cmeta_monotonic_ms() < deadline) {
            if (!cflow_executor_run_one(&fixture.executor))
                cmeta_sleep_ms(1u);
        }
        check_equal(atomic_load(&fixture.probe.values), 2);
        check_equal(atomic_load(&fixture.probe.action_calls), 2);
        check_false(atomic_load(&fixture.probe.wrong_owner_thread));
        check_false(atomic_load(&fixture.probe.wrong_sink_owner_thread));
        check_equal(fixture.probe.observations[0], first);
        check_equal(fixture.probe.observations[1], second);

        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        while (cflow_actor_current_state(&fixture.actor) !=
                    CFLOW_ACTOR_STATE_STOPPED &&
               cmeta_monotonic_ms() < deadline) {
            if (!cflow_executor_run_one(&fixture.executor))
                cmeta_sleep_ms(1u);
        }
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("rejects invalid owners and preserves Machine initialization status") {
        cflow_actor actor = {0};
        cflow_actor_config config = {0};
        cflow_actor_init_result result = cflow_actor_init(NULL, &config);

        check_equal(result.status, CFLOW_ACTOR_INVALID_ARGUMENT);
        check_equal(result.machine_status, CFLOW_MACHINE_INSTANCE_OK);
        result = cflow_actor_init(&actor, NULL);
        check_equal(result.status, CFLOW_ACTOR_INVALID_ARGUMENT);
        check_equal(result.machine_status, CFLOW_MACHINE_INSTANCE_OK);

        cflow_scheduler scheduler = {0};
        check_true(cflow_scheduler_worker_init(&scheduler, 1u));
        config.scheduler = &scheduler;
        result = cflow_actor_init(&actor, &config);
        check_equal(result.status, CFLOW_ACTOR_MACHINE_REJECTED);
        check_equal(result.machine_status,
                    CFLOW_MACHINE_INSTANCE_INVALID_ARGUMENT);
        check_null(actor.impl);
        cflow_scheduler_destroy(&scheduler);
    }

    it("rejects a valid manual scheduler without publishing an Actor") {
        actor_fixture fixture = {0};
        cflow_scheduler manual = {0};
        cflow_actor_config config = {0};
        cflow_actor_init_result result;

        check_true(actor_fixture_init(&fixture));
        cflow_actor_destroy(&fixture.actor);
        check_true(cflow_scheduler_test_init(&manual));
        config.machine = (cflow_machine_instance_config){
            &fixture.machine,
            &fixture.initial_state,
            &cmeta_type_long,
            NULL,
            0u,
            &fixture.action_binding,
            1u,
            1u,
            &fixture.executor};
        config.scheduler = &manual;
        result = cflow_actor_init(&fixture.actor, &config);
        check_equal(result.status, CFLOW_ACTOR_INVALID_SCHEDULER);
        check_equal(result.machine_status, CFLOW_MACHINE_INSTANCE_OK);
        check_null(fixture.actor.impl);

        cflow_scheduler_destroy(&manual);
        actor_fixture_destroy(&fixture);
    }

    it("stops directly from START and reports exact pre-start and terminal sends") {
        actor_fixture fixture;
        cflow_actor_ref ref = {0};
        cflow_actor_ref retained = {0};
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload};
        cflow_actor_stats stats = {0};
        cflow_statechart_actor_stats wrong_stats;
        cflow_statechart_actor_stats wrong_snapshot;

        check_true(actor_fixture_init(&fixture));
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_START);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_true(cflow_actor_ref_retain(&ref, &retained));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_NOT_STARTED);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_equal(cflow_actor_ref_try_send(&retained, &event),
                    CFLOW_ACTOR_SEND_STOPPED);
        check_equal(cflow_actor_request_stop(&fixture.actor),
                    CFLOW_ACTOR_STOPPED);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_STOPPED);
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.rejected_not_started, (uint64_t)1u);
        check_equal(stats.rejected_stopped, (uint64_t)1u);
        memset(&wrong_stats, 0x5a, sizeof(wrong_stats));
        wrong_snapshot = wrong_stats;
        check_false(cflow_statechart_actor_get_stats(
            &fixture.actor, &wrong_stats));
        check_equal(memcmp(&wrong_stats, &wrong_snapshot,
                           sizeof(wrong_stats)), 0);

        cflow_actor_ref_release(&retained);
        cflow_actor_ref_release(&ref);
        actor_fixture_destroy(&fixture);
    }

    it("maps type mismatch and full while one accepted Event transitions once") {
        actor_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        const bool payload = true;
        const int wrong_payload = 1;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload};
        const cflow_event_view wrong_type = {
            100u, &cmeta_type_int, &wrong_payload};
        cflow_actor_stats stats = {0};

        check_true(actor_fixture_init(&fixture));
        check_not_equal(cflow_scheduler_post(
                            &fixture.scheduler, block_scheduler, &blocker),
                        (cflow_task_id)0u);
        check_true(wait_until_true(&blocker.entered));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_RUNNING);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &wrong_type),
                    CFLOW_ACTOR_SEND_TYPE_MISMATCH);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_FULL);

        atomic_store(&blocker.release, true);
        check_true(wait_until_at_least(&fixture.probe.values, 1));
        check_equal(atomic_load(&fixture.probe.action_calls), 1);
        check_equal(atomic_load(&fixture.probe.values), 1);
        check_equal(atomic_load(&fixture.probe.last_value), 42L);
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)1u);
        check_equal(stats.machine.completed, (uint64_t)1u);
        check_equal(stats.machine.current_state,
                    (cflow_machine_state_id)20u);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_equal(atomic_load(&fixture.probe.errors), 0);
        check_equal(atomic_load(&fixture.probe.dones), 1);

        cflow_actor_ref_release(&ref);
        actor_fixture_destroy(&fixture);
    }

    it("fails when Run reports done while Actor is RUNNING") {
        actor_fixture fixture;
        cflow_actor_ref ref = {0};
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload};
        const char *first_error;

        check_true(actor_fixture_init_with_target_kind(
            &fixture, CFLOW_MACHINE_STATE_DONE));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_FAILED);
        first_error = cflow_actor_error(&fixture.actor);
        check_not_null(first_error);
        check_contains(first_error, "RUNNING");
        check_true(wait_until_at_least(&fixture.probe.errors, 1));
        check_equal(atomic_load(&fixture.probe.errors), 1);
        check_equal(atomic_load(&fixture.probe.dones), 0);
        check_equal(cflow_actor_request_stop(&fixture.actor),
                    CFLOW_ACTOR_FAILED);
        check_true(cflow_actor_error(&fixture.actor) == first_error);

        cflow_actor_ref_release(&ref);
        actor_fixture_destroy(&fixture);
    }

    it("returns STALE before validating an Event after owner destruction") {
        actor_fixture fixture;
        cflow_actor_ref ref = {0};
        const cflow_event_view malformed = {0};

        check_true(actor_fixture_init(&fixture));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        cflow_actor_destroy(&fixture.actor);
        check_equal(cflow_actor_ref_try_send(&ref, NULL),
                    CFLOW_ACTOR_SEND_STALE);
        check_equal(cflow_actor_ref_try_send(&ref, &malformed),
                    CFLOW_ACTOR_SEND_STALE);

        cflow_actor_ref_release(&ref);
        actor_fixture_destroy(&fixture);
    }

    it("keeps STOPPING observable and reports repeated lifecycle calls exactly") {
        actor_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload};

        check_true(actor_fixture_init(&fixture));
        check_not_equal(cflow_scheduler_post(
                            &fixture.scheduler, block_scheduler, &blocker),
                        (cflow_task_id)0u);
        check_true(wait_until_true(&blocker.entered));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_start(&fixture.actor),
                    CFLOW_ACTOR_ALREADY_STARTED);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPING);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_STOPPING);
        check_equal(cflow_actor_request_stop(&fixture.actor),
                    CFLOW_ACTOR_STOPPING);

        atomic_store(&blocker.release, true);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        check_equal(cflow_actor_request_stop(&fixture.actor),
                    CFLOW_ACTOR_STOPPED);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_STOPPED);

        cflow_actor_ref_release(&ref);
        actor_fixture_destroy(&fixture);
    }

    it("bounds saturation while one transition is blocked") {
        actor_edge_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        const int payloads[] = {1, 2, 3};
        const cflow_event_view events[] = {
            {100u, &cmeta_type_int, &payloads[0]},
            {100u, &cmeta_type_int, &payloads[1]},
            {100u, &cmeta_type_int, &payloads[2]}
        };
        cflow_actor_stats stats = {0};

        check_true(actor_edge_fixture_init(&fixture, 1u));
        fixture.probe.blocker = &blocker;
        atomic_store(&fixture.probe.block_action, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &events[0]),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_true(&blocker.entered));
        check_equal(cflow_actor_ref_try_send(&ref, &events[1]),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_ref_try_send(&ref, &events[2]),
                    CFLOW_ACTOR_SEND_FULL);

        atomic_store(&blocker.release, true);
        check_true(wait_until_at_least(&fixture.probe.values, 2));
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)2u);
        check_equal(stats.machine.completed, (uint64_t)2u);
        check_equal(stats.machine.cancelled_events, (uint64_t)0u);
        check_equal(stats.machine.pending, (size_t)0u);
        check_equal(stats.machine.in_flight, (size_t)0u);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("accounts for unique payload sequences from concurrent retained refs") {
        enum {
            PRODUCERS = 4,
            EVENTS_PER_PRODUCER = 8,
            TOTAL_EVENTS = PRODUCERS * EVENTS_PER_PRODUCER
        };
        actor_edge_fixture fixture;
        cflow_actor_ref refs[PRODUCERS] = {0};
        actor_sender_context contexts[PRODUCERS] = {0};
        cmeta_thread_t threads[PRODUCERS] = {0};
        atomic_bool go = false;
        cflow_actor_stats stats = {0};
        int created = 0;
        int producer;
        int payload;

        check_true(actor_edge_fixture_init(&fixture, TOTAL_EVENTS));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &refs[0]));
        for (producer = 1; producer < PRODUCERS; ++producer)
            check_true(cflow_actor_ref_retain(&refs[0], &refs[producer]));
        for (producer = 0; producer < PRODUCERS; ++producer) {
            contexts[producer].ref = &refs[producer];
            contexts[producer].event_id =
                (cflow_event_id)(100u + (unsigned)producer);
            contexts[producer].first_payload =
                producer * EVENTS_PER_PRODUCER;
            contexts[producer].count = EVENTS_PER_PRODUCER;
            contexts[producer].go = &go;
            {
                const int create_status = cmeta_thread_create(
                    &threads[producer], actor_sender, &contexts[producer]);
                check_equal(create_status, 0);
                if (create_status != 0) break;
                ++created;
            }
        }
        atomic_store(&go, true);
        check_equal(created, PRODUCERS);
        for (producer = 0; producer < created; ++producer)
            check_true(actor_sender_wait_and_join(
                &contexts[producer], &threads[producer]));
        check_true(wait_until_at_least(
            &fixture.probe.values, TOTAL_EVENTS));
        for (producer = 0; producer < PRODUCERS; ++producer) {
            check_equal(atomic_load(&contexts[producer].accepted),
                        EVENTS_PER_PRODUCER);
            check_equal(atomic_load(&contexts[producer].unexpected), 0);
        }
        for (payload = 0; payload < TOTAL_EVENTS; ++payload) {
            const int expected_event_index =
                payload / EVENTS_PER_PRODUCER;
            int event_index;
            check_equal(atomic_load(&fixture.probe.seen[payload]), 1);
            for (event_index = 0;
                 event_index < ACTOR_EDGE_EVENT_TYPES;
                 ++event_index) {
                check_equal(
                    atomic_load(
                        &fixture.probe.pair_seen[event_index][payload]),
                    event_index == expected_event_index ? 1 : 0);
            }
        }
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)TOTAL_EVENTS);
        check_equal(stats.machine.completed, (uint64_t)TOTAL_EVENTS);
        check_equal(stats.machine.cancelled_events, (uint64_t)0u);
        check_equal(atomic_load(&fixture.probe.action_calls), TOTAL_EVENTS);
        check_equal(atomic_load(&fixture.probe.dones), 1);

        for (producer = 0; producer < PRODUCERS; ++producer)
            cflow_actor_ref_release(&refs[producer]);
        actor_edge_fixture_destroy(&fixture);
    }

    it("admits a self-send from a Machine action in FIFO order") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int payload = 1;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};
        cflow_actor_stats stats = {0};

        check_true(actor_edge_fixture_init(&fixture, 1u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        fixture.probe.self_ref = &ref;
        atomic_store(&fixture.probe.self_send, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_at_least(&fixture.probe.values, 2));
        check_equal(atomic_load(&fixture.probe.self_send_status),
                    (int)CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(fixture.probe.observations[0], 1);
        check_equal(fixture.probe.observations[1], 2);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)2u);
        check_equal(stats.machine.completed, (uint64_t)2u);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("permits request_stop from an Actor sink callback") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int payload = 9;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};
        cflow_actor_stats stats = {0};

        check_true(actor_edge_fixture_init(&fixture, 2u));
        atomic_store(&fixture.probe.stop_on_value, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_true(atomic_load(&fixture.probe.stop_requested));
        check_equal(atomic_load(&fixture.probe.stop_status),
                    (int)CFLOW_ACTOR_OK);
        check_equal(atomic_load(&fixture.probe.values), 1);
        check_equal(atomic_load(&fixture.probe.errors), 0);
        check_equal(atomic_load(&fixture.probe.dones), 1);
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)1u);
        check_equal(stats.machine.completed, (uint64_t)1u);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("stops with queued Events and commits exactly one in-flight transition") {
        actor_edge_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        const int payloads[] = {11, 12, 13};
        const cflow_event_view events[] = {
            {100u, &cmeta_type_int, &payloads[0]},
            {100u, &cmeta_type_int, &payloads[1]},
            {100u, &cmeta_type_int, &payloads[2]}
        };
        cflow_actor_stats stats = {0};
        int index;

        check_true(actor_edge_fixture_init(&fixture, 3u));
        fixture.probe.blocker = &blocker;
        atomic_store(&fixture.probe.block_action, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &events[0]),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_true(&blocker.entered));
        for (index = 1; index < 3; ++index)
            check_equal(cflow_actor_ref_try_send(&ref, &events[index]),
                        CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.state, CFLOW_ACTOR_STATE_STOPPING);
        check_equal(stats.machine.accepted, (uint64_t)3u);
        check_equal(stats.machine.cancelled_events, (uint64_t)2u);
        check_equal(stats.machine.in_flight, (size_t)1u);
        check_equal(stats.machine.pending, (size_t)0u);

        atomic_store(&blocker.release, true);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_until_at_least(&fixture.probe.dones, 1));
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)3u);
        check_equal(stats.machine.completed, (uint64_t)1u);
        check_equal(stats.machine.cancelled_events, (uint64_t)2u);
        check_equal(stats.machine.in_flight, (size_t)0u);
        check_equal(stats.machine.accepted,
                    stats.machine.completed + stats.machine.cancelled_events);
        check_equal(atomic_load(&fixture.probe.values), 1);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("fails an unhandled Event and preserves the first owned error") {
        actor_fixture fixture;
        cflow_actor_ref ref = {0};
        const bool payload = true;
        const cflow_event_view event = {
            100u, &cmeta_type_bool, &payload};
        cflow_actor_stats stats = {0};
        const char *first_error;

        check_true(actor_fixture_init(&fixture));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_at_least(&fixture.probe.values, 1));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_FAILED));
        check_true(wait_until_at_least(&fixture.probe.errors, 1));
        first_error = cflow_actor_error(&fixture.actor);
        check_not_null(first_error);
        check_contains(first_error, "no enabled transition");
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_FAILED);
        check_equal(cflow_actor_request_stop(&fixture.actor),
                    CFLOW_ACTOR_FAILED);
        check_true(cflow_actor_error(&fixture.actor) == first_error);
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)2u);
        check_equal(stats.machine.completed, (uint64_t)1u);
        check_equal(stats.machine.failed, (uint64_t)1u);
        check_equal(stats.machine.cancelled_events, (uint64_t)0u);
        check_equal(stats.rejected_failed, (uint64_t)1u);
        check_equal(atomic_load(&fixture.probe.errors), 1);

        cflow_actor_ref_release(&ref);
        actor_fixture_destroy(&fixture);
    }

    it("fails on a guard error without committing the Event") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int payload = 21;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};
        cflow_actor_stats stats = {0};

        check_true(actor_edge_fixture_init(&fixture, 2u));
        atomic_store(&fixture.probe.fail_guard, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_FAILED));
        check_true(wait_until_at_least(&fixture.probe.errors, 1));
        check_contains(cflow_actor_error(&fixture.actor),
                       "actor edge guard failure");
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)1u);
        check_equal(stats.machine.completed, (uint64_t)0u);
        check_equal(stats.machine.failed, (uint64_t)1u);
        check_equal(stats.machine.cancelled_events, (uint64_t)0u);
        check_equal(atomic_load(&fixture.probe.action_calls), 0);
        check_equal(atomic_load(&fixture.probe.errors), 1);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("fails on an action error without committing the Event") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int payload = 22;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};
        cflow_actor_stats stats = {0};

        check_true(actor_edge_fixture_init(&fixture, 2u));
        atomic_store(&fixture.probe.fail_action, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_FAILED));
        check_true(wait_until_at_least(&fixture.probe.errors, 1));
        check_contains(cflow_actor_error(&fixture.actor),
                       "actor edge action failure");
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)1u);
        check_equal(stats.machine.completed, (uint64_t)0u);
        check_equal(stats.machine.failed, (uint64_t)1u);
        check_equal(stats.machine.cancelled_events, (uint64_t)0u);
        check_equal(atomic_load(&fixture.probe.action_calls), 1);
        check_equal(atomic_load(&fixture.probe.errors), 1);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("fails when the sink rejects a committed value") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        const int payload = 23;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};
        cflow_actor_stats stats = {0};
        const char *first_error;

        check_true(actor_edge_fixture_init(&fixture, 2u));
        atomic_store(&fixture.probe.reject_value, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_FAILED));
        check_true(wait_until_at_least(&fixture.probe.errors, 1));
        first_error = cflow_actor_error(&fixture.actor);
        check_not_null(first_error);
        check_contains(first_error, "observer rejected value");
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)1u);
        check_equal(stats.machine.completed, (uint64_t)1u);
        check_equal(stats.machine.failed, (uint64_t)0u);
        check_equal(stats.machine.cancelled_events, (uint64_t)0u);
        check_equal(atomic_load(&fixture.probe.values), 1);
        check_equal(atomic_load(&fixture.probe.errors), 1);
        check_equal(atomic_load(&fixture.probe.dones), 0);
        check_equal(cflow_actor_request_stop(&fixture.actor),
                    CFLOW_ACTOR_FAILED);
        check_true(cflow_actor_error(&fixture.actor) == first_error);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("settles queued sink failure accounting before wait returns") {
        enum { WAITERS = 8 };
        actor_edge_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        actor_wait_stats_context waiters[WAITERS] = {0};
        cmeta_thread_t threads[WAITERS] = {0};
        int payloads[] = {41, 42, 43};
        cflow_event_view events[] = {
            {100u, &cmeta_type_int, &payloads[0]},
            {101u, &cmeta_type_int, &payloads[1]},
            {102u, &cmeta_type_int, &payloads[2]}
        };
        int created = 0;
        int index;

        check_true(actor_edge_fixture_init(&fixture, 4u));
        fixture.probe.blocker = &blocker;
        atomic_store(&fixture.probe.block_action, true);
        atomic_store(&fixture.probe.reject_value, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &events[0]),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_true(&blocker.entered));
        check_equal(cflow_actor_ref_try_send(&ref, &events[1]),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_ref_try_send(&ref, &events[2]),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        for (index = 0; index < WAITERS; ++index) {
            waiters[index].actor = &fixture.actor;
            if (cmeta_thread_create(
                    &threads[index], actor_wait_and_snapshot,
                    &waiters[index]) != 0)
                break;
            ++created;
        }
        check_equal(created, WAITERS);
        for (index = 0; index < created; ++index)
            check_true(wait_until_true(&waiters[index].started));

        atomic_store(&blocker.release, true);
        for (index = 0; index < created; ++index) {
            const bool completed = wait_until_true(&waiters[index].completed);
            check_true(completed);
            if (!completed) abort();
            check_equal(cmeta_thread_join(&threads[index]), 0);
            check_equal(waiters[index].state, CFLOW_ACTOR_STATE_FAILED);
            check_true(waiters[index].stats_valid);
            check_equal(waiters[index].stats.machine.accepted, (uint64_t)3u);
            check_equal(waiters[index].stats.machine.completed, (uint64_t)1u);
            check_equal(waiters[index].stats.machine.cancelled_events,
                        (uint64_t)2u);
            check_equal(waiters[index].stats.machine.pending, (size_t)0u);
            check_equal(waiters[index].stats.machine.in_flight, (size_t)0u);
            check_equal(
                waiters[index].stats.machine.accepted,
                waiters[index].stats.machine.completed +
                    waiters[index].stats.machine.cancelled_events);
        }

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("fails an idle Actor when its Mailbox wake reaches a full Scheduler") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        cflow_scheduler_stats scheduler_stats = {0};
        cflow_actor_stats actor_stats = {0};
        const int payload = 51;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};
        cflow_schedule_result occupied;

        check_true(actor_edge_fixture_init_with_scheduler_capacity(
            &fixture, 2u, 1u, 1u));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_scheduler_wait_idle(&fixture.scheduler));
        occupied = cflow_scheduler_try_post_after(
            &fixture.scheduler, UINT64_C(1000),
            actor_scheduler_slot, NULL);
        check_equal(occupied.status, CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));

        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_at_least(&fixture.probe.action_calls, 1));
        check_true(wait_actor_state(
            &fixture.actor, CFLOW_ACTOR_STATE_FAILED));
        check_true(wait_until_at_least(&fixture.probe.errors, 1));
        check_true(cflow_actor_get_stats(&fixture.actor, &actor_stats));
        check_equal(actor_stats.machine.accepted, (uint64_t)1u);
        check_equal(actor_stats.machine.accepted,
                    actor_stats.machine.completed +
                        actor_stats.machine.cancelled_events);
        check_true(cflow_scheduler_get_stats(
            &fixture.scheduler, &scheduler_stats));
        check_greater(scheduler_stats.rejected_full, (size_t)0u);
        check_contains(cflow_actor_error(&fixture.actor), "scheduler is full");

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("returns from send when Executor rejection also meets a full Scheduler") {
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        actor_sender_context sender = {0};
        cmeta_thread_t thread = {0};
        cflow_schedule_result occupied;
        check_true(actor_edge_fixture_init_with_scheduler_capacity(
            &fixture, 2u, 1u, 1u));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_scheduler_wait_idle(&fixture.scheduler));
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_true(cflow_executor_shutdown(&fixture.executor));
        occupied = cflow_scheduler_try_post_after(
            &fixture.scheduler, UINT64_C(60000), actor_scheduler_slot, NULL);
        check_equal(occupied.status, CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        sender.ref = &ref;
        sender.event_id = 100u;
        sender.first_payload = 9;
        sender.count = 1;
        check_equal(cmeta_thread_create(&thread, actor_sender, &sender), 0);
        /* A deadlock must fail the test process instead of hanging teardown. */
        if (!wait_until_true(&sender.completed)) abort();
        check_equal(cmeta_thread_join(&thread), 0);
        check_equal(atomic_load(&sender.accepted), 1);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_FAILED);
        check_equal(atomic_load(&fixture.probe.errors), 1);
        check_contains(cflow_actor_error(&fixture.actor), "scheduler is full");
        check_true(cflow_scheduler_cancel(&fixture.scheduler, occupied.task_id));
        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("keeps retained refs stale until each shell reference is released") {
        actor_edge_fixture fixture;
        cflow_actor_ref first = {0};
        cflow_actor_ref second = {0};
        cflow_actor_ref third = {0};
        const int payload = 31;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};

        check_true(actor_edge_fixture_init(&fixture, 2u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &first));
        check_true(cflow_actor_ref_retain(&first, &second));
        cflow_actor_destroy(&fixture.actor);
        check_null(fixture.actor.impl);
        check_true(cflow_actor_ref_retain(&second, &third));
        check_equal(cflow_actor_ref_try_send(&first, &event),
                    CFLOW_ACTOR_SEND_STALE);
        check_equal(cflow_actor_ref_try_send(&second, &event),
                    CFLOW_ACTOR_SEND_STALE);
        cflow_actor_ref_release(&first);
        check_null(first.impl);
        check_equal(cflow_actor_ref_try_send(&third, &event),
                    CFLOW_ACTOR_SEND_STALE);
        cflow_actor_ref_release(&second);
        check_null(second.impl);
        cflow_actor_ref_release(&third);
        check_null(third.impl);
        cflow_actor_ref_release(&third);

        actor_edge_fixture_destroy(&fixture);
    }

    it("keeps an in-flight retained-ref send safe across owner destruction") {
        enum { DESTROY_OVERLAP_MAX_SENDS = 4096, DESTROY_POST_SENDS = 16 };
        actor_edge_fixture fixture;
        cflow_actor_ref ref = {0};
        actor_blocker blocker = {0};
        const int payload = 37;
        const cflow_event_view event = {100u, &cmeta_type_int, &payload};
        cmeta_thread_t destroy_thread = {0};
        atomic_bool destroy_started = false;
        atomic_bool destroy_returned = false;
        actor_destroy_context destroy_context = {
            &fixture.actor, &destroy_returned, &destroy_started};
        int overlap_attempts = 0;
        int index;

        check_true(actor_edge_fixture_init(&fixture, 8u));
        fixture.probe.blocker = &blocker;
        atomic_store(&fixture.probe.block_action, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        {
            const bool entered = wait_until_true(&blocker.entered);
            check_true(entered);
            if (!entered) abort();
        }
        {
            const int create_status = cmeta_thread_create(
                &destroy_thread, actor_destroy_owner, &destroy_context);
            check_equal(create_status, 0);
            if (create_status != 0) abort();
        }
        {
            const bool started = wait_until_true(&destroy_started);
            check_true(started);
            if (!started) abort();
        }

        for (index = 0; index < DESTROY_OVERLAP_MAX_SENDS; ++index) {
            ++overlap_attempts;
            if (cflow_actor_ref_try_send(&ref, &event) ==
                CFLOW_ACTOR_SEND_STALE)
                break;
            cmeta_sleep_ms(1u);
        }
        check_less(index, DESTROY_OVERLAP_MAX_SENDS);
        check_greater(overlap_attempts, 0);
        check_false(atomic_load(&destroy_returned));
        for (index = 0; index < DESTROY_POST_SENDS; ++index)
            check_equal(cflow_actor_ref_try_send(&ref, &event),
                        CFLOW_ACTOR_SEND_STALE);

        atomic_store(&blocker.release, true);
        {
            const bool returned = wait_until_true(&destroy_returned);
            check_true(returned);
            if (!returned) abort();
        }
        check_equal(cmeta_thread_join(&destroy_thread), 0);
        check_null(fixture.actor.impl);
        for (index = 0; index < DESTROY_POST_SENDS; ++index)
            check_equal(cflow_actor_ref_try_send(&ref, &event),
                        CFLOW_ACTOR_SEND_STALE);

        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("replays the same Event sequence deterministically across two Actors") {
        enum { REPLAY_COUNT = 6 };
        const int sequence[REPLAY_COUNT] = {4, 1, 7, 3, 9, 2};
        actor_edge_fixture first;
        actor_edge_fixture second;
        cflow_actor_ref first_ref = {0};
        cflow_actor_ref second_ref = {0};
        cflow_actor_stats first_stats = {0};
        cflow_actor_stats second_stats = {0};
        int index;

        check_true(actor_edge_fixture_init(&first, REPLAY_COUNT));
        check_true(actor_edge_fixture_init(&second, REPLAY_COUNT));
        check_equal(cflow_actor_start(&first.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_start(&second.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&first.actor, &first_ref));
        check_true(cflow_actor_ref_acquire(&second.actor, &second_ref));
        for (index = 0; index < REPLAY_COUNT; ++index) {
            const cflow_event_view first_event = {
                (cflow_event_id)(100u + (unsigned)(index % 2)),
                &cmeta_type_int,
                &sequence[index]};
            const cflow_event_view second_event = first_event;
            check_equal(cflow_actor_ref_try_send(&first_ref, &first_event),
                        CFLOW_ACTOR_SEND_ACCEPTED);
            check_equal(cflow_actor_ref_try_send(&second_ref, &second_event),
                        CFLOW_ACTOR_SEND_ACCEPTED);
        }
        check_true(wait_until_at_least(&first.probe.values, REPLAY_COUNT));
        check_true(wait_until_at_least(&second.probe.values, REPLAY_COUNT));
        for (index = 0; index < REPLAY_COUNT; ++index) {
            check_equal(first.probe.observations[index], sequence[index]);
            check_equal(second.probe.observations[index], sequence[index]);
        }
        check_equal(cflow_actor_request_stop(&first.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_request_stop(&second.actor), CFLOW_ACTOR_OK);
        check_true(wait_actor_state(&first.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_actor_state(&second.actor, CFLOW_ACTOR_STATE_STOPPED));
        check_true(wait_until_at_least(&first.probe.dones, 1));
        check_true(wait_until_at_least(&second.probe.dones, 1));
        check_true(cflow_actor_get_stats(&first.actor, &first_stats));
        check_true(cflow_actor_get_stats(&second.actor, &second_stats));
        check_equal(first_stats.machine.accepted, (uint64_t)REPLAY_COUNT);
        check_equal(second_stats.machine.accepted, (uint64_t)REPLAY_COUNT);
        check_equal(first_stats.machine.completed, (uint64_t)REPLAY_COUNT);
        check_equal(second_stats.machine.completed, (uint64_t)REPLAY_COUNT);
        check_equal(first_stats.machine.current_state,
                    second_stats.machine.current_state);

        cflow_actor_ref_release(&first_ref);
        cflow_actor_ref_release(&second_ref);
        actor_edge_fixture_destroy(&first);
        actor_edge_fixture_destroy(&second);
    }

    it("classifies a bounded retained-ref send race against stop") {
        enum {
            STRESS_ITERATIONS = 20,
            STRESS_PRODUCERS = 4,
            STRESS_SENDS_PER_PRODUCER = 32,
            STRESS_SENDS = STRESS_PRODUCERS * STRESS_SENDS_PER_PRODUCER
        };
        int iteration;

        for (iteration = 0; iteration < STRESS_ITERATIONS; ++iteration) {
            actor_edge_fixture fixture;
            actor_blocker blocker = {0};
            cflow_actor_ref refs[STRESS_PRODUCERS] = {0};
            actor_sender_context contexts[STRESS_PRODUCERS] = {0};
            cmeta_thread_t threads[STRESS_PRODUCERS] = {0};
            atomic_bool go = false;
            atomic_int attempted = 0;
            cflow_actor_stats stats = {0};
            const int seed_payload = 200;
            const cflow_event_view seed = {
                100u, &cmeta_type_int, &seed_payload};
            uint64_t classified_accepted = 1u;
            uint64_t classified_stopping = 0u;
            uint64_t classified_stopped = 0u;
            int created = 0;
            int producer;

            check_true(actor_edge_fixture_init(&fixture, 8u));
            fixture.probe.blocker = &blocker;
            atomic_store(&fixture.probe.block_action, true);
            check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
            check_true(cflow_actor_ref_acquire(&fixture.actor, &refs[0]));
            for (producer = 1; producer < STRESS_PRODUCERS; ++producer)
                check_true(cflow_actor_ref_retain(
                    &refs[0], &refs[producer]));
            check_equal(cflow_actor_ref_try_send(&refs[0], &seed),
                        CFLOW_ACTOR_SEND_ACCEPTED);
            check_true(wait_until_true(&blocker.entered));
            for (producer = 0; producer < STRESS_PRODUCERS; ++producer) {
                contexts[producer].ref = &refs[producer];
                contexts[producer].event_id =
                    (cflow_event_id)(100u + (unsigned)producer);
                contexts[producer].first_payload =
                    producer * STRESS_SENDS_PER_PRODUCER;
                contexts[producer].count = STRESS_SENDS_PER_PRODUCER;
                contexts[producer].go = &go;
                contexts[producer].attempted = &attempted;
                {
                    const int create_status = cmeta_thread_create(
                        &threads[producer], actor_sender,
                        &contexts[producer]);
                    check_equal(create_status, 0);
                    if (create_status != 0) break;
                    ++created;
                }
            }
            atomic_store(&go, true);
            check_equal(created, STRESS_PRODUCERS);
            check_equal(cflow_actor_request_stop(&fixture.actor),
                        CFLOW_ACTOR_OK);
            atomic_store(&blocker.release, true);
            for (producer = 0; producer < created; ++producer) {
                check_true(actor_sender_wait_and_join(
                    &contexts[producer], &threads[producer]));
                classified_accepted += (uint64_t)atomic_load(
                    &contexts[producer].accepted);
                classified_stopping += (uint64_t)atomic_load(
                    &contexts[producer].stopping);
                classified_stopped += (uint64_t)atomic_load(
                    &contexts[producer].stopped);
                check_equal(atomic_load(&contexts[producer].failed), 0);
                check_equal(atomic_load(&contexts[producer].stale), 0);
                check_equal(atomic_load(&contexts[producer].unexpected), 0);
            }
            check_equal(atomic_load(&attempted), STRESS_SENDS);
            check_true(wait_actor_terminal(&fixture.actor));
            check_equal(cflow_actor_current_state(&fixture.actor),
                        CFLOW_ACTOR_STATE_STOPPED);
            check_true(wait_until_at_least(&fixture.probe.dones, 1));
            check_true(cflow_actor_get_stats(&fixture.actor, &stats));
            check_equal(stats.machine.accepted, classified_accepted);
            check_equal(stats.machine.accepted,
                        stats.machine.completed +
                            stats.machine.cancelled_events);
            check_equal(stats.machine.failed, (uint64_t)0u);
            check_equal(stats.machine.in_flight, (size_t)0u);
            check_equal(stats.machine.pending, (size_t)0u);
            check_equal(stats.rejected_stopping, classified_stopping);
            check_equal(stats.rejected_stopped, classified_stopped);
            check_equal(stats.rejected_failed, (uint64_t)0u);
            check_equal(classified_accepted +
                            classified_stopping +
                            classified_stopped +
                            (uint64_t)(
                                atomic_load(&contexts[0].full) +
                                atomic_load(&contexts[1].full) +
                                atomic_load(&contexts[2].full) +
                                atomic_load(&contexts[3].full)),
                        (uint64_t)STRESS_SENDS + 1u);

            for (producer = 0; producer < STRESS_PRODUCERS; ++producer)
                cflow_actor_ref_release(&refs[producer]);
            actor_edge_fixture_destroy(&fixture);
        }
    }
}

typedef struct actor_foreign_stop_context {
    cflow_actor *actor;
    atomic_bool started;
    atomic_bool finished;
    atomic_int status;
} actor_foreign_stop_context;

static void actor_foreign_stop(void *user) {
    actor_foreign_stop_context *context = (actor_foreign_stop_context *)user;
    atomic_store(&context->started, true);
    atomic_store(&context->status,
                 (int)cflow_actor_request_stop(context->actor));
    atomic_store(&context->finished, true);
}

/* #1105: foreign stop must synchronize with an executing worker pump.
 * Run under source-instrumented TSan, not merely a Release-only matrix. */
suite("CFlow worker Actor cross-thread terminal synchronization") {
    it("keeps wait blocked until a worker terminal callback is complete") {
        actor_edge_fixture fixture;
        actor_blocker terminal = {0};
        actor_wait_stats_context waiter = {0};
        cmeta_thread_t wait_thread = {0};
        cflow_actor_ref ref = {0};
        const int payload = 42;
        const cflow_event_view event = {
            100u, &cmeta_type_int, &payload};

        check_true(actor_edge_fixture_init(&fixture, 1u));
        fixture.probe.done_blocker = &terminal;
        atomic_store(&fixture.probe.block_done, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_at_least(&fixture.probe.values, 1));
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(wait_until_true(&terminal.entered));

        /* STOPPED is visible but wait must still protect borrowed callback
         * storage. The callback is deliberately held on the worker lane. */
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        waiter.actor = &fixture.actor;
        check_equal(cmeta_thread_create(
                        &wait_thread, actor_wait_and_snapshot, &waiter), 0);
        check_true(wait_until_true(&waiter.started));
        cmeta_sleep_ms(10u);
        check_false(atomic_load(&waiter.completed));

        atomic_store(&terminal.release, true);
        check_true(wait_until_true(&waiter.completed));
        check_equal(cmeta_thread_join(&wait_thread), 0);
        check_equal(waiter.state, CFLOW_ACTOR_STATE_STOPPED);
        check_true(waiter.stats_valid);
        check_equal(atomic_load(&fixture.probe.dones), 1);
        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("settles a blocked worker action and foreign stop exactly once") {
        enum { STOP_RACE_REPETITIONS = 50 };
        for (int iteration = 0; iteration < STOP_RACE_REPETITIONS; ++iteration) {
            actor_edge_fixture fixture;
            actor_blocker blocker = {0};
            cflow_actor_ref ref = {0};
            const int payload = iteration % ACTOR_EDGE_OBSERVATIONS;
            const cflow_event_view event = {
                100u, &cmeta_type_int, &payload};
            cflow_actor_stats stats = {0};
            actor_foreign_stop_context stop = {0};
            cmeta_thread_t stop_thread = {0};

            check_true(actor_edge_fixture_init(&fixture, 1u));
            fixture.probe.blocker = &blocker;
            atomic_store(&fixture.probe.block_action, true);
            check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
            check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
            check_equal(cflow_actor_ref_try_send(&ref, &event),
                        CFLOW_ACTOR_SEND_ACCEPTED);
            check_true(wait_until_true(&blocker.entered));

            /* Main/control lane requests stop while worker is inside
             * a real Machine action; callback never holds run lock. */
            stop.actor = &fixture.actor;
            check_equal(cmeta_thread_create(
                            &stop_thread, actor_foreign_stop, &stop), 0);
            check_true(wait_until_true(&stop.started));
            /* Release the owner worker while a separate control lane is
             * independently requesting termination. */
            atomic_store(&blocker.release, true);
            check_true(wait_until_true(&stop.finished));
            check_equal(cmeta_thread_join(&stop_thread), 0);
            check_equal(atomic_load(&stop.status), (int)CFLOW_ACTOR_OK);
            check_equal(cflow_actor_wait(&fixture.actor),
                        CFLOW_ACTOR_STATE_STOPPED);
            check_true(cflow_actor_get_stats(&fixture.actor, &stats));
            check_equal(stats.machine.accepted, (uint64_t)1u);
            check_equal(stats.machine.accepted,
                        stats.machine.completed +
                            stats.machine.cancelled_events);
            check_equal(stats.machine.in_flight, (size_t)0u);
            check_equal(stats.machine.pending, (size_t)0u);
            check_equal(atomic_load(&fixture.probe.errors), 0);
            check_equal(atomic_load(&fixture.probe.dones), 1);

            actor_edge_fixture_destroy(&fixture);
            check_equal(cflow_actor_ref_try_send(&ref, &event),
                        CFLOW_ACTOR_SEND_STALE);
            cflow_actor_ref_release(&ref);
        }
    }
}

suite("CFlow owner Actor teardown") {
    static actor_edge_fixture fixture;
    static cflow_actor_ref ref;

    before_each() {
        ref = (cflow_actor_ref){0};
        check_true(actor_edge_fixture_init_full_owner(&fixture, 2u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
    }

    after_each() {
        cflow_executor_control control = {0};
        cflow_actor_destroy(&fixture.actor);
        /* Keep failure cleanup safe if a regression leaves borrowed Machine
         * tasks queued: discard descriptors instead of executing them. */
        if (cflow_executor_valid(&fixture.executor) &&
            cflow_executor_project_control(&fixture.executor, &control)) {
            (void)cflow_executor_control_shutdown(
                &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING);
            (void)cflow_executor_run_ready(&fixture.executor);
        }
        cflow_actor_ref_release(&ref);
        actor_edge_fixture_destroy(&fixture);
    }

    it("settles queued Machine work before releasing the Actor") {
        const int payload = 42;
        const cflow_event_view event = {100u, &cmeta_type_int, &payload};
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        /* The first Subscription turn queues a Machine task and yields. */
        check_true(cflow_executor_run_one(&fixture.executor));
        check_equal(cflow_executor_pending(&fixture.executor), (size_t)1u);

        cflow_actor_destroy(&fixture.actor);
        check_null(fixture.actor.impl);
        check_equal(cflow_executor_pending(&fixture.executor), (size_t)0u);
        check_equal(cflow_scheduler_pending(&fixture.scheduler), (size_t)0u);
        check_equal(atomic_load(&fixture.probe.action_calls), 0);
        check_equal(atomic_load(&fixture.probe.values), 0);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_STALE);

        /* Actor teardown must leave its borrowed execution lane usable. */
        check_equal(cflow_executor_try_post(
                        &fixture.executor, actor_scheduler_slot, NULL),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_run_one(&fixture.executor));
        check_false(cflow_executor_run_one(&fixture.executor));
    }

    it("destroys an owner Actor after normal terminal settlement") {
        const int payload = 17;
        const cflow_event_view event = {100u, &cmeta_type_int, &payload};
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        (void)cflow_scheduler_run_until_idle(&fixture.scheduler, 0u);
        check_equal(atomic_load(&fixture.probe.action_calls), 1);
        check_equal(atomic_load(&fixture.probe.values), 1);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        (void)cflow_scheduler_run_until_idle(&fixture.scheduler, 0u);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_equal(atomic_load(&fixture.probe.dones), 1);

        cflow_actor_destroy(&fixture.actor);
        cflow_actor_destroy(&fixture.actor);
        check_null(fixture.actor.impl);
        check_equal(cflow_executor_pending(&fixture.executor), (size_t)0u);
        check_equal(cflow_scheduler_pending(&fixture.scheduler), (size_t)0u);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_STALE);
    }
}

/* ACE Active Object: one reflected CMeta typed sender and one borrowed action
 * Strategy consume the existing Actor implementation without a second queue
 * or runtime. The strategy is called only by the Actor's serial execution. */
typedef struct ace_actor_port_binding {
    cflow_actor_ref *ref; /* borrowed, the port never owns a producer ref */
} ace_actor_port_binding;

typedef struct ace_actor_strategy_probe {
    atomic_int calls;
} ace_actor_strategy_probe;

static int ace_actor_port_send_impl(void *self, int payload) {
    const ace_actor_port_binding *binding = (const ace_actor_port_binding *)self;
    const cflow_event_view event = {100u, &cmeta_type_int, &payload};
    return (int)cflow_actor_ref_try_send(binding->ref, &event);
}

static int ace_actor_strategy_transform_impl(void *self, int payload) {
    ace_actor_strategy_probe *probe = (ace_actor_strategy_probe *)self;
    atomic_fetch_add(&probe->calls, 1);
    return payload + 100;
}

CMETA_IMPLEMENTS(ace_actor_port, ace_actor_port_impl, 0u,
    .try_send = ace_actor_port_send_impl);
CMETA_IMPLEMENTS(ace_actor_strategy, ace_actor_strategy_impl, 0u,
    .transform = ace_actor_strategy_transform_impl);

suite("ACE Active Object composed from CMeta and CFlow Actor") {
    it("accepts bounded typed events and dispatches Strategy only on execution") {
        actor_edge_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        ace_actor_port_binding binding = {&ref};
        ace_actor_strategy_probe strategy_probe = {0};
        ace_actor_port port = ace_actor_port_impl_as_ace_actor_port(&binding);
        ace_actor_strategy strategy =
            ace_actor_strategy_impl_as_ace_actor_strategy(&strategy_probe);
        cflow_actor_stats stats = {0};
        const bool wrong_value = true;
        const cflow_event_view wrong_type = {
            100u, &cmeta_type_bool, &wrong_value};

        check_true(cmeta_interface_desc_valid(ace_actor_port_interface()));
        check_true(cmeta_interface_desc_valid(ace_actor_strategy_interface()));
        check_not_null(ace_actor_port_interface()->methods[0].abi);
        check_not_null(ace_actor_strategy_interface()->methods[0].abi);
        check_equal(ace_actor_port_interface()->methods[0].function->param_count,
                    (size_t)1u);
        check_equal(ace_actor_strategy_interface()->methods[0]
                    .function->result_flags & CMETA_RESULT_CLASS_MASK,
                    CMETA_RESULT_VALUE);
        check_true(ace_actor_port_valid(&port));
        check_true(ace_actor_strategy_valid(&strategy));

        check_true(actor_edge_fixture_init(&fixture, 1u));
        fixture.probe.blocker = &blocker;
        fixture.probe.ace_strategy = &strategy;
        atomic_store(&fixture.probe.block_action, true);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));

        check_equal(cflow_actor_ref_try_send(&ref, &wrong_type),
                    CFLOW_ACTOR_SEND_TYPE_MISMATCH);
        check_equal(ace_actor_port_try_send(&port, 1),
                    (int)CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(wait_until_true(&blocker.entered));
        check_equal(ace_actor_port_try_send(&port, 2),
                    (int)CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(ace_actor_port_try_send(&port, 3),
                    (int)CFLOW_ACTOR_SEND_FULL);
        check_equal(atomic_load(&strategy_probe.calls), 0);
        check_equal(atomic_load(&fixture.probe.action_calls), 1);

        atomic_store(&blocker.release, true);
        check_true(wait_until_at_least(&fixture.probe.values, 2));
        check_equal(atomic_load(&strategy_probe.calls), 2);
        check_equal(fixture.probe.observations[0], 101);
        check_equal(fixture.probe.observations[1], 102);
        check_equal(atomic_load(&fixture.probe.pair_seen[0][1]), 1);
        check_equal(atomic_load(&fixture.probe.pair_seen[0][2]), 1);
        check_equal(atomic_load(&fixture.probe.pair_seen[0][3]), 0);
        check_true(cflow_actor_get_stats(&fixture.actor, &stats));
        check_equal(stats.machine.accepted, (uint64_t)2u);
        check_equal(stats.machine.completed, (uint64_t)2u);

        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_equal(ace_actor_port_try_send(&port, 4),
                    (int)CFLOW_ACTOR_SEND_STOPPED);
        check_equal(atomic_load(&strategy_probe.calls), 2);

        /* A retained producer is stale after owner/scheduler teardown. */
        actor_edge_fixture_destroy(&fixture);
        check_equal(ace_actor_port_try_send(&port, 5),
                    (int)CFLOW_ACTOR_SEND_STALE);
        check_equal(atomic_load(&strategy_probe.calls), 2);
        cflow_actor_ref_release(&ref);
        check_null(ref.impl);
    }

    it("rejects producer admission while STOPPING and after destruction") {
        actor_edge_fixture fixture;
        actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        ace_actor_port_binding binding = {&ref};
        ace_actor_port port = ace_actor_port_impl_as_ace_actor_port(&binding);

        check_true(actor_edge_fixture_init(&fixture, 1u));
        check_not_equal(cflow_scheduler_post(
            &fixture.scheduler, block_scheduler, &blocker), (cflow_task_id)0u);
        check_true(wait_until_true(&blocker.entered));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));

        check_equal(ace_actor_port_try_send(&port, 9),
                    (int)CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPING);
        check_equal(ace_actor_port_try_send(&port, 10),
                    (int)CFLOW_ACTOR_SEND_STOPPING);
        atomic_store(&blocker.release, true);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_equal(ace_actor_port_try_send(&port, 11),
                    (int)CFLOW_ACTOR_SEND_STOPPED);
        actor_edge_fixture_destroy(&fixture);
        check_equal(ace_actor_port_try_send(&port, 12),
                    (int)CFLOW_ACTOR_SEND_STALE);
        cflow_actor_ref_release(&ref);
    }
}
