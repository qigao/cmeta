#include <cflow/actor.h>
#include <thread.h>

#include "tinytest.h"

#include <stdlib.h>

enum { BURST = 4096, ACTOR_SAMPLES = 128, MAILBOX_SAMPLES = 512 };

typedef struct actor_bench_probe {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    size_t completed;
    bool emit;
    bool failed;
} actor_bench_probe;

static void bench_complete(actor_bench_probe *probe) {
    cmeta_mutex_lock(&probe->lock);
    ++probe->completed;
    if (probe->completed % BURST == 0u)
        cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->lock);
}

static bool bench_action(void *user, const void *state, const void *event,
                         void *target, void *observation, const char **error) {
    actor_bench_probe *probe = (actor_bench_probe *)user;
    (void)event;
    (void)error;
    *(int *)target = *(const int *)state + 1;
    if (probe->emit)
        *(int *)observation = *(int *)target;
    else
        bench_complete(probe);
    return true;
}

static bool bench_value(void *user, const cmeta_type_desc *type,
                         const void *value) {
    (void)type;
    (void)value;
    bench_complete((actor_bench_probe *)user);
    return true;
}

static void bench_error(void *user, const char *message) {
    actor_bench_probe *probe = (actor_bench_probe *)user;
    (void)message;
    cmeta_mutex_lock(&probe->lock);
    probe->failed = true;
    cmeta_cond_broadcast(&probe->changed);
    cmeta_mutex_unlock(&probe->lock);
}

suite("Actor and mailbox steady-state benchmarks") {
    bench("measures mailbox schema lookup and bounded copy") {
        cflow_event_type schema[64];
        const int payload = 73;
        const cflow_event_view event = {64u, &cmeta_type_int, &payload};
        size_t schema_count;
        size_t index;
        for (index = 0u; index < 64u; ++index)
            schema[index] = (cflow_event_type){index + 1u, &cmeta_type_int};
        for (schema_count = 1u; schema_count <= 64u; schema_count *= 64u) {
            cflow_mailbox mailbox = {0};
            cflow_event_id id = 0u;
            const cmeta_type_desc *type = NULL;
            int observed = 0;
            bool valid = true;
            cflow_mailbox_stats stats = {0};
            check_equal(cflow_mailbox_init(
                &mailbox, schema + 64u - schema_count, schema_count, BURST),
                CFLOW_MAILBOX_OK);
            benchmark_ops(schema_count == 1u ? "mailbox send + receive, schema=1"
                                             : "mailbox send + receive, schema=64",
                          MAILBOX_SAMPLES, 2u * BURST) {
                for (index = 0u; index < BURST; ++index)
                    if (cflow_mailbox_try_send(&mailbox, &event) != CFLOW_MAILBOX_OK)
                        valid = false;
                for (index = 0u; index < BURST; ++index)
                    if (cflow_mailbox_try_receive(&mailbox, &id, &type,
                                                  &observed, sizeof(observed)) !=
                        CFLOW_MAILBOX_OK || id != event.id ||
                        observed != payload || type != &cmeta_type_int)
                        valid = false;
            }
            check_true(valid);
            check_true(cflow_mailbox_get_stats(&mailbox, &stats));
            check_equal(stats.accepted, (uint64_t)MAILBOX_SAMPLES * BURST);
            check_equal(stats.received, stats.accepted);
            cflow_mailbox_destroy(&mailbox);
        }
    }

    bench("measures Actor ingress and optional value delivery") {
        bool emit;
        for (emit = false;; emit = true) {
            cflow_machine machine = {0};
            cflow_actor actor = {0};
            cflow_actor_ref ref = {0};
            cflow_executor executor = {0};
            cflow_scheduler scheduler = {0};
            actor_bench_probe probe = {0};
            const cflow_machine_state state = {1u, &cmeta_type_int,
                                                CFLOW_MACHINE_STATE_ACTIVE};
            const cflow_event_type event_type = {1u, &cmeta_type_int};
            const cflow_machine_action action = {
                1u, &cmeta_type_int, 1u, &cmeta_type_int, &cmeta_type_int,
                CMETA_EFFECT_STATEFUL, CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
                emit ? CFLOW_MACHINE_ACTION_VALUE : CFLOW_MACHINE_ACTION_NONE,
                emit ? &cmeta_type_int : NULL, 0u};
            const cflow_machine_transition transition = {1u, 1u, 0u, 1u, 1u, 0u};
            const cflow_machine_definition definition = {
                &state, 1u, 1u, &event_type, 1u, NULL, 0u,
                &action, 1u, &transition, 1u};
            const cflow_machine_action_binding binding = {1u, bench_action, &probe};
            const int initial_state = 0;
            const int payload = 1;
            const cflow_event_view event = {1u, &cmeta_type_int, &payload};
            cflow_actor_config config = {0};
            cflow_actor_stats stats = {0};
            size_t expected = 0u;
            size_t index;
            bool valid = true;
            probe.emit = emit;
            cmeta_mutex_init(&probe.lock);
            cmeta_cond_init(&probe.changed);
            check_not_null(probe.lock);
            check_not_null(probe.changed);
            check_equal(cflow_machine_build(&machine, &definition), CFLOW_MACHINE_OK);
            check_true(cflow_executor_serial_init(&executor));
            check_true(cflow_scheduler_worker_init(&scheduler, 1u));
            config.machine = (cflow_machine_instance_config){
                &machine, &initial_state, &cmeta_type_int, NULL, 0u,
                &binding, 1u, BURST, &executor};
            config.scheduler = &scheduler;
            config.callbacks = (cflow_subscriber_callbacks){
                bench_value, bench_error, NULL, &probe};
            check_equal(cflow_actor_init(&actor, &config).status, CFLOW_ACTOR_OK);
            check_true(cflow_actor_ref_acquire(&actor, &ref));
            check_equal(cflow_actor_start(&actor), CFLOW_ACTOR_OK);
            check_true(cflow_scheduler_wait_idle(&scheduler));
            check_true(cflow_executor_wait_idle(&executor));
            benchmark_ops(emit ? "Actor burst with value delivery"
                               : "Actor burst without value delivery",
                          ACTOR_SAMPLES, BURST) {
                expected += BURST;
                for (index = 0u; index < BURST; ++index)
                    if (cflow_actor_ref_try_send(&ref, &event) != CFLOW_ACTOR_SEND_ACCEPTED)
                        valid = false;
                if (!valid) abort();
                cmeta_mutex_lock(&probe.lock);
                while (probe.completed < expected && !probe.failed)
                    cmeta_cond_wait(&probe.changed, &probe.lock);
                valid = valid && !probe.failed;
                cmeta_mutex_unlock(&probe.lock);
            }
            check_true(valid);
            check_equal(cflow_actor_request_stop(&actor), CFLOW_ACTOR_OK);
            check_equal(cflow_actor_wait(&actor), CFLOW_ACTOR_STATE_STOPPED);
            check_true(cflow_actor_get_stats(&actor, &stats));
            check_equal(stats.machine.completed, (uint64_t)ACTOR_SAMPLES * BURST);
            check_equal(stats.machine.failed, (uint64_t)0u);
            cflow_actor_ref_release(&ref);
            cflow_actor_destroy(&actor);
            cflow_scheduler_destroy(&scheduler);
            cflow_executor_destroy(&executor);
            cflow_machine_destroy(&machine);
            cmeta_cond_destroy(&probe.changed);
            cmeta_mutex_destroy(&probe.lock);
            if (emit) break;
        }
    }
}
