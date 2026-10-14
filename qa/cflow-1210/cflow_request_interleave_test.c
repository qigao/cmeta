/*
 * PRIVATE QA ONLY: deterministic red/green reproduction of Salts #1210.
 *
 * The corresponding QA setup script injects a test hook into two disposable
 * source worktrees. This file and that hook are NEVER built into the release
 * SDK or included in the clean three-file fix PR #1217.
 */
#include <cflow/cflow.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { QA_VALUE_COUNT = 1025 };
#define QA_BOUNDED_WAIT_NS UINT64_C(15000000000)

/* Private QA symbol emitted ONLY by the disposable source instrumentation. */
extern void cflow_qa_set_request_hook(void (*callback)(void));

typedef struct qa_gate {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    bool blocker_started;
    bool release_blocker;
    bool terminal_seen;
    bool timed_out;
    unsigned hook_calls;
    atomic_size_t values;
    atomic_int errors;
    atomic_int dones;
} qa_gate;

static qa_gate *qa_active_gate;

static bool qa_wait_locked(qa_gate *gate, const bool *flag) {
    while (!*flag && !gate->timed_out) {
        if (cmeta_cond_timedwait(&gate->changed, &gate->mutex,
                                 QA_BOUNDED_WAIT_NS) != 0) {
            gate->timed_out = true;
            cmeta_cond_broadcast(&gate->changed);
        }
    }
    return *flag && !gate->timed_out;
}

static void qa_hold_worker(void *user) {
    qa_gate *gate = (qa_gate *)user;
    cmeta_mutex_lock(&gate->mutex);
    gate->blocker_started = true;
    cmeta_cond_broadcast(&gate->changed);
    (void)qa_wait_locked(gate, &gate->release_blocker);
    cmeta_mutex_unlock(&gate->mutex);
}

/* This QA hook runs after demand is published and after the Run lock is
 * released, but BEFORE request_result() calls schedule_pump(). It is only
 * registered on this thread; the worker may now drain and finish normally. */
static void qa_after_demand_published(void) {
    qa_gate *gate = qa_active_gate;
    if (!gate) return;
    cmeta_mutex_lock(&gate->mutex);
    ++gate->hook_calls;
    gate->release_blocker = true;
    cmeta_cond_broadcast(&gate->changed);
    (void)qa_wait_locked(gate, &gate->terminal_seen);
    cmeta_mutex_unlock(&gate->mutex);
}

static bool qa_sink_value(void *user, const cmeta_type_desc *type,
                          const void *value) {
    qa_gate *gate = (qa_gate *)user;
    size_t expected = atomic_fetch_add(&gate->values, (size_t)1u);
    if (!value || !cmeta_type_equal(type, &cmeta_type_int) ||
        expected >= (size_t)QA_VALUE_COUNT ||
        *(const int *)value != (int)expected) {
        atomic_fetch_add(&gate->errors, 1);
        return false;
    }
    return true;
}

static void qa_sink_terminal(qa_gate *gate) {
    cmeta_mutex_lock(&gate->mutex);
    gate->terminal_seen = true;
    cmeta_cond_broadcast(&gate->changed);
    cmeta_mutex_unlock(&gate->mutex);
}

static void qa_sink_error(void *user, const char *message) {
    qa_gate *gate = (qa_gate *)user;
    (void)message;
    atomic_fetch_add(&gate->errors, 1);
    qa_sink_terminal(gate);
}

static void qa_sink_done(void *user) {
    qa_gate *gate = (qa_gate *)user;
    atomic_fetch_add(&gate->dones, 1);
    qa_sink_terminal(gate);
}

