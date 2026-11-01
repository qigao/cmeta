#include <cflow/executor.h>
#include <cflow/scheduler.h>
#include <thread.h>
#include "scheduler_internal.h"
#include <tinytest.h>

#include <stdatomic.h>
#include <stdint.h>

enum { SCHED_TEST_PRODUCERS = 4, SCHED_TEST_POSTS = 8 };

typedef struct owner_scheduler_probe {
    const void *owner;
    atomic_int runs;
    atomic_int wrong_owner;
    atomic_int cancels;
    atomic_int finalizers;
    atomic_int wakes;
    atomic_int cancel_off_owner;
} owner_scheduler_probe;

typedef struct owner_scheduler_sender {
    cflow_scheduler *scheduler;
    owner_scheduler_probe *probe;
    int count;
    atomic_int rejected;
    atomic_int foreign_drive;
} owner_scheduler_sender;

typedef struct owner_cancel_sender {
    cflow_scheduler *scheduler;
    cflow_task_id task_id;
    atomic_bool accepted;
} owner_cancel_sender;

static void owner_sched_signal(void *user) {
    owner_scheduler_probe *probe = (owner_scheduler_probe *)user;
    atomic_fetch_add(&probe->wakes, 1);
}

static void owner_sched_run(void *user) {
    owner_scheduler_probe *probe = (owner_scheduler_probe *)user;
    if (probe->owner != cmeta_thread_current_token())
        atomic_fetch_add(&probe->wrong_owner, 1);
    atomic_fetch_add(&probe->runs, 1);
}

static void owner_sched_cancel(void *user) {
    owner_scheduler_probe *probe = (owner_scheduler_probe *)user;
    if (probe->owner != cmeta_thread_current_token())
        atomic_fetch_add(&probe->cancel_off_owner, 1);
    atomic_fetch_add(&probe->cancels, 1);
}

static void owner_sched_finalize(void *user) {
    owner_scheduler_probe *probe = (owner_scheduler_probe *)user;
    atomic_fetch_add(&probe->finalizers, 1);
}

static void owner_sched_produce(void *user) {
    owner_scheduler_sender *p = (owner_scheduler_sender *)user;
    for (int i = 0; i < p->count; ++i) {
        cflow_schedule_result admitted = cflow_scheduler_try_post_after(
            p->scheduler, 0u, owner_sched_run, p->probe);
        if (admitted.status != CFLOW_ADMISSION_ACCEPTED)
            atomic_fetch_add(&p->rejected, 1);
    }
    if (cflow_scheduler_run_one(p->scheduler))
        atomic_fetch_add(&p->foreign_drive, 1);
}

static void owner_sched_cancel_foreign(void *user) {
    owner_cancel_sender *p = (owner_cancel_sender *)user;
    atomic_store(&p->accepted,
                 cflow_scheduler_cancel(p->scheduler, p->task_id));
}

typedef struct owner_blocked_cancel {
    owner_scheduler_probe probe;
    atomic_bool cancel_entered;
    atomic_bool cancel_release;
} owner_blocked_cancel;

static void owner_blocked_run(void *user) {
    owner_blocked_cancel *p = (owner_blocked_cancel *)user;
    owner_sched_run(&p->probe);
}

static void owner_blocked_cancel_fn(void *user) {
    owner_blocked_cancel *p = (owner_blocked_cancel *)user;
    atomic_store(&p->cancel_entered, true);
    while (!atomic_load(&p->cancel_release)) cmeta_thread_yield();
    owner_sched_cancel(&p->probe);
}

static void owner_blocked_finalize(void *user) {
    owner_blocked_cancel *p = (owner_blocked_cancel *)user;
    owner_sched_finalize(&p->probe);
}

enum { OWNER_CANCEL_RACE_TASKS = 32 };

typedef struct owner_cancel_race {
    cflow_scheduler *scheduler;
    cflow_task_id ids[OWNER_CANCEL_RACE_TASKS];
    atomic_bool started;
    atomic_bool done;
    atomic_int won;
} owner_cancel_race;

