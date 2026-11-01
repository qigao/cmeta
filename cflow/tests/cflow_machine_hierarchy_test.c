#include <cflow/machine_hierarchy.h>

#include <thread.h>

#include "tinytest.h"

static cflow_machine_hierarchy_definition hierarchy_definition(
    cflow_machine_hierarchy_state *states,
    cflow_event_type *events,
    cflow_machine_guard *guards,
    cflow_machine_action *actions,
    cflow_machine_transition *transitions) {
    states[0] = (cflow_machine_hierarchy_state){
        1u, 0u, 2u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    states[1] = (cflow_machine_hierarchy_state){
        2u, 1u, 3u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    states[2] = (cflow_machine_hierarchy_state){
        3u, 2u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    states[3] = (cflow_machine_hierarchy_state){
        4u, 2u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    states[4] = (cflow_machine_hierarchy_state){
        5u, 1u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_DONE};
    events[0] = (cflow_event_type){10u, &cmeta_type_bool};
    guards[0] = (cflow_machine_guard){
        20u, &cmeta_type_int, 10u, &cmeta_type_bool,
        CMETA_EFFECT_PURE, CMETA_PROP_STABLE | CMETA_PROP_NO_ALIAS};
    guards[1] = guards[0];
    guards[1].id = 21u;
    actions[0] = (cflow_machine_action){
        30u, &cmeta_type_int, 10u, &cmeta_type_bool, &cmeta_type_int,
        CMETA_EFFECT_PURE, CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
        CFLOW_MACHINE_ACTION_NONE, NULL, 0u};
    actions[1] = actions[0];
    actions[1].id = 31u;
    transitions[0] = (cflow_machine_transition){
        3u, 10u, 20u, 30u, 4u, 90u};
    transitions[1] = (cflow_machine_transition){
        2u, 10u, 21u, 31u, 5u, 1u};
    return (cflow_machine_hierarchy_definition){
        states, 5u, 1u, events, 1u, guards, 2u, actions, 2u,
        transitions, 2u};
}

static cflow_machine_hierarchy_status build_status(
    const cflow_machine_hierarchy_definition *definition) {
    cflow_machine_hierarchy hierarchy = {0};
    const cflow_machine_hierarchy_status status =
        cflow_machine_hierarchy_build(&hierarchy, definition);
    cflow_machine_hierarchy_destroy(&hierarchy);
    return status;
}

static void destroy_resumable(cflow_resumable *resumable) {
    if (resumable != NULL && resumable->ops != NULL)
        resumable->ops->destroy(resumable->state);
    if (resumable != NULL) *resumable = (cflow_resumable){0};
}

typedef struct hierarchy_close_wake_probe {
    cflow_machine_hierarchy_instance *instance;
    cflow_event_view event;
    cflow_timer_event_status schedule_status;
    size_t wakes;
} hierarchy_close_wake_probe;

static void hierarchy_close_wake(void *user) {
    hierarchy_close_wake_probe *probe =
        (hierarchy_close_wake_probe *)user;
    cflow_timer_event_schedule_result result;
    if (probe == NULL) return;
    ++probe->wakes;
    result = cflow_machine_hierarchy_instance_try_schedule_after(
        probe->instance, 2u, (cflow_duration){1u}, &probe->event);
    probe->schedule_status = result.status;
}

suite("CFlow hierarchical Machine normalization") {
    it("descends initial states and bubbles leaf transitions before parents") {
        cflow_machine_hierarchy_state states[5];
        cflow_event_type events[1];
        cflow_machine_guard guards[2];
        cflow_machine_action actions[2];
        cflow_machine_transition transitions[2];
        cflow_machine_hierarchy_definition definition = hierarchy_definition(
            states, events, guards, actions, transitions);
        cflow_machine_hierarchy hierarchy = {0};
        const cflow_machine *flat;

        check_equal(cflow_machine_hierarchy_build(&hierarchy, &definition),
                    CFLOW_MACHINE_HIERARCHY_OK);
        flat = cflow_machine_hierarchy_flat_machine(&hierarchy);
        check_not_null(flat);
        check_equal(cflow_machine_initial_state(flat),
                    (cflow_machine_state_id)3u);
        check_equal(cflow_machine_state_count(flat), (size_t)3u);
        check_equal(cflow_machine_transition_count(flat), (size_t)3u);

        check_equal(cflow_machine_transition_at(flat, 0u)->source,
                    (cflow_machine_state_id)3u);
        check_equal(cflow_machine_transition_at(flat, 0u)->guard,
                    (cflow_machine_guard_id)20u);
        check_equal(cflow_machine_transition_at(flat, 0u)->priority,
                    (uint32_t)0u);
        check_equal(cflow_machine_transition_at(flat, 1u)->source,
                    (cflow_machine_state_id)3u);
        check_equal(cflow_machine_transition_at(flat, 1u)->guard,
                    (cflow_machine_guard_id)21u);
        check_equal(cflow_machine_transition_at(flat, 1u)->priority,
                    (uint32_t)1u);
        check_equal(cflow_machine_transition_at(flat, 2u)->source,
                    (cflow_machine_state_id)4u);
        check_equal(cflow_machine_transition_at(flat, 2u)->guard,
                    (cflow_machine_guard_id)21u);
        cflow_machine_hierarchy_destroy(&hierarchy);
    }

    it("descends composite transition targets through their initial child") {
        const cflow_machine_hierarchy_state states[] = {
            {1u, 0u, 2u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {2u, 1u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {3u, 1u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
        };
        const cflow_event_type events[] = {{10u, &cmeta_type_bool}};
        const cflow_machine_transition transitions[] = {
            {2u, 10u, 0u, 0u, 3u, 0u},
            {3u, 10u, 0u, 0u, 1u, 0u}
        };
        const cflow_machine_hierarchy_definition definition = {
            states, 3u, 1u, events, 1u, NULL, 0u, NULL, 0u,
            transitions, 2u
        };
        cflow_machine_hierarchy hierarchy = {0};
        const cflow_machine *flat;

        check_equal(cflow_machine_hierarchy_build(&hierarchy, &definition),
                    CFLOW_MACHINE_HIERARCHY_OK);
        flat = cflow_machine_hierarchy_flat_machine(&hierarchy);
        check_equal(cflow_machine_transition_at(flat, 1u)->target,
                    (cflow_machine_state_id)2u);
        cflow_machine_hierarchy_destroy(&hierarchy);
    }

    it("records leaf-to-LCA exits and LCA-to-leaf entries") {
        cflow_machine_hierarchy_state states[5];
        cflow_event_type events[1];
        cflow_machine_guard guards[2];
        cflow_machine_action actions[2];
        cflow_machine_transition transitions[2];
        cflow_machine_hierarchy_definition definition = hierarchy_definition(
            states, events, guards, actions, transitions);
        cflow_machine_hierarchy hierarchy = {0};
        cflow_machine_hierarchy_route route = {0};

        check_equal(cflow_machine_hierarchy_build(&hierarchy, &definition),
                    CFLOW_MACHINE_HIERARCHY_OK);
        check_true(cflow_machine_hierarchy_route_at(&hierarchy, 0u, &route));
        check_equal(route.exit_count, (size_t)1u);
        check_equal(route.exit_states[0], (cflow_machine_state_id)3u);
        check_equal(route.entry_count, (size_t)1u);
        check_equal(route.entry_states[0], (cflow_machine_state_id)4u);
        check_true(cflow_machine_hierarchy_route_at(&hierarchy, 1u, &route));
        check_equal(route.exit_count, (size_t)2u);
        check_equal(route.exit_states[0], (cflow_machine_state_id)3u);
        check_equal(route.exit_states[1], (cflow_machine_state_id)2u);
        check_equal(route.entry_count, (size_t)1u);
        check_equal(route.entry_states[0], (cflow_machine_state_id)5u);
        check_false(cflow_machine_hierarchy_route_at(&hierarchy, 3u, &route));
        cflow_machine_hierarchy_destroy(&hierarchy);
    }

    it("rejects cycles invalid initial children and heterogeneous state types") {
        cflow_machine_hierarchy_state states[5];
        cflow_event_type events[1];
        cflow_machine_guard guards[2];
        cflow_machine_action actions[2];
        cflow_machine_transition transitions[2];
        cflow_machine_hierarchy_definition definition = hierarchy_definition(
            states, events, guards, actions, transitions);
        const cflow_machine_hierarchy_definition empty = {0};

        check_equal(build_status(&empty),
                    CFLOW_MACHINE_HIERARCHY_EMPTY);

        states[0].parent = 2u;
        check_equal(build_status(&definition),
                    CFLOW_MACHINE_HIERARCHY_INVALID_PARENT);
        definition = hierarchy_definition(states, events, guards, actions,
                                           transitions);
        states[1].initial_child = 5u;
        check_equal(build_status(&definition),
                    CFLOW_MACHINE_HIERARCHY_INVALID_INITIAL_CHILD);
        definition = hierarchy_definition(states, events, guards, actions,
                                           transitions);
        states[4].value_type = &cmeta_type_long;
        check_equal(build_status(&definition),
                    CFLOW_MACHINE_HIERARCHY_TYPE_MISMATCH);
        definition = hierarchy_definition(states, events, guards, actions,
                                           transitions);
        transitions[1].source = 3u;
        transitions[1].priority = 90u;
        check_equal(build_status(&definition),
                    CFLOW_MACHINE_HIERARCHY_AMBIGUOUS_TRANSITION);
    }

    it("propagates terminal states and rejects timers outside active scopes") {
        const cflow_machine_state_kind terminal_kinds[] = {
            CFLOW_MACHINE_STATE_DONE,
            CFLOW_MACHINE_STATE_ERROR
        };
        size_t index;
        for (index = 0u; index < 2u; ++index) {
            const cflow_machine_hierarchy_state states[] = {
                {1u, 0u, 2u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
                {2u, 1u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
                {3u, 1u, 0u, &cmeta_type_int, terminal_kinds[index]}
            };
            const cflow_event_type events[] = {{10u, &cmeta_type_bool}};
            const cflow_machine_transition transitions[] = {
                {2u, 10u, 0u, 0u, 3u, 0u}
            };
            const cflow_machine_hierarchy_definition definition = {
                states, 3u, 1u, events, 1u, NULL, 0u, NULL, 0u,
                transitions, 1u
            };
            cflow_machine_hierarchy hierarchy = {0};
            cflow_machine_hierarchy_instance instance = {0};
            cflow_executor executor = {0};
            cflow_clock clock = {0};
            cflow_resumable resumable = {0};
            cflow_publish_context resume_context = {0};
            const int initial_state = 0;
            const bool payload = true;
            const cflow_event_view event = {
                10u, &cmeta_type_bool, &payload
            };
            cflow_machine_hierarchy_instance_config config;
            cflow_machine_hierarchy_instance_init_result initialized;
            cflow_machine_hierarchy_instance_stats stats = {0};
            cflow_timer_event_schedule_result scheduled;
            cflow_step step;
            int output = 0;

            check_equal(cflow_machine_hierarchy_build(
                            &hierarchy, &definition),
                        CFLOW_MACHINE_HIERARCHY_OK);
            check_equal(cflow_machine_transition_at(
                            cflow_machine_hierarchy_flat_machine(&hierarchy),
                            0u)->target,
                        (cflow_machine_state_id)3u);
            check_true(cflow_executor_serial_init(&executor));
            check_true(cflow_clock_virtual_init(&clock, (cflow_instant){0u}));
            config = (cflow_machine_hierarchy_instance_config){
                &hierarchy, &initial_state, &cmeta_type_int,
                NULL, 0u, NULL, 0u, 1u, &executor, &clock, 1u
            };
            initialized = cflow_machine_hierarchy_instance_init(
                &instance, &config);
            check_equal(initialized.status,
                        CFLOW_MACHINE_HIERARCHY_INSTANCE_OK);
            check_true(cflow_machine_hierarchy_instance_as_resumable(
                &instance, &resumable));
            step = resumable.ops->resume(
                resumable.state, &resume_context, &output);
            check_equal(step.kind, CFLOW_STEP_WAIT);
            scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
                &instance, 1u, cflow_duration_from_ms(100u), &event);
            check_equal(scheduled.status, CFLOW_TIMER_EVENT_OK);
            check_equal(cflow_machine_hierarchy_instance_try_send(
                            &instance, &event),
                        CFLOW_MAILBOX_OK);
            check_true(cflow_executor_wait_idle(&executor));
            check_equal(cflow_machine_hierarchy_instance_current_state(
                            &instance),
                        (cflow_machine_state_id)3u);
            scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
                &instance, 3u, cflow_duration_from_ms(1u), &event);
            check_equal(scheduled.status, CFLOW_TIMER_EVENT_CLOSED);
            check_true(cflow_machine_hierarchy_instance_get_stats(
                &instance, &stats));
            check_equal(stats.timers.pending, (size_t)0u);
            check_true(stats.timers.closed);
            step = resumable.ops->resume(
                resumable.state, &resume_context, &output);
            check_equal(step.kind,
                        terminal_kinds[index] == CFLOW_MACHINE_STATE_DONE
                            ? CFLOW_STEP_DONE : CFLOW_STEP_ERROR);
            if (terminal_kinds[index] == CFLOW_MACHINE_STATE_ERROR)
                check_not_null(cflow_machine_hierarchy_instance_error(
                    &instance));

            destroy_resumable(&resumable);
            cflow_machine_hierarchy_instance_destroy(&instance);
            cflow_clock_destroy(&clock);
            cflow_executor_destroy(&executor);
            cflow_machine_hierarchy_destroy(&hierarchy);
        }
    }

    it("closes all scopes when an unhandled Event fails the Machine") {
        const cflow_machine_hierarchy_state states[] = {
            {1u, 0u, 2u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {2u, 1u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
        };
        const cflow_event_type events[] = {{10u, &cmeta_type_bool}};
        const cflow_machine_hierarchy_definition definition = {
            states, 2u, 1u, events, 1u, NULL, 0u, NULL, 0u, NULL, 0u
        };
        cflow_machine_hierarchy hierarchy = {0};
        cflow_machine_hierarchy_instance instance = {0};
        cflow_executor executor = {0};
        cflow_clock clock = {0};
        cflow_resumable resumable = {0};
        cflow_publish_context resume_context = {0};
        cflow_machine_hierarchy_instance_stats stats = {0};
        const int initial_state = 0;
        const bool payload = true;
        const cflow_event_view event = {10u, &cmeta_type_bool, &payload};
        cflow_machine_hierarchy_instance_config config;
        cflow_machine_hierarchy_instance_init_result initialized;
        cflow_timer_event_schedule_result scheduled;
        cflow_step step;
        int output = 0;

        check_equal(cflow_machine_hierarchy_build(&hierarchy, &definition),
                    CFLOW_MACHINE_HIERARCHY_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_true(cflow_clock_virtual_init(&clock, (cflow_instant){0u}));
        config = (cflow_machine_hierarchy_instance_config){
            &hierarchy, &initial_state, &cmeta_type_int,
            NULL, 0u, NULL, 0u, 1u, &executor, &clock, 1u
        };
        initialized = cflow_machine_hierarchy_instance_init(
            &instance, &config);
        check_equal(initialized.status,
                    CFLOW_MACHINE_HIERARCHY_INSTANCE_OK);
        check_true(cflow_machine_hierarchy_instance_as_resumable(
            &instance, &resumable));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
            &instance, 1u, cflow_duration_from_ms(100u), &event);
        check_equal(scheduled.status, CFLOW_TIMER_EVENT_OK);
        check_equal(cflow_machine_hierarchy_instance_try_send(
                        &instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&executor));
        check_not_null(cflow_machine_hierarchy_instance_error(&instance));
        check_true(cflow_machine_hierarchy_instance_get_stats(
            &instance, &stats));
        check_true(stats.timers.closed);
        check_equal(stats.timers.pending, (size_t)0u);
        scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
            &instance, 1u, cflow_duration_from_ms(1u), &event);
        check_equal(scheduled.status, CFLOW_TIMER_EVENT_CLOSED);

        destroy_resumable(&resumable);
        cflow_machine_hierarchy_instance_destroy(&instance);
        cflow_clock_destroy(&clock);
        cflow_executor_destroy(&executor);
        cflow_machine_hierarchy_destroy(&hierarchy);
    }

    it("cancels timers scoped to exited states without duplicating state") {
        const cflow_machine_hierarchy_state states[] = {
            {1u, 0u, 2u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {2u, 1u, 3u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {3u, 2u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE},
            {4u, 2u, 0u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE}
        };
        const cflow_event_type events[] = {
            {10u, &cmeta_type_bool}
        };
        const cflow_machine_transition transitions[] = {
            {3u, 10u, 0u, 0u, 4u, 0u},
            {4u, 10u, 0u, 0u, 3u, 0u}
        };
        const cflow_machine_hierarchy_definition definition = {
            states, 4u, 1u, events, 1u, NULL, 0u, NULL, 0u,
            transitions, 2u
        };
        cflow_machine_hierarchy hierarchy = {0};
        cflow_machine_hierarchy_instance instance = {0};
        cflow_machine_hierarchy_instance_config config;
        cflow_machine_hierarchy_instance_init_result initialized;
        cflow_machine_hierarchy_instance_stats stats = {0};
        cflow_executor executor = {0};
        cflow_clock clock = {0};
        cflow_resumable resumable = {0};
        cflow_publish_context resume_context = {0};
        cflow_step step;
        const bool payload = true;
        const cflow_event_view event = {10u, &cmeta_type_bool, &payload};
        cflow_timer_event_schedule_result scheduled;
        hierarchy_close_wake_probe close_probe;
        const int initial_state = 7;
        int output = 0;

        check_equal(cflow_machine_hierarchy_build(&hierarchy, &definition),
                    CFLOW_MACHINE_HIERARCHY_OK);
        check_true(cflow_executor_serial_init(&executor));
        check_true(cflow_clock_virtual_init(&clock, (cflow_instant){0u}));
        config = (cflow_machine_hierarchy_instance_config){
            &hierarchy, &initial_state, &cmeta_type_int,
            NULL, 0u, NULL, 0u, 2u, &executor, &clock, 2u
        };
        initialized = cflow_machine_hierarchy_instance_init(
            &instance, &config);
        check_equal(initialized.status,
                    CFLOW_MACHINE_HIERARCHY_INSTANCE_OK);
        check_true(cflow_machine_hierarchy_instance_as_resumable(
            &instance, &resumable));
        step = resumable.ops->resume(
            resumable.state, &resume_context, &output);
        check_equal(step.kind, CFLOW_STEP_WAIT);
        scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
            &instance, 3u, (cflow_duration){10u}, &event);
        check_equal(scheduled.status, CFLOW_TIMER_EVENT_OK);
        check_equal(cflow_machine_hierarchy_instance_try_send(
                        &instance, &event),
                    CFLOW_MAILBOX_OK);
        check_true(cflow_executor_wait_idle(&executor));
        check_equal(cflow_machine_hierarchy_instance_current_state(&instance),
                    (cflow_machine_state_id)4u);
        check_true(cflow_machine_hierarchy_instance_get_stats(
            &instance, &stats));
        check_equal(stats.timers.pending, (size_t)0u);
        check_equal(stats.timers.cancelled, (uint64_t)1u);
        scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
            &instance, 3u, (cflow_duration){1u}, &event);
        check_equal(scheduled.status, CFLOW_TIMER_EVENT_INVALID_ARGUMENT);
        scheduled = cflow_machine_hierarchy_instance_try_schedule_after(
            &instance, 2u, (cflow_duration){1u}, &event);
        check_equal(scheduled.status, CFLOW_TIMER_EVENT_OK);
        close_probe = (hierarchy_close_wake_probe){
            &instance, event, CFLOW_TIMER_EVENT_OK, 0u
        };
        check_true(cflow_waitable_arm(
            &step.waitable, (cflow_waker){hierarchy_close_wake, &close_probe}));
        resumable.ops->cancel(resumable.state);
        check_equal(close_probe.wakes, (size_t)1u);
        check_equal(close_probe.schedule_status, CFLOW_TIMER_EVENT_CLOSED);

        destroy_resumable(&resumable);
        cflow_machine_hierarchy_instance_destroy(&instance);
        cflow_clock_destroy(&clock);
        cflow_executor_destroy(&executor);
        cflow_machine_hierarchy_destroy(&hierarchy);
    }
}
