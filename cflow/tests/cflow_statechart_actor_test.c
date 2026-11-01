#include <cflow/cflow.h>

#include <clock.h>
#include <thread.h>

#include "tinytest.h"

#include <stdatomic.h>
#include <string.h>

enum {
    ACTOR_ROOT = 10,
    ACTOR_INITIAL = 20,
    ACTOR_ACTIVE = 30,
    ACTOR_FINAL = 40,
    ACTOR_LOOP_EVENT = 100,
    ACTOR_FINISH_EVENT = 101,
    ACTOR_INITIAL_TRANSITION = 200,
    ACTOR_LOOP_TRANSITION = 201,
    ACTOR_FINAL_TRANSITION = 202,
    ACTOR_TEST_TIMEOUT_MS = 5000
};

typedef struct statechart_actor_probe {
    atomic_int values;
    atomic_int errors;
    atomic_int dones;
} statechart_actor_probe;

typedef struct statechart_actor_blocker {
    atomic_bool entered;
    atomic_bool release;
} statechart_actor_blocker;

typedef struct statechart_actor_fixture {
    cflow_statechart_state states[4];
    cflow_event_type events[2];
    cflow_statechart_transition transitions[3];
    cflow_statechart_definition definition;
    cflow_statechart statechart;
    cflow_executor executor;
    cflow_scheduler scheduler;
    cflow_actor actor;
    statechart_actor_probe probe;
    int initial_state;
} statechart_actor_fixture;

/* Force the already-terminal source's pump to finish during subscribe, before
 * start can request demand. Keep the real concurrent backend and its methods. */
static SALTS_THREAD_LOCAL cflow_scheduler *early_terminal_scheduler;

static cflow_schedule_result early_terminal_post(
    void *self, uint64_t delay, cflow_task_fn fn, void *user) {
    cflow_schedule_result result = early_terminal_scheduler->vtable->try_post_after(
        self, delay, fn, user);
    if (result.status == CFLOW_ADMISSION_ACCEPTED)
        (void)cflow_scheduler_wait_idle(early_terminal_scheduler);
    return result;
}

static bool statechart_actor_wait_flag(atomic_bool *value) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        if (atomic_load(value)) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(value);
}

static bool statechart_actor_wait_count(atomic_int *value, int expected) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        if (atomic_load(value) >= expected) return true;
        cmeta_sleep_ms(1u);
    }
    return atomic_load(value) >= expected;
}

static bool statechart_actor_wait_state(
    const cflow_actor *actor, cflow_actor_state expected) {
    const uint64_t started = cmeta_monotonic_ms();
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS) {
        if (cflow_actor_current_state(actor) == expected) return true;
        cmeta_sleep_ms(1u);
    }
    return cflow_actor_current_state(actor) == expected;
}

static bool statechart_actor_on_value(
    void *user, const cmeta_type_desc *type, const void *value) {
    statechart_actor_probe *probe = (statechart_actor_probe *)user;
    (void)type;
    (void)value;
    if (probe != NULL) atomic_fetch_add(&probe->values, 1);
    return true;
}

static void statechart_actor_on_error(void *user, const char *message) {
    statechart_actor_probe *probe = (statechart_actor_probe *)user;
    if (probe != NULL && message != NULL)
        atomic_fetch_add(&probe->errors, 1);
}

static void statechart_actor_on_done(void *user) {
    statechart_actor_probe *probe = (statechart_actor_probe *)user;
    if (probe != NULL) atomic_fetch_add(&probe->dones, 1);
}

static void statechart_actor_block(void *user) {
    statechart_actor_blocker *blocker =
        (statechart_actor_blocker *)user;
    const uint64_t started = cmeta_monotonic_ms();
    if (blocker == NULL) return;
    atomic_store(&blocker->entered, true);
    while (cmeta_monotonic_ms() - started < ACTOR_TEST_TIMEOUT_MS &&
           !atomic_load(&blocker->release))
        cmeta_sleep_ms(1u);
}

