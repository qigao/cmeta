#include <cflow/executor.h>
#include <thread.h>
#include <tinytest.h>

#include <stdatomic.h>
#include <stdint.h>

extern bool cflow_executor_is_current_internal(const cflow_executor *executor);

enum { OWNER_TEST_PRODUCERS = 2, OWNER_TEST_TASKS = 8 };

typedef struct owner_test_probe {
    cflow_executor *executor;
    cflow_executor_control *control;
    const void *owner_token;
    atomic_int count;
    atomic_int wrong_owner;
    atomic_int saw_current;
    atomic_int wake_count;
    atomic_int cancelled;
    atomic_int finalized;
    atomic_int callback_post;
    atomic_int callback_post_again;
    atomic_int callback_wait;
} owner_test_probe;

typedef struct owner_producer {
    cflow_executor *executor;
    owner_test_probe *probe;
    int count;
    atomic_int rejected;
    atomic_int incorrectly_ran;
} owner_producer;

static void owner_wake(void *user) {
    owner_test_probe *probe = (owner_test_probe *)user;
    atomic_fetch_add(&probe->wake_count, 1);
    /* The wake must never recursively drive the executor. */
}

static void owner_count(void *user) {
    owner_test_probe *probe = (owner_test_probe *)user;
    if (probe->owner_token != cmeta_thread_current_token())
        atomic_fetch_add(&probe->wrong_owner, 1);
    if (cflow_executor_is_current_internal(probe->executor))
        atomic_fetch_add(&probe->saw_current, 1);
    atomic_fetch_add(&probe->count, 1);
}

static void owner_cancel(void *user) {
    owner_test_probe *probe = (owner_test_probe *)user;
    if (probe->owner_token != cmeta_thread_current_token())
        atomic_fetch_add(&probe->wrong_owner, 1);
    atomic_fetch_add(&probe->cancelled, 1);
}

static void owner_finalize(void *user) {
    owner_test_probe *probe = (owner_test_probe *)user;
    if (probe->owner_token != cmeta_thread_current_token())
        atomic_fetch_add(&probe->wrong_owner, 1);
    atomic_fetch_add(&probe->finalized, 1);
}

static void owner_producer_main(void *user) {
    owner_producer *p = (owner_producer *)user;
    int i;
    /* A foreign thread is not allowed to drive the captured owner queue. */
    if (cflow_executor_run_one(p->executor))
        atomic_fetch_add(&p->incorrectly_ran, 1);
    for (i = 0; i < p->count; ++i) {
        if (cflow_executor_try_post(p->executor, owner_count, p->probe) !=
            CFLOW_ADMISSION_ACCEPTED)
            atomic_fetch_add(&p->rejected, 1);
    }
}

static void owner_reentrant(void *user) {
    owner_test_probe *p = (owner_test_probe *)user;
    atomic_store(&p->callback_post,
        (int)cflow_executor_control_post(p->control, owner_count, p));
    atomic_store(&p->callback_post_again,
        (int)cflow_executor_control_post(p->control, owner_count, p));
    atomic_store(&p->callback_wait,
        (int)cflow_executor_control_wait_idle(p->control));
}