static void owner_sched_cancel_race_foreign(void *user) {
    owner_cancel_race *r = (owner_cancel_race *)user;
    while (!atomic_load(&r->started)) cmeta_thread_yield();
    for (size_t i = 0u; i < OWNER_CANCEL_RACE_TASKS; ++i) {
        if (cflow_scheduler_cancel(r->scheduler, r->ids[i]))
            atomic_fetch_add(&r->won, 1);
    }
    atomic_store(&r->done, true);
}

typedef struct owner_queue_order {
    atomic_int count;
    int seen[4];
    const void *owner;
    atomic_int wrong_owner;
} owner_queue_order;

typedef struct owner_order_task {
    owner_queue_order *probe;
    int value;
} owner_order_task;

static void owner_sched_record(void *user) {
    owner_order_task *item = (owner_order_task *)user;
    owner_queue_order *p = item->probe;
    if (p->owner != cmeta_thread_current_token())
        atomic_fetch_add(&p->wrong_owner, 1);
    const int index = atomic_fetch_add(&p->count, 1);
    if (index >= 0 && index < 4) p->seen[index] = item->value;
}

suite("CFlow shared-owner Concurrent Scheduler") {
    it("rejects unsupported bindings and shares the existing Executor queue") {
        cflow_executor executor = {0};
        cflow_executor manual = {0};
        cflow_scheduler scheduler = {0};
        cflow_schedule_result result;
        owner_scheduler_probe probe = {0};

        probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_manual_init_with_capacity(&manual, 8u));
        check_false(cflow_scheduler_owner_bind(&scheduler, &manual, 4u));
        check_false(cflow_scheduler_valid(&scheduler));
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 8u, owner_sched_signal, &probe));
        check_false(cflow_scheduler_owner_bind(&scheduler, &executor, 0u));
        check_false(cflow_scheduler_owner_bind(&scheduler, &executor, 9u));
        check_true(cflow_scheduler_owner_bind(&scheduler, &executor, 4u));
        check_false(cflow_scheduler_owner_bind(&scheduler, &executor, 4u));
        check_true((cflow_scheduler_capabilities(&scheduler) &
                    CMETA_SCHED_CAP_CONCURRENT) != 0u);
        check_true((cflow_scheduler_capabilities(&scheduler) &
                    CMETA_SCHED_CAP_CALLER_DRIVEN_ZERO_DELAY) != 0u);
        check_false((cflow_scheduler_capabilities(&scheduler) &
                     CMETA_SCHED_CAP_DELAYED) != 0u);

        result = cflow_scheduler_try_post_after(
            &scheduler, 1u, owner_sched_run, &probe);
        check_equal(result.status, CFLOW_ADMISSION_INVALID_ARGUMENT);
        check_equal(result.task_id, (cflow_task_id)0u);
        result = cflow_scheduler_try_post_after(
            &scheduler, 0u, owner_sched_run, &probe);
        check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
        check_not_equal(result.task_id, (cflow_task_id)0u);
        check_equal(atomic_load(&probe.runs), 0);
        check_equal(cflow_executor_pending(&executor), (size_t)1u);
        check_equal(cflow_scheduler_pending(&scheduler), (size_t)1u);
        check_equal(cflow_scheduler_run_until_idle(&scheduler, 1u), (size_t)1u);
        check_equal(atomic_load(&probe.runs), 1);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        check_true(cflow_scheduler_wait_idle(&scheduler));

        cflow_scheduler_destroy(&scheduler);
        check_false(cflow_scheduler_valid(&scheduler));
        check_true(cflow_executor_try_post(
            &executor, owner_sched_run, &probe) == CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_run_one(&executor));
        check_equal(atomic_load(&probe.runs), 2);
        cflow_executor_destroy(&executor);
        cflow_executor_destroy(&manual);
    }

    it("shares one FIFO Executor with Machine work and other owner Schedulers") {
        cflow_executor executor = {0};
        cflow_scheduler one = {0};
        cflow_scheduler two = {0};
        owner_queue_order probe = {0};
        owner_order_task first = {&probe, 1};
        owner_order_task second = {&probe, 2};
        owner_order_task third = {&probe, 3};

        probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 8u, NULL, NULL));
        check_true(cflow_scheduler_owner_bind(&one, &executor, 4u));
        check_true(cflow_scheduler_owner_bind(&two, &executor, 4u));

        check_not_equal(cflow_scheduler_post(
            &one, owner_sched_record, &first), (cflow_task_id)0u);
        check_equal(cflow_executor_try_post(
            &executor, owner_sched_record, &second), CFLOW_ADMISSION_ACCEPTED);
        check_not_equal(cflow_scheduler_post(
            &two, owner_sched_record, &third), (cflow_task_id)0u);
        check_equal(cflow_executor_pending(&executor), (size_t)3u);
        check_equal(cflow_scheduler_pending(&one), (size_t)1u);
        check_equal(cflow_scheduler_pending(&two), (size_t)1u);

        /* Draining through either borrowed Scheduler progresses the shared
         * owner queue; it is not another callback-dispatch or mailbox loop. */
        check_equal(cflow_scheduler_run_until_idle(&two, 3u), (size_t)3u);
        check_equal(atomic_load(&probe.count), 3);
        check_equal(probe.seen[0], 1);
        check_equal(probe.seen[1], 2);
        check_equal(probe.seen[2], 3);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        check_true(cflow_scheduler_wait_idle(&one));
        check_true(cflow_scheduler_wait_idle(&two));
        cflow_scheduler_destroy(&two);
        cflow_scheduler_destroy(&one);
        cflow_executor_destroy(&executor);
    }

    it("accepts concurrent producers and only the owner dispatches callbacks") {
        cflow_executor executor = {0};
        cflow_scheduler scheduler = {0};
        owner_scheduler_probe probe = {0};
        cmeta_thread_t producers[SCHED_TEST_PRODUCERS] = {0};
        owner_scheduler_sender senders[SCHED_TEST_PRODUCERS] = {0};
        cflow_scheduler_stats stats = {0};
        int i;

        probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, SCHED_TEST_PRODUCERS * SCHED_TEST_POSTS,
            owner_sched_signal, &probe));
        check_true(cflow_scheduler_owner_bind(
            &scheduler, &executor, SCHED_TEST_PRODUCERS * SCHED_TEST_POSTS));

        for (i = 0; i < SCHED_TEST_PRODUCERS; ++i) {
            senders[i].scheduler = &scheduler;
            senders[i].probe = &probe;
            senders[i].count = SCHED_TEST_POSTS;
            check_equal(cmeta_thread_create(
                &producers[i], owner_sched_produce, &senders[i]), 0);
        }
        for (i = 0; i < SCHED_TEST_PRODUCERS; ++i)
            check_equal(cmeta_thread_join(&producers[i]), 0);
        for (i = 0; i < SCHED_TEST_PRODUCERS; ++i) {
            check_equal(atomic_load(&senders[i].rejected), 0);
            check_equal(atomic_load(&senders[i].foreign_drive), 0);
        }
        check_equal(atomic_load(&probe.runs), 0);
        check_equal(cflow_scheduler_pending(&scheduler),
                    (size_t)(SCHED_TEST_PRODUCERS * SCHED_TEST_POSTS));
        check_equal(cflow_scheduler_try_post_after(
            &scheduler, 0u, owner_sched_run, &probe).status,
            CFLOW_ADMISSION_FULL);
        check_equal(cflow_scheduler_run_until_idle(&scheduler, 7u), (size_t)7u);
        check_equal(atomic_load(&probe.runs), 7);
        check_equal(cflow_scheduler_run_until_idle(&scheduler, 0u),
                    (size_t)(SCHED_TEST_PRODUCERS * SCHED_TEST_POSTS - 7));
        check_equal(atomic_load(&probe.runs),
                    SCHED_TEST_PRODUCERS * SCHED_TEST_POSTS);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        check_true(cflow_scheduler_get_stats(&scheduler, &stats));
        check_equal(stats.ready_capacity,
                    (size_t)(SCHED_TEST_PRODUCERS * SCHED_TEST_POSTS));
        check_equal(stats.rejected_full, (size_t)1u);
        check_equal(stats.ready_pending, (size_t)0u);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        cflow_scheduler_destroy(&scheduler);
        cflow_executor_destroy(&executor);
    }

    it("settles a foreign-thread cancel immediately and retains bounded tombstone") {
        cflow_executor executor = {0};
        cflow_scheduler scheduler = {0};
        owner_scheduler_probe probe = {0};
        owner_cancel_sender cancel = {0};
        cmeta_thread_t canceller = NULL;
        cflow_schedule_result posted;
        cflow_executor_task task = {
            .run = owner_sched_run,
            .cancel = owner_sched_cancel,
            .finalize = owner_sched_finalize,
            .user = &probe
        };

        probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 2u, NULL, NULL));
        check_true(cflow_scheduler_owner_bind(&scheduler, &executor, 1u));
        posted = (cflow_schedule_result){0};
        check_true(cflow_scheduler_try_post_task_after_internal(
            &scheduler, 0u, &task, &posted));
        check_equal(posted.status, CFLOW_ADMISSION_ACCEPTED);
        cancel.scheduler = &scheduler;
        cancel.task_id = posted.task_id;
        check_equal(cmeta_thread_create(
            &canceller, owner_sched_cancel_foreign, &cancel), 0);
        check_equal(cmeta_thread_join(&canceller), 0);
        check_true(atomic_load(&cancel.accepted));
        check_false(cflow_scheduler_cancel(&scheduler, posted.task_id));
        check_equal(atomic_load(&probe.runs), 0);
        check_equal(atomic_load(&probe.cancels), 1);
        check_equal(atomic_load(&probe.finalizers), 1);
        check_equal(atomic_load(&probe.cancel_off_owner), 1);

        check_equal(cflow_scheduler_pending(&scheduler), (size_t)1u);
        check_equal(cflow_scheduler_try_post_after(
            &scheduler, 0u, owner_sched_run, &probe).status,
            CFLOW_ADMISSION_FULL);
        check_equal(cflow_scheduler_run_until_idle(&scheduler, 1u), (size_t)1u);
        check_equal(cflow_scheduler_pending(&scheduler), (size_t)0u);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        check_equal(atomic_load(&probe.cancels), 1);
        check_equal(atomic_load(&probe.finalizers), 1);
        cflow_scheduler_destroy(&scheduler);
        cflow_executor_destroy(&executor);
    }

    it("keeps a cancellation slot occupied until the external callback completes") {
        cflow_executor executor = {0};
        cflow_scheduler scheduler = {0};
        owner_blocked_cancel probe = {0};
        owner_cancel_sender cancel = {0};
        cmeta_thread_t canceller = NULL;
        cflow_schedule_result first = {0};
        const cflow_executor_task task = {
            .run = owner_blocked_run,
            .cancel = owner_blocked_cancel_fn,
            .finalize = owner_blocked_finalize,
            .user = &probe
        };

        probe.probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 2u, NULL, NULL));
        check_true(cflow_scheduler_owner_bind(&scheduler, &executor, 1u));
        check_true(cflow_scheduler_try_post_task_after_internal(
            &scheduler, 0u, &task, &first));
        check_equal(first.status, CFLOW_ADMISSION_ACCEPTED);

        cancel.scheduler = &scheduler;
        cancel.task_id = first.task_id;
        check_equal(cmeta_thread_create(
            &canceller, owner_sched_cancel_foreign, &cancel), 0);
        for (int tries = 0; tries < 5000 &&
                 !atomic_load(&probe.cancel_entered); ++tries)
            cmeta_sleep_ms(1u);
        check_true(atomic_load(&probe.cancel_entered));

        /* The owner consumes the Executor tombstone while cancellation is
         * still blocked on a foreign thread. The slot must not be recycled. */
        check_true(cflow_executor_run_one(&executor));
        check_equal(cflow_scheduler_pending(&scheduler), (size_t)1u);
        check_false(cflow_scheduler_wait_idle(&scheduler));
        check_equal(cflow_scheduler_try_post_after(
            &scheduler, 0u, owner_sched_run, &probe.probe).status,
            CFLOW_ADMISSION_FULL);

        atomic_store(&probe.cancel_release, true);
        check_equal(cmeta_thread_join(&canceller), 0);
        check_true(atomic_load(&cancel.accepted));
        check_equal(atomic_load(&probe.probe.cancels), 1);
        check_equal(atomic_load(&probe.probe.finalizers), 1);
        check_equal(atomic_load(&probe.probe.runs), 0);
        check_equal(cflow_scheduler_pending(&scheduler), (size_t)0u);

        check_equal(cflow_scheduler_try_post_after(
            &scheduler, 0u, owner_sched_run, &probe.probe).status,
            CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_run_one(&executor));
        check_equal(atomic_load(&probe.probe.runs), 1);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        cflow_scheduler_destroy(&scheduler);
        cflow_executor_destroy(&executor);
    }

    it("settles racing owner runs and foreign cancellation exactly once") {
        cflow_executor executor = {0};
        cflow_scheduler scheduler = {0};
        owner_scheduler_probe probe = {0};
        owner_cancel_race race = {0};
        cmeta_thread_t canceller = NULL;
        cflow_executor_task task = {
            .run = owner_sched_run,
            .cancel = owner_sched_cancel,
            .finalize = owner_sched_finalize,
            .user = &probe
        };

        probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, OWNER_CANCEL_RACE_TASKS, NULL, NULL));
        check_true(cflow_scheduler_owner_bind(
            &scheduler, &executor, OWNER_CANCEL_RACE_TASKS));

        race.scheduler = &scheduler;
        for (size_t i = 0u; i < OWNER_CANCEL_RACE_TASKS; ++i) {
            cflow_schedule_result posted = {0};
            check_true(cflow_scheduler_try_post_task_after_internal(
                &scheduler, 0u, &task, &posted));
            check_equal(posted.status, CFLOW_ADMISSION_ACCEPTED);
            race.ids[i] = posted.task_id;
        }
        check_equal(cmeta_thread_create(
            &canceller, owner_sched_cancel_race_foreign, &race), 0);
        atomic_store(&race.started, true);
        while (!atomic_load(&race.done) ||
               cflow_scheduler_pending(&scheduler) != 0u) {
            if (!cflow_executor_run_one(&executor))
                cmeta_thread_yield();
        }
        check_equal(cmeta_thread_join(&canceller), 0);
        check_true(cflow_scheduler_wait_idle(&scheduler));
        check_equal(atomic_load(&probe.runs) +
                    atomic_load(&probe.cancels), OWNER_CANCEL_RACE_TASKS);
        check_equal(atomic_load(&probe.finalizers), OWNER_CANCEL_RACE_TASKS);
        check_equal(atomic_load(&probe.wrong_owner), 0);
        check_equal(atomic_load(&probe.cancel_off_owner),
                    atomic_load(&race.won));

        cflow_scheduler_destroy(&scheduler);
        cflow_executor_destroy(&executor);
    }

    it("does not shut down the borrowed Executor when Scheduler admission seals") {
        cflow_executor executor = {0};
        cflow_scheduler scheduler = {0};
        owner_scheduler_probe probe = {0};
        probe.owner = cmeta_thread_current_token();
        check_true(cflow_executor_owner_init_with_capacity(
            &executor, 4u, NULL, NULL));
        check_true(cflow_scheduler_owner_bind(&scheduler, &executor, 2u));
        check_not_equal(cflow_scheduler_post(
            &scheduler, owner_sched_run, &probe), (cflow_task_id)0u);
        check_true(cflow_scheduler_shutdown(&scheduler));
        check_true(cflow_scheduler_shutdown(&scheduler));
        check_equal(cflow_scheduler_try_post_after(
            &scheduler, 0u, owner_sched_run, &probe).status,
            CFLOW_ADMISSION_CLOSED);
        check_equal(cflow_scheduler_run_until_idle(&scheduler, 1u), (size_t)1u);
        check_equal(atomic_load(&probe.runs), 1);
        check_true(cflow_executor_try_post(
            &executor, owner_sched_run, &probe) == CFLOW_ADMISSION_ACCEPTED);
        check_true(cflow_executor_run_one(&executor));
        check_equal(atomic_load(&probe.runs), 2);
        cflow_scheduler_destroy(&scheduler);
        cflow_executor_destroy(&executor);
    }
}