static void statechart_actor_define(statechart_actor_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->states[0] = (cflow_statechart_state){
        ACTOR_ROOT, 0u, CFLOW_STATECHART_COMPOUND, 0u};
    fixture->states[1] = (cflow_statechart_state){
        ACTOR_INITIAL, ACTOR_ROOT, CFLOW_STATECHART_INITIAL, 1u};
    fixture->states[2] = (cflow_statechart_state){
        ACTOR_ACTIVE, ACTOR_ROOT, CFLOW_STATECHART_ATOMIC, 2u};
    fixture->states[3] = (cflow_statechart_state){
        ACTOR_FINAL, ACTOR_ROOT, CFLOW_STATECHART_FINAL, 3u};
    fixture->events[0] = (cflow_event_type){
        ACTOR_LOOP_EVENT, &cmeta_type_int};
    fixture->events[1] = (cflow_event_type){
        ACTOR_FINISH_EVENT, &cmeta_type_int};
    fixture->transitions[0] = (cflow_statechart_transition){
        ACTOR_INITIAL_TRANSITION, ACTOR_INITIAL,
        CFLOW_STATECHART_TRIGGER_EVENTLESS, 0u, 0u, 0u,
        ACTOR_ACTIVE, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 0u};
    fixture->transitions[1] = (cflow_statechart_transition){
        ACTOR_LOOP_TRANSITION, ACTOR_ACTIVE,
        CFLOW_STATECHART_TRIGGER_EVENT, ACTOR_LOOP_EVENT, 0u, 0u,
        ACTOR_ACTIVE, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 1u};
    fixture->transitions[2] = (cflow_statechart_transition){
        ACTOR_FINAL_TRANSITION, ACTOR_ACTIVE,
        CFLOW_STATECHART_TRIGGER_EVENT, ACTOR_FINISH_EVENT, 0u, 0u,
        ACTOR_FINAL, CFLOW_STATECHART_TRANSITION_EXTERNAL, 0u, 2u};
    fixture->definition = (cflow_statechart_definition){
        &cmeta_type_int,
        fixture->states, 4u,
        fixture->events, 2u,
        NULL, 0u,
        NULL, 0u,
        fixture->transitions, 3u,
        NULL, 0u,
        NULL, 0u};
    fixture->initial_state = 41;
}

static bool statechart_actor_fixture_init(
    statechart_actor_fixture *fixture, size_t external_capacity) {
    cflow_statechart_actor_config config = {0};
    statechart_actor_define(fixture);
    if (cflow_statechart_build(&fixture->statechart,
                               &fixture->definition) !=
        CFLOW_STATECHART_OK)
        return false;
    if (!cflow_executor_serial_init(&fixture->executor)) return false;
    if (!cflow_scheduler_worker_init(&fixture->scheduler, 1u)) return false;
    config.statechart = (cflow_statechart_instance_config){
        .statechart = &fixture->statechart,
        .initial_state = &fixture->initial_state,
        .external_event_capacity = external_capacity,
        .internal_event_capacity = 4u,
        .completion_capacity = 4u,
        .microstep_limit = 16u,
        .executor = &fixture->executor};
    config.scheduler = &fixture->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){
        statechart_actor_on_value,
        statechart_actor_on_error,
        statechart_actor_on_done,
        &fixture->probe};
    return cflow_statechart_actor_init(&fixture->actor, &config).status ==
        CFLOW_ACTOR_OK;
}

static void statechart_actor_fixture_destroy(
    statechart_actor_fixture *fixture) {
    cflow_actor_destroy(&fixture->actor);
    if (cflow_scheduler_valid(&fixture->scheduler))
        cflow_scheduler_destroy(&fixture->scheduler);
    if (cflow_executor_valid(&fixture->executor))
        cflow_executor_destroy(&fixture->executor);
    cflow_statechart_destroy(&fixture->statechart);
}

