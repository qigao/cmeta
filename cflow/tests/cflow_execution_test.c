#include <cflow/clock.h>
#include <cflow/executor.h>
#include <cflow/scheduler.h>
#include <cflow/time.h>
#include <thread.h>
#include "scheduler_internal.h"
#include "timer_queue.h"
#include "tinytest.h"

#include <stdatomic.h>

extern bool cflow_executor_is_current_internal(const cflow_executor *executor);

static atomic_int executor_counter;
static atomic_int active_callbacks;
static atomic_int max_active_callbacks;
static atomic_int producer_failures;
static atomic_int worker_gate_open;
static atomic_int worker_gate_started;

typedef struct executor_control_probe {
  cflow_executor_control *control;
  cflow_executor_post_status first_post;
  cflow_executor_post_status second_post;
  cflow_executor_wait_status wait;
} executor_control_probe;

typedef struct cflow_task_lifecycle_probe {
  atomic_int run_count;
  atomic_int cancel_count;
  atomic_int finalize_count;
  atomic_int finalize_entered;
  atomic_int finalize_gate;
  atomic_int sequence;
  atomic_int run_sequence;
  atomic_int cancel_sequence;
  atomic_int finalize_sequence;
} cflow_task_lifecycle_probe;

typedef struct cflow_wait_probe {
  cflow_executor_control *control;
  atomic_int entered;
  atomic_int done;
  cflow_executor_wait_status status;
} cflow_wait_probe;

typedef struct cflow_current_probe {
  cflow_executor *executor;
  atomic_int seen_current;
} cflow_current_probe;

static void count_task(void *user) {
  (void)user;
  atomic_fetch_add(&executor_counter, 1);
}

static void gated_count_task(void *user) {
  (void)user;
  atomic_store(&worker_gate_started, 1);
  while (!atomic_load(&worker_gate_open)) cmeta_sleep_ms(1);
  atomic_fetch_add(&executor_counter, 1);
}

static void cflow_lifecycle_run(void *user) {
  cflow_task_lifecycle_probe *probe = (cflow_task_lifecycle_probe *)user;
  atomic_fetch_add(&probe->run_count, 1);
  atomic_store(&probe->run_sequence,
               atomic_fetch_add(&probe->sequence, 1) + 1);
}

static void cflow_lifecycle_cancel(void *user) {
  cflow_task_lifecycle_probe *probe = (cflow_task_lifecycle_probe *)user;
  atomic_fetch_add(&probe->cancel_count, 1);
  atomic_store(&probe->cancel_sequence,
               atomic_fetch_add(&probe->sequence, 1) + 1);
}

static void cflow_lifecycle_finalize(void *user) {
  cflow_task_lifecycle_probe *probe = (cflow_task_lifecycle_probe *)user;
  atomic_fetch_add(&probe->finalize_count, 1);
  atomic_store(&probe->finalize_sequence,
               atomic_fetch_add(&probe->sequence, 1) + 1);
}

static void cflow_blocking_lifecycle_finalize(void *user) {
  cflow_task_lifecycle_probe *probe = (cflow_task_lifecycle_probe *)user;
  cflow_lifecycle_finalize(user);
  atomic_store(&probe->finalize_entered, 1);
  while (!atomic_load(&probe->finalize_gate)) cmeta_sleep_ms(1);
}

static void cflow_wait_for_executor(void *user) {
  cflow_wait_probe *probe = (cflow_wait_probe *)user;
  atomic_store(&probe->entered, 1);
  probe->status = cflow_executor_control_wait_idle(probe->control);
  atomic_store(&probe->done, 1);
}

static void cflow_check_current_executor(void *user) {
  cflow_current_probe *probe = (cflow_current_probe *)user;
  atomic_store(&probe->seen_current,
               cflow_executor_is_current_internal(probe->executor) ? 1 : -1);
}

static void serial_probe(void *user) {
  int active;
  int seen;
  (void)user;

  active = atomic_fetch_add(&active_callbacks, 1) + 1;
  seen = atomic_load(&max_active_callbacks);
  while (active > seen &&
         !atomic_compare_exchange_weak(&max_active_callbacks, &seen, active)) {
  }
  cmeta_sleep_ms(1);
  atomic_fetch_add(&executor_counter, 1);
  atomic_fetch_sub(&active_callbacks, 1);
}