suite("CFlow owner-affine SerialExecutor") {
    it("binds one driver thread, accepts concurrent producers and coalesces wake") {
        cflow_executor executor = {0};
        cflow_executor_stats stats = {0};
        owner_test_probe probe = {0};
        owner_producer producers[OWNER_TEST_PRODUCERS] = {0};
        cmeta_thread_t threads[OWNER_TEST_PRODUCERS] = {0};
        int i;

        check_false(cflow_executor_owner_init_with_capacity(
            &executor, 0u, owner_wake, &probe));
        check_false(cflow_executor_valid(&executor));
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, OWNER_TEST_PRODUCERS * OWNER_TEST_TASKS,
            owner_wake, &probe));
        probe.executor = &executor;
        probe.owner_token = cmeta_thread_current_token();
        check_true(cflow_executor_has(&executor, CMETA_EXEC_CAP_SERIAL));
        check_true(cflow_executor_has(&executor, CMETA_EXEC_CAP_MANUAL));
        check_true(cflow_executor_has(&executor, CMETA_EXEC_CAP_OWNER_AFFINE));
        check_false(cflow_executor_has(&executor, CMETA_EXEC_CAP_CONCURRENT));
        check_false(cflow_executor_is_current_internal(&executor));
        check_false(cflow_executor_owner_init_with_capacity(
            &executor, 1u, owner_wake, &probe));

        for (i = 0; i < OWNER_TEST_PRODUCERS; ++i) {
            producers[i].executor = &executor;
            producers[i].probe = &probe;
            producers[i].count = OWNER_TEST_TASKS;
            check_equal(cmeta_thread_create(
                &threads[i], owner_producer_main, &producers[i]), 0);
        }
        for (i = 0; i < OWNER_TEST_PRODUCERS; ++i)
            check_equal(cmeta_thread_join(&threads[i]), 0);
        for (i = 0; i < OWNER_TEST_PRODUCERS; ++i) {
            check_equal(atomic_load(&producers[i].rejected), 0);
            check_equal(atomic_load(&producers[i].incorrectly_ran), 0);
        }

        check_equal(atomic_load(&probe.count), 0);
        check_equal(atomic_load(&probe.wake_count), 1);
        check_equal(cflow_executor_pending(&executor),
                    (size_t)(OWNER_TEST_PRODUCERS * OWNER_TEST_TASKS));
        check_equal(cflow_executor_try_post(&executor, owner_count, &probe),
                    CFLOW_ADMISSION_FULL);
        check_true(cflow_executor_get_stats(&executor, &stats));
        check_equal(stats.capacity,
                    (size_t)(OWNER_TEST_PRODUCERS * OWNER_TEST_TASKS));
        check_equal(stats.rejected_full, (size_t)1u);

        /* Host fairness: the owner can drive a fixed quantum, then yield to I/O. */
        for (i = 0; i < 4; ++i)
            check_true(cflow_executor_run_one(&executor));
        check_equal(atomic_load(&probe.count), 4);
        check_equal(cflow_executor_run_ready(&executor),
                    (size_t)(OWNER_TEST_PRODUCERS * OWNER_TEST_TASKS - 4));
        check_equal(atomic_load(&probe.count),
                    OWNER_TEST_PRODUCERS * OWNER_TEST_TASKS);
        check_equal(atomic_load(&probe.saw_current),
                    OWNER_TEST_PRODUCERS * OWNER_TEST_TASKS);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        check_true(cflow_executor_wait_idle(&executor));
        check_false(cflow_executor_is_current_internal(&executor));
        check_equal(atomic_load(&probe.wake_count), 1);
        cflow_executor_destroy(&executor);
        check_false(cflow_executor_valid(&executor));
    }

    it("keeps full and WOULD_BLOCK distinct in a callback") {
        cflow_executor executor = {0};
        cflow_executor_control control = {0};
        owner_test_probe probe = {0};
        probe.executor = &executor;
        probe.control = &control;
        probe.owner_token = cmeta_thread_current_token();

        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 1u, owner_wake, &probe));
        check_true(cflow_executor_as_control(&executor, &control));
        check_equal(cflow_executor_try_post(
            &executor, owner_reentrant, &probe), CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_run_one(&executor));
        check_equal(atomic_load(&probe.callback_post),
                    CFLOW_EXECUTOR_POST_ACCEPTED);
        check_equal(atomic_load(&probe.callback_post_again),
                    CFLOW_EXECUTOR_POST_WOULD_BLOCK);
        check_equal(atomic_load(&probe.callback_wait),
                    CFLOW_EXECUTOR_WAIT_WOULD_BLOCK);
        check_equal(atomic_load(&probe.count), 0);
        check_equal(cflow_executor_run_ready(&executor), (size_t)1u);
        check_equal(atomic_load(&probe.count), 1);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        cflow_executor_destroy(&executor);
    }

    it("settles pending cancellation and finalization on the owner exactly once") {
        cflow_executor executor = {0};
        cflow_executor_control control = {0};
        cflow_executor_protocol_stats stats = {0};
        owner_test_probe probe = {0};
        cflow_executor_task task = {
            .run = owner_count, .cancel = owner_cancel,
            .finalize = owner_finalize, .user = &probe
        };
        probe.executor = &executor;
        probe.owner_token = cmeta_thread_current_token();

        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 2u, NULL, NULL));
        check_true(cflow_executor_as_control(&executor, &control));
        check_equal(cflow_executor_try_post_task(&executor, &task),
                    CFLOW_ADMISSION_ACCEPTED);
        check_equal(cflow_executor_control_post_task(&control, &task),
                    CFLOW_EXECUTOR_POST_ACCEPTED);
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
        check_equal(cflow_executor_try_post_task(&executor, &task),
                    CFLOW_ADMISSION_CLOSED);
        check_false(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN));
        check_equal(cflow_executor_control_wait_idle(&control),
                    CFLOW_EXECUTOR_WAIT_PENDING);
        check_equal(cflow_executor_run_ready(&executor), (size_t)2u);
        check_equal(cflow_executor_control_wait_idle(&control),
                    CFLOW_EXECUTOR_WAIT_IDLE);
        check_true(cflow_executor_control_get_stats(&control, &stats));
        check_equal(stats.lifecycle, CFLOW_EXECUTOR_CLOSED);
        check_equal(stats.accepted, (size_t)2u);
        check_equal(stats.completed, (size_t)0u);
        check_equal(stats.cancelled, (size_t)2u);
        check_equal(stats.accepted, stats.completed + stats.cancelled);
        check_equal(atomic_load(&probe.count), 0);
        check_equal(atomic_load(&probe.cancelled), 2);
        check_equal(atomic_load(&probe.finalized), 2);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        cflow_executor_destroy(&executor);
    }

    it("drains accepted work after closing admission") {
        cflow_executor executor = {0};
        cflow_executor_control control = {0};
        owner_test_probe probe = {0};
        probe.executor = &executor;
        probe.owner_token = cmeta_thread_current_token();

        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 2u, NULL, NULL));
        check_true(cflow_executor_as_control(&executor, &control));
        check_equal(cflow_executor_try_post(
            &executor, owner_count, &probe), CFLOW_ADMISSION_ACCEPTED);
        check_equal(cflow_executor_try_post(
            &executor, owner_count, &probe), CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_control_shutdown(
            &control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN));
        check_equal(cflow_executor_run_ready(&executor), (size_t)2u);
        check_equal(atomic_load(&probe.count), 2);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        cflow_executor_destroy(&executor);
    }
}