int main(void) {
    qa_gate gate = {0};
    cflow_scheduler scheduler = {0};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_publisher source = {0};
    cflow_subscription run = {0};
    cflow_subscriber_callbacks callbacks = {
        qa_sink_value, qa_sink_error, qa_sink_done, &gate
    };
    cflow_subscriber sink = cflow_subscriber_from_callbacks(&callbacks);
    int inputs[QA_VALUE_COUNT];
    cflow_status first_status = CFLOW_STATUS_INVALID_ARGUMENT;
    cflow_status run_status = CFLOW_STATUS_INVALID_ARGUMENT;
    cflow_status later_status = CFLOW_STATUS_INVALID_ARGUMENT;
    cflow_scheduler_stats stats = {0};
    size_t values = 0u;
    int errors = 0;
    int dones = 0;
    unsigned hooks = 0u;
    bool timeout = false, done = false, stats_ok = false;
    bool live = false;
    int answer = 19;

    atomic_init(&gate.values, (size_t)0u);
    atomic_init(&gate.errors, 0);
    atomic_init(&gate.dones, 0);
    cmeta_mutex_init(&gate.mutex);
    cmeta_cond_init(&gate.changed);
    if (!gate.mutex || !gate.changed) goto cleanup;

    for (size_t i = 0; i < (size_t)QA_VALUE_COUNT; ++i)
        inputs[i] = (int)i;

    normalized.root = CMETA_INVALID_ID;
    cflow_graph_init(&surface, &cmeta_type_int);
    if (!cflow_graph_normalize(&normalized, &surface)) goto cleanup;
    if (!cflow_scheduler_worker_init_with_capacity(
            &scheduler, 1u, 8u, 64u)) goto cleanup;

    /* Occupy the ONLY worker before wake is posted. Its pending pump cannot
     * run until the admitted-request gate deliberately releases this task. */
    const cflow_schedule_result blocker =
        cflow_scheduler_try_post_after(&scheduler, 0u, qa_hold_worker, &gate);
    if (blocker.status != CFLOW_ADMISSION_ACCEPTED) goto cleanup;

    cmeta_mutex_lock(&gate.mutex);
    bool started = qa_wait_locked(&gate, &gate.blocker_started);
    cmeta_mutex_unlock(&gate.mutex);
    if (!started) goto cleanup;

    if (!cflow_publisher_from_array(&source, &cmeta_type_int,
                                    inputs, QA_VALUE_COUNT)) goto cleanup;
    if (!cflow_subscribe(&run, &normalized, &source, &scheduler, &sink))
        goto cleanup;
    live = true;

    cflow_subscription_wake(&run);
    qa_active_gate = &gate;
    cflow_qa_set_request_hook(qa_after_demand_published);
    first_status = cflow_subscription_request_result(
        &run, SIZE_MAX).status;
    cflow_qa_set_request_hook(NULL);
    qa_active_gate = NULL;

    cmeta_mutex_lock(&gate.mutex);
    gate.release_blocker = true;
    cmeta_cond_broadcast(&gate.changed);
    done = gate.terminal_seen;
    timeout = gate.timed_out;
    hooks = gate.hook_calls;
    cmeta_mutex_unlock(&gate.mutex);

    values = atomic_load(&gate.values);
    errors = atomic_load(&gate.errors);
    dones = atomic_load(&gate.dones);
    run_status = cflow_subscription_status(&run);
    later_status = cflow_subscription_request_result(&run, 1u).status;
    stats_ok = cflow_scheduler_get_stats(&scheduler, &stats);

    /* The two builds MUST satisfy the same real terminal/delivery contract.
     * Only the initial request status may differ: CLOSED on the unfixed
     * source, OK on the clean #1217 source. */
    if (done && !timeout && hooks == 1u &&
        values == QA_VALUE_COUNT && errors == 0 && dones == 1 &&
        run_status == CFLOW_STATUS_OK &&
        later_status == CFLOW_STATUS_CLOSED && stats_ok &&
        stats.rejected_full == 0u && stats.rejected_closed == 0u) {
        if (first_status == CFLOW_STATUS_CLOSED) {
            printf("BASELINE_CLOSED_AFTER_SUCCESS status=%d values=%zu "
                   "dones=%d rejected_full=%zu rejected_closed=%zu\n",
                   (int)first_status, values, dones,
                   stats.rejected_full, stats.rejected_closed);
            answer = 42;
        } else if (first_status == CFLOW_STATUS_OK) {
            printf("FIXED_ADMITTED_OK status=%d values=%zu "
                   "dones=%d rejected_full=%zu rejected_closed=%zu\n",
                   (int)first_status, values, dones,
                   stats.rejected_full, stats.rejected_closed);
            answer = 0;
        }
    }
    if (answer == 19) {
        fprintf(stderr,
            "QA_INTERLEAVING_UNEXPECTED first=%d run=%d later=%d "
            "hook=%u done=%d timeout=%d values=%zu errors=%d dones=%d "
            "stats_ok=%d rejected_full=%zu rejected_closed=%zu\n",
            (int)first_status, (int)run_status, (int)later_status,
            hooks, done ? 1 : 0, timeout ? 1 : 0, values, errors, dones,
            stats_ok ? 1 : 0, stats.rejected_full, stats.rejected_closed);
    }

cleanup:
    /* No shared Run or borrowed callback state can outlive gate teardown. */
    if (gate.mutex) {
        cmeta_mutex_lock(&gate.mutex);
        gate.release_blocker = true;
        cmeta_cond_broadcast(&gate.changed);
        cmeta_mutex_unlock(&gate.mutex);
    }
    cflow_qa_set_request_hook(NULL);
    qa_active_gate = NULL;
    if (live) cflow_subscription_close(&run);
    if (cflow_publisher_valid(&source)) cflow_publisher_destroy(&source);
    if (cflow_scheduler_valid(&scheduler)) cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
    cmeta_cond_destroy(&gate.changed);
    cmeta_mutex_destroy(&gate.mutex);
    fflush(stdout);
    return answer;
}