spec("CFlow Statechart Actor facade") {
    it("preserves exact Statechart initialization rejection") {
        cflow_actor actor = {0};
        cflow_scheduler scheduler = {0};
        statechart_actor_fixture fixture;
        cflow_statechart_actor_config config = {0};
        cflow_statechart_actor_init_result result =
            cflow_statechart_actor_init(NULL, &config);

        check_equal(result.status, CFLOW_ACTOR_INVALID_ARGUMENT);
        check_equal(result.statechart_status, CFLOW_STATECHART_INSTANCE_OK);
        result = cflow_statechart_actor_init(&actor, NULL);
        check_equal(result.status, CFLOW_ACTOR_INVALID_ARGUMENT);
        check_equal(result.statechart_status, CFLOW_STATECHART_INSTANCE_OK);

        check_true(cflow_scheduler_worker_init(&scheduler, 1u));
        config.scheduler = &scheduler;
        result = cflow_statechart_actor_init(&actor, &config);
        check_equal(result.status, CFLOW_ACTOR_STATECHART_REJECTED);
        check_equal(result.statechart_status,
                    CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT);
        check_null(actor.impl);
        cflow_scheduler_destroy(&scheduler);

        statechart_actor_define(&fixture);
        check_equal(cflow_statechart_build(
                        &fixture.statechart, &fixture.definition),
                    CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&fixture.executor));
        check_true(cflow_scheduler_test_init(&fixture.scheduler));
        config = (cflow_statechart_actor_config){0};
        config.statechart = (cflow_statechart_instance_config){
            .statechart = &fixture.statechart,
            .initial_state = &fixture.initial_state,
            .external_event_capacity = 2u,
            .internal_event_capacity = 2u,
            .completion_capacity = 2u,
            .microstep_limit = 8u,
            .executor = &fixture.executor};
        config.scheduler = &fixture.scheduler;
        result = cflow_statechart_actor_init(&fixture.actor, &config);
        check_equal(result.status, CFLOW_ACTOR_INVALID_SCHEDULER);
        check_equal(result.statechart_status, CFLOW_STATECHART_INSTANCE_OK);
        check_null(fixture.actor.impl);
        statechart_actor_fixture_destroy(&fixture);
    }

    it("preserves root FINAL delivered before start requests demand") {
        const cflow_statechart_state root = {ACTOR_ROOT, 0u, CFLOW_STATECHART_FINAL, 0u};
        const cflow_statechart_definition definition = {
            &cmeta_type_int, &root, 1u, NULL, 0u, NULL, 0u,
            NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u};
        cflow_statechart statechart = {0};
        cflow_executor executor = {0};
        cflow_scheduler scheduler = {0};
        cflow_scheduler proxy;
        cflow_scheduler_vtable proxy_vtable;
        cflow_actor actor = {0};
        statechart_actor_probe probe = {0};
        const int initial = 0;
        cflow_statechart_actor_config config = {0};
        check_equal(cflow_statechart_build(&statechart, &definition), CFLOW_STATECHART_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_true(cflow_scheduler_worker_init(&scheduler, 1u));
        proxy_vtable = *scheduler.vtable;
        proxy_vtable.try_post_after = early_terminal_post;
        proxy = scheduler;
        proxy.vtable = &proxy_vtable;
        config.statechart = (cflow_statechart_instance_config){
            .statechart = &statechart, .initial_state = &initial,
            .external_event_capacity = 1u, .internal_event_capacity = 1u,
            .completion_capacity = 1u, .microstep_limit = 1u, .executor = &executor};
        config.scheduler = &proxy;
        config.callbacks = (cflow_subscriber_callbacks){
            statechart_actor_on_value, statechart_actor_on_error,
            statechart_actor_on_done, &probe};
        check_equal(cflow_statechart_actor_init(&actor, &config).status, CFLOW_ACTOR_OK);
        early_terminal_scheduler = &scheduler;
        check_equal(cflow_actor_start(&actor), CFLOW_ACTOR_OK);
        early_terminal_scheduler = NULL;
        check_equal(cflow_actor_wait(&actor), CFLOW_ACTOR_STATE_STOPPED);
        check_equal(atomic_load(&probe.dones), 1);
        check_equal(atomic_load(&probe.errors), 0);
        check_null(cflow_actor_error(&actor));
        cflow_actor_destroy(&actor);
        cflow_scheduler_destroy(&scheduler);
        cflow_executor_destroy(&executor);
        cflow_statechart_destroy(&statechart);
    }

    it("maps bounded admission and treats root FINAL as normal completion") {
        statechart_actor_fixture fixture;
        statechart_actor_blocker blocker = {0};
        cflow_actor_ref ref = {0};
        cflow_statechart_actor_stats stats = {0};
        cflow_actor_stats wrong_stats;
        cflow_actor_stats wrong_snapshot;
        const int payloads[] = {1, 2, 3};
        const cflow_event_view loop = {
            ACTOR_LOOP_EVENT, &cmeta_type_int, &payloads[0]};
        const cflow_event_view queued = {
            ACTOR_LOOP_EVENT, &cmeta_type_int, &payloads[1]};
        const cflow_event_view finish = {
            ACTOR_FINISH_EVENT, &cmeta_type_int, &payloads[2]};
        const cflow_event_view mismatch = {
            ACTOR_LOOP_EVENT, &cmeta_type_bool, &payloads[0]};
        const cflow_executor_task blocking_task = {
            .run = statechart_actor_block, .user = &blocker};

        check_true(statechart_actor_fixture_init(&fixture, 1u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_ref_try_send(&ref, &loop),
                    CFLOW_ACTOR_SEND_NOT_STARTED);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &mismatch),
                    CFLOW_ACTOR_SEND_TYPE_MISMATCH);
        check_equal(cflow_executor_try_post_task(
                        &fixture.executor, &blocking_task),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(statechart_actor_wait_flag(&blocker.entered));
        check_equal(cflow_actor_ref_try_send(&ref, &loop),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_ref_try_send(&ref, &queued),
                    CFLOW_ACTOR_SEND_FULL);
        atomic_store(&blocker.release, true);
        check_true(cflow_executor_wait_idle(&fixture.executor));
        check_equal(cflow_actor_ref_try_send(&ref, &finish),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_wait(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        check_true(statechart_actor_wait_count(
            &fixture.probe.dones, 1));

        check_equal(atomic_load(&fixture.probe.values), 0);
        check_equal(atomic_load(&fixture.probe.errors), 0);
        check_equal(atomic_load(&fixture.probe.dones), 1);
        check_true(cflow_statechart_actor_get_stats(
            &fixture.actor, &stats));
        check_equal(stats.state, CFLOW_ACTOR_STATE_STOPPED);
        check_equal(stats.statechart.external_accepted, (uint64_t)2u);
        check_equal(stats.statechart.external_completed, (uint64_t)2u);
        check_true(stats.statechart.done);
        memset(&wrong_stats, 0x5a, sizeof(wrong_stats));
        wrong_snapshot = wrong_stats;
        check_false(cflow_actor_get_stats(&fixture.actor, &wrong_stats));
        check_equal(memcmp(&wrong_stats, &wrong_snapshot,
                           sizeof(wrong_stats)), 0);
        check_equal(cflow_actor_ref_try_send(&ref, &loop),
                    CFLOW_ACTOR_SEND_STOPPED);

        cflow_actor_ref_release(&ref);
        statechart_actor_fixture_destroy(&fixture);
    }

    it("closes directly from START and preserves terminal send status") {
        statechart_actor_fixture fixture;
        cflow_actor_ref ref = {0};
        cflow_statechart_actor_stats stats = {0};
        const int payload = 7;
        const cflow_event_view event = {
            ACTOR_LOOP_EVENT, &cmeta_type_int, &payload};

        check_true(statechart_actor_fixture_init(&fixture, 2u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_STOPPED);
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_STOPPED);
        check_true(cflow_statechart_actor_get_stats(
            &fixture.actor, &stats));
        check_true(stats.statechart.closed);
        check_true(stats.statechart.done);

        cflow_actor_ref_release(&ref);
        statechart_actor_fixture_destroy(&fixture);
    }

    it("cancels a queued Event before completing a running stop") {
        statechart_actor_fixture fixture;
        statechart_actor_blocker executor_blocker = {0};
        statechart_actor_blocker scheduler_blocker = {0};
        cflow_actor_ref ref = {0};
        cflow_statechart_actor_stats stats = {0};
        const int payload = 9;
        const cflow_event_view event = {
            ACTOR_LOOP_EVENT, &cmeta_type_int, &payload};
        const cflow_executor_task blocking_task = {
            .run = statechart_actor_block, .user = &executor_blocker};

        check_true(statechart_actor_fixture_init(&fixture, 2u));
        check_equal(cflow_executor_try_post_task(
                        &fixture.executor, &blocking_task),
                    CFLOW_ADMISSION_ACCEPTED);
        check_true(statechart_actor_wait_flag(&executor_blocker.entered));
        check_not_equal(cflow_scheduler_post(
                            &fixture.scheduler, statechart_actor_block,
                            &scheduler_blocker),
                        (cflow_task_id)0u);
        check_true(statechart_actor_wait_flag(&scheduler_blocker.entered));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPING);

        atomic_store(&executor_blocker.release, true);
        atomic_store(&scheduler_blocker.release, true);
        check_equal(cflow_actor_wait(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        check_true(statechart_actor_wait_count(
            &fixture.probe.dones, 1));
        check_true(cflow_statechart_actor_get_stats(
            &fixture.actor, &stats));
        check_equal(stats.statechart.external_accepted, (uint64_t)1u);
        check_equal(stats.statechart.external_cancelled, (uint64_t)1u);
        check_equal(stats.statechart.external_pending, (size_t)0u);
        check_equal(stats.statechart.external_in_flight, (size_t)0u);
        check_equal(stats.statechart.external_accepted,
                    stats.statechart.external_cancelled);

        cflow_actor_ref_release(&ref);
        statechart_actor_fixture_destroy(&fixture);
    }

    it("propagates one stable Statechart executor failure") {
        statechart_actor_fixture fixture;
        cflow_actor_ref ref = {0};
        cflow_statechart_actor_stats stats = {0};
        const int payload = 11;
        const cflow_event_view event = {
            ACTOR_LOOP_EVENT, &cmeta_type_int, &payload};
        const char *first_error;

        check_true(statechart_actor_fixture_init(&fixture, 2u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        check_equal(cflow_actor_start(&fixture.actor), CFLOW_ACTOR_OK);
        check_true(cflow_executor_shutdown(&fixture.executor));
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_ACCEPTED);
        check_true(statechart_actor_wait_state(
            &fixture.actor, CFLOW_ACTOR_STATE_FAILED));
        check_equal(cflow_actor_wait(&fixture.actor),
                    CFLOW_ACTOR_STATE_FAILED);
        first_error = cflow_actor_error(&fixture.actor);
        check_not_null(first_error);
        check_equal(atomic_load(&fixture.probe.errors), 1);
        check_equal(atomic_load(&fixture.probe.dones), 0);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_FAILED);
        check_true(cflow_statechart_actor_get_stats(
            &fixture.actor, &stats));
        check_true(stats.statechart.errored);
        check_equal(stats.statechart.last_status,
                    CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED);
        check_equal(cflow_actor_error(&fixture.actor), first_error);

        cflow_actor_ref_release(&ref);
        statechart_actor_fixture_destroy(&fixture);
    }

    it("keeps retained refs stale after owner destruction") {
        statechart_actor_fixture fixture;
        cflow_actor_ref ref = {0};
        const int payload = 13;
        const cflow_event_view event = {
            ACTOR_LOOP_EVENT, &cmeta_type_int, &payload};

        check_true(statechart_actor_fixture_init(&fixture, 2u));
        check_true(cflow_actor_ref_acquire(&fixture.actor, &ref));
        cflow_actor_destroy(&fixture.actor);
        check_null(fixture.actor.impl);
        check_equal(cflow_actor_ref_try_send(&ref, &event),
                    CFLOW_ACTOR_SEND_STALE);
        cflow_actor_ref_release(&ref);
        statechart_actor_fixture_destroy(&fixture);
    }
}