static void executor_control_callback(void *user) {
  executor_control_probe *probe = (executor_control_probe *)user;
  probe->first_post = cflow_executor_control_post(
      probe->control, count_task, NULL);
  probe->second_post = cflow_executor_control_post(
      probe->control, count_task, NULL);
  probe->wait = cflow_executor_control_wait_idle(probe->control);
}

typedef struct executor_producer_ctx {
  cflow_executor *executor;
  int count;
} executor_producer_ctx;

static void executor_producer(void *user) {
  executor_producer_ctx *ctx = (executor_producer_ctx *)user;
  for (int i = 0; i < ctx->count; ++i) {
    if (!cflow_executor_post(ctx->executor, serial_probe, NULL))
      atomic_fetch_add(&producer_failures, 1);
  }
}

spec("CFlow execution foundation") {
  it("saturates deadline arithmetic") {
    cflow_instant now = {UINT64_MAX - 5u};
    cflow_duration delay = cflow_duration_from_ns(10u);
    cflow_deadline deadline = cflow_deadline_after(now, delay);
    check_equal(deadline.ns, UINT64_MAX);
  }

  it("saturates duration unit conversion") {
    check_equal(cflow_duration_from_s(UINT64_MAX).ns, UINT64_MAX);
  }

  it("advances virtual time exactly") {
    cflow_clock clock = {0};
    check_true(cflow_clock_virtual_init(&clock, (cflow_instant){100u}));
    check_equal(cflow_clock_now(&clock).ns, 100u);
    check_true(cflow_clock_advance(&clock, cflow_duration_from_ns(25u)));
    check_equal(cflow_clock_now(&clock).ns, 125u);
    cflow_clock_destroy(&clock);
  }

  it("does not manually advance system time") {
    cflow_clock clock = {0};
    check_true(cflow_clock_system_init(&clock));
    check(cflow_clock_now(&clock).ns > 0u);
    check_false(cflow_clock_advance(&clock, cflow_duration_from_ns(1u)));
    cflow_clock_destroy(&clock);
  }

  it("keeps ManualExecutor explicitly driven") {
    cflow_executor executor = {0};
    atomic_store(&executor_counter, 0);

    check_true(cflow_executor_manual_init(&executor));
    check_equal(cflow_executor_try_post(&executor, NULL, NULL),
                CFLOW_ADMISSION_INVALID_ARGUMENT);
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_ACCEPTED);
    check_equal(cflow_executor_pending(&executor), (size_t)1u);
    check_false(cflow_executor_wait_idle(&executor));
    check_equal(atomic_load(&executor_counter), 0);

    check_true(cflow_executor_run_one(&executor));
    check_equal(atomic_load(&executor_counter), 1);
    check_equal(cflow_executor_pending(&executor), (size_t)0u);
    check_true(cflow_executor_wait_idle(&executor));
    cflow_executor_destroy(&executor);
  }

  it("bounds ManualExecutor storage and reuses released capacity") {
    cflow_executor executor = {0};
    cflow_executor_stats stats = {0};
    const void *task_storage;

    check_false(cflow_executor_manual_init_with_capacity(&executor, 0u));
    check_false(cflow_executor_valid(&executor));
    check_false(cflow_executor_manual_init_with_capacity(&executor, SIZE_MAX));
    check_false(cflow_executor_valid(&executor));

    check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
    task_storage = *(void *const *)executor.self;
    check(task_storage != NULL);
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_ACCEPTED);
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_FULL);
    check_false(cflow_executor_post(&executor, count_task, NULL));
    check_true(cflow_executor_get_stats(&executor, &stats));
    check_equal(stats.capacity, (size_t)1u);
    check_equal(stats.pending, (size_t)1u);
    check_equal(stats.peak_pending, (size_t)1u);
    check_equal(stats.rejected_full, (size_t)2u);
    check_true(cflow_executor_run_one(&executor));
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_ACCEPTED);
    check(*(void *const *)executor.self == task_storage);
    check_true(cflow_executor_run_one(&executor));
    check_true(cflow_executor_shutdown(&executor));
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_CLOSED);
    check_true(cflow_executor_get_stats(&executor, &stats));
    check_equal(stats.rejected_closed, (size_t)1u);
    cflow_executor_destroy(&executor);
  }

  it("clears an Executor handle after destroy") {
    cflow_executor executor = {0};
    bool cleared;

    check_true(cflow_executor_manual_init(&executor));
    cflow_executor_destroy(&executor);
    cleared = !cflow_executor_valid(&executor) && executor.self == NULL &&
              executor.vtable == NULL;
    check_true(cleared);
    if (cleared) cflow_executor_destroy(&executor);
  }

  it("preserves a live Executor when reinitialization is rejected") {
    cflow_executor executor = {0};
    cflow_executor original;

    check_true(cflow_executor_manual_init(&executor));
    original = executor;
    check_false(cflow_executor_manual_init_with_capacity(&executor, 2u));
    check_false(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check(executor.self == original.self);
    check(executor.vtable == original.vtable);
    if (executor.self != original.self || executor.vtable != original.vtable)
      executor = original;
    cflow_executor_destroy(&executor);
    check_true(cflow_executor_manual_init_with_capacity(&executor, 2u));
    cflow_executor_destroy(&executor);
  }

  it("preserves a live WorkerExecutor when reinitialization is rejected") {
    cflow_executor executor = {0};
    cflow_executor original;

    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    original = executor;
    check_false(cflow_executor_worker_init_with_capacity(&executor, 2u, 2u));
    check_false(cflow_executor_serial_init_with_capacity(&executor, 2u));
    check(executor.self == original.self);
    check(executor.vtable == original.vtable);
    if (executor.self != original.self || executor.vtable != original.vtable)
      executor = original;
    cflow_executor_destroy(&executor);
  }

  it("preserves a live test Scheduler when reinitialization is rejected") {
    cflow_scheduler scheduler = {0};
    cflow_scheduler original;

    check_true(cflow_scheduler_test_init_with_capacity(&scheduler, 2u, 2u));
    original = scheduler;
    check_false(cflow_scheduler_test_init_with_capacity(&scheduler, 4u, 4u));
    check_true(scheduler.self == original.self);
    check_true(scheduler.vtable == original.vtable);
    if (scheduler.self != original.self || scheduler.vtable != original.vtable) {
      cflow_scheduler_destroy(&scheduler);
      scheduler = original;
    }
    check(cflow_scheduler_post(&scheduler, count_task, NULL) != 0u);
    check_equal(cflow_scheduler_run_until_idle(&scheduler, 0u), (size_t)1u);
    cflow_scheduler_destroy(&scheduler);
  }

  it("preserves a live worker Scheduler when reinitialization is rejected") {
    cflow_scheduler scheduler = {0};
    cflow_scheduler original;

    check_true(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 1u, 2u, 2u));
    original = scheduler;
    check_false(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 2u, 4u, 4u));
    check_true(scheduler.self == original.self);
    check_true(scheduler.vtable == original.vtable);
    if (scheduler.self != original.self || scheduler.vtable != original.vtable) {
      cflow_scheduler_destroy(&scheduler);
      scheduler = original;
    }
    check(cflow_scheduler_post(&scheduler, count_task, NULL) != 0u);
    check_true(cflow_scheduler_wait_idle(&scheduler));
    cflow_scheduler_destroy(&scheduler);
  }

  it("serializes callbacks from concurrent producers") {
    enum { PRODUCERS = 4, TASKS_PER_PRODUCER = 8 };
    cflow_executor executor = {0};
    cmeta_thread_t producers[PRODUCERS];
    executor_producer_ctx contexts[PRODUCERS];

    atomic_store(&executor_counter, 0);
    atomic_store(&active_callbacks, 0);
    atomic_store(&max_active_callbacks, 0);
    atomic_store(&producer_failures, 0);

    check_true(cflow_executor_serial_init(&executor));
    for (int i = 0; i < PRODUCERS; ++i) {
      contexts[i].executor = &executor;
      contexts[i].count = TASKS_PER_PRODUCER;
      check_equal(cmeta_thread_create(&producers[i], executor_producer,
                                      &contexts[i]), 0);
    }
    for (int i = 0; i < PRODUCERS; ++i)
      check_equal(cmeta_thread_join(&producers[i]), 0);

    check_true(cflow_executor_wait_idle(&executor));
    check_equal(atomic_load(&producer_failures), 0);
    check_equal(atomic_load(&executor_counter), PRODUCERS * TASKS_PER_PRODUCER);
    check_equal(atomic_load(&max_active_callbacks), 1);
    cflow_executor_destroy(&executor);
  }

  it("runs work through WorkerExecutor") {
    cflow_executor executor = {0};
    atomic_store(&executor_counter, 0);

    check_true(cflow_executor_worker_init(&executor, 2u));
    check_true(cflow_executor_post(&executor, count_task, NULL));
    check_true(cflow_executor_post(&executor, count_task, NULL));
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(atomic_load(&executor_counter), 2);
    cflow_executor_destroy(&executor);
  }

  it("dispatches WorkerExecutor protocol through provider vtables") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_vtable foreign_executor_vtable;
    cflow_executor_control_vtable foreign_control_vtable;
    cflow_task_lifecycle_probe lifecycle = {0};
    cflow_current_probe current = {
        .executor = &executor,
    };
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &lifecycle,
    };

    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 4u));
    foreign_executor_vtable = *executor.vtable;
    executor.vtable = &foreign_executor_vtable;

    check_true(cflow_executor_as_control(&executor, &control));
    foreign_control_vtable = *control.vtable;
    control.vtable = &foreign_control_vtable;

    check_equal(cflow_executor_try_post_task(&executor, &task),
                CFLOW_ADMISSION_ACCEPTED);
    check_equal(cflow_executor_control_post_task(&control, &task),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_control_post(
                    &control, cflow_check_current_executor, &current),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);

    check_equal(atomic_load(&lifecycle.run_count), 2);
    check_equal(atomic_load(&lifecycle.cancel_count), 0);
    check_equal(atomic_load(&lifecycle.finalize_count), 2);
    check_equal(atomic_load(&current.seen_current), 1);
    cflow_executor_destroy(&executor);
  }

  it("reports WorkerExecutor saturation without growing its queue") {
    cflow_executor executor = {0};
    cflow_executor_stats stats = {0};
    int attempts = 0;

    check_false(cflow_executor_serial_init_with_capacity(&executor, 0u));
    check_false(cflow_executor_worker_init_with_capacity(&executor, 0u, 1u));
    check_false(cflow_executor_worker_init_with_capacity(&executor, 1u, 0u));
    atomic_store(&executor_counter, 0);
    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check_true(cflow_executor_post(&executor, gated_count_task, NULL));
    while (!atomic_load(&worker_gate_started) && attempts++ < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_ACCEPTED);
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_FULL);
    check_true(cflow_executor_get_stats(&executor, &stats));
    check_equal(stats.capacity, (size_t)1u);
    check_equal(stats.pending, (size_t)2u);
    check_equal(stats.peak_pending, (size_t)2u);
    check_equal(stats.rejected_full, (size_t)1u);

    atomic_store(&worker_gate_open, 1);
    check_true(cflow_executor_wait_idle(&executor));
    check_equal(atomic_load(&executor_counter), 2);
    cflow_executor_destroy(&executor);
  }

  it("drains accepted ManualExecutor work after closing admission") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};

    atomic_store(&executor_counter, 0);
    check_true(cflow_executor_manual_init_with_capacity(&executor, 2u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_control_post(&control, count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_control_post(&control, count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN));
    check_equal(cflow_executor_try_post(&executor, count_task, NULL),
                CFLOW_ADMISSION_CLOSED);
    check_equal(cflow_executor_run_ready(&executor), (size_t)2u);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.lifecycle, CFLOW_EXECUTOR_CLOSED);
    check_equal(stats.accepted, (size_t)2u);
    check_equal(stats.completed, (size_t)2u);
    check_equal(stats.cancelled, (size_t)0u);
    check_equal(atomic_load(&executor_counter), 2);
    cflow_executor_destroy(&executor);
  }

  it("cancels pending ManualExecutor work without invoking callbacks") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};

    atomic_store(&executor_counter, 0);
    check_true(cflow_executor_manual_init_with_capacity(&executor, 2u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_control_post(&control, count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_control_post(&control, count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    check_equal(cflow_executor_run_ready(&executor), (size_t)0u);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.accepted, (size_t)2u);
    check_equal(stats.completed, (size_t)0u);
    check_equal(stats.cancelled, (size_t)2u);
    check_equal(stats.accepted, stats.completed + stats.cancelled);
    check_equal(atomic_load(&executor_counter), 0);
    cflow_executor_destroy(&executor);
  }

  it("runs and finalizes accepted ManualExecutor descriptor exactly once") {
    cflow_executor executor = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };

    check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
    check_equal(cflow_executor_try_post_task(&executor, &task),
                CFLOW_ADMISSION_ACCEPTED);
    check_true(cflow_executor_run_one(&executor));
    check_equal(atomic_load(&probe.run_count), 1);
    check_equal(atomic_load(&probe.cancel_count), 0);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.run_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_executor_destroy(&executor);
  }

  it("cancels and finalizes ManualExecutor descriptor exactly once") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };

    check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_control_post_task(&control, &task),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_equal(atomic_load(&probe.run_count), 0);
    check_equal(atomic_load(&probe.cancel_count), 1);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.cancel_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_executor_destroy(&executor);
  }

  it("cancels and finalizes pending ManualExecutor descriptor on destroy") {
    cflow_executor executor = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };

    check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
    check_equal(cflow_executor_try_post_task(&executor, &task),
                CFLOW_ADMISSION_ACCEPTED);
    cflow_executor_destroy(&executor);
    check_equal(atomic_load(&probe.run_count), 0);
    check_equal(atomic_load(&probe.cancel_count), 1);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.cancel_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
  }

  it("does not invoke ManualExecutor descriptor after full rejection") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_task_lifecycle_probe accepted_probe = {0};
    cflow_task_lifecycle_probe rejected_probe = {0};
    cflow_executor_task accepted = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &accepted_probe,
    };
    cflow_executor_task rejected = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &rejected_probe,
    };

    check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_try_post_task(&executor, &accepted),
                CFLOW_ADMISSION_ACCEPTED);
    check_equal(cflow_executor_try_post_task(&executor, &rejected),
                CFLOW_ADMISSION_FULL);
    check_equal(atomic_load(&rejected_probe.sequence), 0);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    check_equal(atomic_load(&accepted_probe.cancel_count), 1);
    check_equal(atomic_load(&accepted_probe.finalize_count), 1);
    check_equal(atomic_load(&rejected_probe.sequence), 0);
    cflow_executor_destroy(&executor);
  }

  it("rejects ManualExecutor callback self-blocking through control") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};
    executor_control_probe probe = {0};

    atomic_store(&executor_counter, 0);
    check_true(cflow_executor_manual_init_with_capacity(&executor, 1u));
    check_true(cflow_executor_as_control(&executor, &control));
    probe.control = &control;
    check_equal(cflow_executor_control_post(
                    &control, executor_control_callback, &probe),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_run_ready(&executor), (size_t)2u);
    check_equal(probe.first_post, CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(probe.second_post, CFLOW_EXECUTOR_POST_WOULD_BLOCK);
    check_equal(probe.wait, CFLOW_EXECUTOR_WAIT_WOULD_BLOCK);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.rejected_would_block, (size_t)2u);
    check_equal(atomic_load(&executor_counter), 1);
    cflow_executor_destroy(&executor);
  }

  it("rejects WorkerExecutor callback self-blocking through control") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    executor_control_probe probe = {0};

    atomic_store(&executor_counter, 0);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check_true(cflow_executor_as_control(&executor, &control));
    probe.control = &control;
    check_equal(cflow_executor_control_post(
                    &control, executor_control_callback, &probe),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_equal(probe.first_post, CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(probe.second_post, CFLOW_EXECUTOR_POST_WOULD_BLOCK);
    check_equal(probe.wait, CFLOW_EXECUTOR_WAIT_WOULD_BLOCK);
    {
      cflow_executor_protocol_stats stats = {0};
      check_true(cflow_executor_control_get_stats(&control, &stats));
      check_equal(stats.rejected_would_block, (size_t)2u);
    }
    check_equal(atomic_load(&executor_counter), 1);
    cflow_executor_destroy(&executor);
  }

  it("conserves terminal WorkerExecutor accounting after cancellation") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};
    int attempts = 0;

    atomic_store(&executor_counter, 0);
    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 2u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_control_post(
                    &control, gated_count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    while (!atomic_load(&worker_gate_started) && attempts++ < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);
    check_equal(cflow_executor_control_post(&control, count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_equal(cflow_executor_control_post(&control, count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    check_false(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN));
    atomic_store(&worker_gate_open, 1);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.lifecycle, CFLOW_EXECUTOR_CLOSED);
    check_equal(stats.accepted, (size_t)3u);
    check_equal(stats.queued, (size_t)0u);
    check_equal(stats.running, (size_t)0u);
    check_equal(stats.completed, (size_t)1u);
    check_equal(stats.cancelled, (size_t)2u);
    check_equal(stats.accepted, stats.completed + stats.cancelled);
    check_equal(atomic_load(&executor_counter), 1);
    cflow_executor_destroy(&executor);
  }

  it("cancels and finalizes queued WorkerExecutor descriptor exactly once") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };
    int attempts = 0;

    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_control_post(
                    &control, gated_count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    while (!atomic_load(&worker_gate_started) && attempts++ < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);
    check_equal(cflow_executor_control_post_task(&control, &task),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    atomic_store(&worker_gate_open, 1);
    check_equal(cflow_executor_control_wait_idle(&control),
                CFLOW_EXECUTOR_WAIT_IDLE);
    check_equal(atomic_load(&probe.run_count), 0);
    check_equal(atomic_load(&probe.cancel_count), 1);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.cancel_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_executor_destroy(&executor);
  }

  it("keeps WorkerExecutor cancellation observable until finalize returns") {
    cflow_executor executor = {0};
    cflow_executor_control control = {0};
    cflow_executor_protocol_stats stats = {0};
    cflow_task_lifecycle_probe lifecycle = {0};
    cflow_wait_probe waiter = {
        .control = &control,
        .status = CFLOW_EXECUTOR_WAIT_INVALID_ARGUMENT,
    };
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_blocking_lifecycle_finalize,
        .user = &lifecycle,
    };
    cmeta_thread_t wait_thread;
    int attempts = 0;

    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_executor_worker_init_with_capacity(&executor, 1u, 1u));
    check_true(cflow_executor_as_control(&executor, &control));
    check_equal(cflow_executor_control_post(
                    &control, gated_count_task, NULL),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    while (!atomic_load(&worker_gate_started) && ++attempts < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);
    check_equal(cflow_executor_control_post_task(&control, &task),
                CFLOW_EXECUTOR_POST_ACCEPTED);
    check_true(cflow_executor_control_shutdown(
        &control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));

    atomic_store(&worker_gate_open, 1);
    attempts = 0;
    while (!atomic_load(&lifecycle.finalize_entered) && ++attempts < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&lifecycle.finalize_entered), 1);

    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.lifecycle, CFLOW_EXECUTOR_CLOSING);
    check_equal(stats.accepted, (size_t)2u);
    check_equal(stats.queued, (size_t)1u);
    check_equal(stats.running, (size_t)0u);
    check_equal(stats.completed, (size_t)1u);
    check_equal(stats.cancelled, (size_t)0u);
    check_equal(stats.accepted,
                stats.queued + stats.running + stats.completed +
                    stats.cancelled);

    check_equal(cmeta_thread_create(
                    &wait_thread, cflow_wait_for_executor, &waiter),
                0);
    attempts = 0;
    while (!atomic_load(&waiter.entered) && ++attempts < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&waiter.entered), 1);
    cmeta_sleep_ms(10);
    check_equal(atomic_load(&waiter.done), 0);

    atomic_store(&lifecycle.finalize_gate, 1);
    cmeta_thread_join(&wait_thread);
    check_equal(waiter.status, CFLOW_EXECUTOR_WAIT_IDLE);
    check_equal(atomic_load(&waiter.done), 1);
    check_true(cflow_executor_control_get_stats(&control, &stats));
    check_equal(stats.lifecycle, CFLOW_EXECUTOR_CLOSED);
    check_equal(stats.queued, (size_t)0u);
    check_equal(stats.running, (size_t)0u);
    check_equal(stats.completed, (size_t)1u);
    check_equal(stats.cancelled, (size_t)1u);
    check_equal(stats.accepted, stats.completed + stats.cancelled);
    cflow_executor_destroy(&executor);
  }

  it("preserves a ready timer across saturated executor handoff") {
    cflow_scheduler scheduler = {0};
    cflow_scheduler_stats stats = {0};
    int attempts = 0;

    atomic_store(&executor_counter, 0);
    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 1u, 1u, 1u));
    check(cflow_scheduler_post(&scheduler, gated_count_task, NULL) != 0u);
    while (!atomic_load(&worker_gate_started) && attempts++ < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);

    check(cflow_scheduler_post(&scheduler, count_task, NULL) != 0u);
    attempts = 0;
    do {
      check_true(cflow_scheduler_get_stats(&scheduler, &stats));
      if (stats.ready_pending == 2u && stats.timer_pending == 0u) break;
      cmeta_sleep_ms(1);
    } while (attempts++ < 200);
    check_equal(stats.ready_pending, (size_t)2u);

    check(cflow_scheduler_post(&scheduler, count_task, NULL) != 0u);
    attempts = 0;
    do {
      check_true(cflow_scheduler_get_stats(&scheduler, &stats));
      if (stats.dispatching == 1u) break;
      cmeta_sleep_ms(1);
    } while (attempts++ < 200);
    check_equal(stats.dispatching, (size_t)1u);
    check_equal(stats.peak_pending, (size_t)3u);

    atomic_store(&worker_gate_open, 1);
    check_true(cflow_scheduler_wait_idle(&scheduler));
    check_equal(atomic_load(&executor_counter), 3);
    cflow_scheduler_destroy(&scheduler);
  }

  it("cancels a blocked timer handoff during shutdown") {
    cflow_scheduler scheduler = {0};
    cflow_scheduler_stats stats = {0};
    int attempts = 0;

    atomic_store(&executor_counter, 0);
    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 1u, 1u, 1u));
    check(cflow_scheduler_post(&scheduler, gated_count_task, NULL) != 0u);
    while (!atomic_load(&worker_gate_started) && attempts++ < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);
    check(cflow_scheduler_post(&scheduler, count_task, NULL) != 0u);
    attempts = 0;
    do {
      check_true(cflow_scheduler_get_stats(&scheduler, &stats));
      if (stats.ready_pending == 2u && stats.timer_pending == 0u) break;
      cmeta_sleep_ms(1);
    } while (attempts++ < 200);
    check(cflow_scheduler_post(&scheduler, count_task, NULL) != 0u);
    attempts = 0;
    do {
      check_true(cflow_scheduler_get_stats(&scheduler, &stats));
      if (stats.dispatching == 1u) break;
      cmeta_sleep_ms(1);
    } while (attempts++ < 200);
    check_equal(stats.dispatching, (size_t)1u);

    check_true(cflow_scheduler_shutdown(&scheduler));
    check_true(cflow_scheduler_get_stats(&scheduler, &stats));
    check_equal(stats.cancelled_on_shutdown, (size_t)1u);
    check_equal(stats.rejected_closed, (size_t)1u);
    atomic_store(&worker_gate_open, 1);
    cflow_scheduler_destroy(&scheduler);
    check_equal(atomic_load(&executor_counter), 2);
  }

  it("settles a test Scheduler descriptor exactly once on cancel") {
    cflow_scheduler scheduler = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };
    cflow_schedule_result result = {0};

    check_true(cflow_scheduler_test_init(&scheduler));
    check_true(cflow_scheduler_try_post_task_after_internal(
        &scheduler, 100u, &task, &result));
    check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
    check_true(cflow_scheduler_cancel(&scheduler, result.task_id));
    check_equal(atomic_load(&probe.run_count), 0);
    check_equal(atomic_load(&probe.cancel_count), 1);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.cancel_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_scheduler_destroy(&scheduler);
  }

  it("settles an Inline Scheduler descriptor before admission returns") {
    cflow_scheduler scheduler = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };
    cflow_schedule_result result = {0};

    check_true(cflow_scheduler_inline_init(&scheduler));
    check_true(cflow_scheduler_try_post_task_after_internal(
        &scheduler, 0u, &task, &result));
    check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
    check_not_equal(result.task_id, (cflow_task_id)0u);
    check_equal(atomic_load(&probe.run_count), 1);
    check_equal(atomic_load(&probe.cancel_count), 0);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.run_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_scheduler_destroy(&scheduler);
  }

  it("transfers immediate Worker Scheduler descriptors without a timer handoff") {
    cflow_scheduler scheduler = {0};
    cflow_scheduler_stats stats = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };
    cflow_schedule_result result = {0};
    int attempts = 0;

    atomic_store(&worker_gate_open, 0);
    atomic_store(&worker_gate_started, 0);
    check_true(cflow_scheduler_worker_init_with_capacity(&scheduler, 1u, 1u, 1u));
    check(cflow_scheduler_post(&scheduler, gated_count_task, NULL) != 0u);
    while (!atomic_load(&worker_gate_started) && attempts++ < 200)
      cmeta_sleep_ms(1);
    check_equal(atomic_load(&worker_gate_started), 1);

    check_true(cflow_scheduler_try_post_task_after_internal(
        &scheduler, 0u, &task, &result));
    check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
    check_not_equal(result.task_id, (cflow_task_id)0u);
    check_true(cflow_scheduler_get_stats(&scheduler, &stats));
    check_equal(stats.ready_pending, (size_t)2u);
    check_equal(stats.timer_pending, (size_t)0u);
    check_equal(stats.dispatching, (size_t)0u);
    check_false(cflow_scheduler_cancel(&scheduler, result.task_id));

    atomic_store(&worker_gate_open, 1);
    check_true(cflow_scheduler_wait_idle(&scheduler));
    check_equal(atomic_load(&probe.run_count), 1);
    check_equal(atomic_load(&probe.cancel_count), 0);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.run_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_scheduler_destroy(&scheduler);
  }

  it("settles a worker Scheduler descriptor exactly once on shutdown") {
    cflow_scheduler scheduler = {0};
    cflow_task_lifecycle_probe probe = {0};
    cflow_executor_task task = {
        .run = cflow_lifecycle_run,
        .cancel = cflow_lifecycle_cancel,
        .finalize = cflow_lifecycle_finalize,
        .user = &probe,
    };
    cflow_schedule_result result = {0};

    check_true(cflow_scheduler_worker_init(&scheduler, 1u));
    check_true(cflow_scheduler_try_post_task_after_internal(
        &scheduler, 60000u, &task, &result));
    check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
    check_true(cflow_scheduler_shutdown(&scheduler));
    check_equal(atomic_load(&probe.run_count), 0);
    check_equal(atomic_load(&probe.cancel_count), 1);
    check_equal(atomic_load(&probe.finalize_count), 1);
    check_equal(atomic_load(&probe.cancel_sequence), 1);
    check_equal(atomic_load(&probe.finalize_sequence), 2);
    cflow_scheduler_destroy(&scheduler);
  }

  it("orders TimerQueue by deadline then insertion order") {
    cflow_timer_queue queue;
    cflow_timer_task task;
    int a = 1;
    int b = 2;
    int c = 3;

    check_true(cflow_timer_queue_init(&queue));
    check(cflow_timer_queue_schedule(&queue, (cflow_deadline){20u}, count_task, &a) != 0u);
    check(cflow_timer_queue_schedule(&queue, (cflow_deadline){10u}, count_task, &b) != 0u);
    check(cflow_timer_queue_schedule(&queue, (cflow_deadline){10u}, count_task, &c) != 0u);

    check_true(cflow_timer_queue_take_ready(&queue, (cflow_instant){10u}, &task));
    check(task.user == &b);
    check_true(cflow_timer_queue_take_ready(&queue, (cflow_instant){10u}, &task));
    check(task.user == &c);
    check_false(cflow_timer_queue_take_ready(&queue, (cflow_instant){10u}, &task));
    check_true(cflow_timer_queue_take_ready(&queue, (cflow_instant){20u}, &task));
    check(task.user == &a);
    check_equal(cflow_timer_queue_pending(&queue), (size_t)0u);
    cflow_timer_queue_destroy(&queue);
  }

  it("cancels only pending TimerQueue entries") {
    cflow_timer_queue queue;
    cflow_timer_task task;
    int a = 1;
    int b = 2;
    cflow_timer_id a_id;
    cflow_timer_id b_id;

    check_true(cflow_timer_queue_init(&queue));
    a_id = cflow_timer_queue_schedule(&queue, (cflow_deadline){10u}, count_task, &a);
    b_id = cflow_timer_queue_schedule(&queue, (cflow_deadline){20u}, count_task, &b);
    check(a_id != 0u);
    check(b_id != 0u);
    check_true(cflow_timer_queue_cancel(&queue, a_id));
    check_false(cflow_timer_queue_cancel(&queue, a_id));
    check_true(cflow_timer_queue_take_ready(&queue, (cflow_instant){20u}, &task));
    check(task.user == &b);
    check_false(cflow_timer_queue_cancel(&queue, b_id));
    cflow_timer_queue_destroy(&queue);
  }

  it("bounds TimerQueue storage and reuses released capacity") {
    cflow_timer_queue queue;
    cflow_timer_task task;
    cflow_schedule_result result;
    const void *item_storage;

    check_false(cflow_timer_queue_init_with_capacity(&queue, 0u));
    check_false(cflow_timer_queue_init_with_capacity(&queue, SIZE_MAX));
    check_true(cflow_timer_queue_init_with_capacity(&queue, 1u));
    item_storage = queue.items;
    check(item_storage != NULL);

    result = cflow_timer_queue_try_schedule(
        &queue, (cflow_deadline){10u}, count_task, NULL);
    check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
    check(result.task_id != 0u);
    result = cflow_timer_queue_try_schedule(
        &queue, (cflow_deadline){20u}, count_task, NULL);
    check_equal(result.status, CFLOW_ADMISSION_FULL);
    check_equal(result.task_id, (cflow_task_id)0u);

    check_true(cflow_timer_queue_take_ready(
        &queue, (cflow_instant){10u}, &task));
    result = cflow_timer_queue_try_schedule(
        &queue, (cflow_deadline){20u}, count_task, NULL);
    check_equal(result.status, CFLOW_ADMISSION_ACCEPTED);
    check(queue.items == item_storage);
    cflow_timer_queue_destroy(&queue);
  }
}
