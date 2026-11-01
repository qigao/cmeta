#include <cflow/scheduler.h>
#include <thread.h>
#include "tinytest.h"

#include <stdatomic.h>

static int scheduler_order[4];
static size_t scheduler_order_count;

enum { CROSS_THREAD_POST_COUNT = 256 };

typedef struct cross_thread_post_state {
  cflow_scheduler *scheduler;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  bool start;
  _Atomic size_t accepted;
  _Atomic size_t executed;
  _Atomic bool producer_done;
} cross_thread_post_state;

static void count_cross_thread_task(void *user) {
  cross_thread_post_state *state = (cross_thread_post_state *)user;
  atomic_fetch_add(&state->executed, 1u);
}

static void post_from_external_callback_thread(void *user) {
  cross_thread_post_state *state = (cross_thread_post_state *)user;

  cmeta_mutex_lock(&state->mutex);
  while (!state->start)
    cmeta_cond_wait(&state->changed, &state->mutex);
  cmeta_mutex_unlock(&state->mutex);

  for (size_t i = 0u; i < CROSS_THREAD_POST_COUNT; ++i) {
    if (cflow_scheduler_post(state->scheduler,
                             count_cross_thread_task, state) != 0u)
      atomic_fetch_add(&state->accepted, 1u);
  }
  atomic_store(&state->producer_done, true);
}

static void record_order(void *user) {
  const int *value = (const int *)user;
  if (scheduler_order_count < sizeof(scheduler_order) / sizeof(scheduler_order[0]))
    scheduler_order[scheduler_order_count++] = *value;
}

spec("CFlow scheduler compatibility") {
  it("executes zero-delay inline work before admission returns") {
    cflow_scheduler scheduler = {0};
    cflow_schedule_result delayed;
    int value = 17;

    scheduler_order_count = 0u;
    check_true(cflow_scheduler_inline_init(&scheduler));
    check_equal(cflow_scheduler_name(&scheduler), "inline_scheduler");
    check_true((cflow_scheduler_capabilities(&scheduler) &
                CMETA_SCHED_CAP_CALLER_DRIVEN_ZERO_DELAY) != 0u);
    check_not_equal(cflow_scheduler_post(
                        &scheduler, record_order, &value),
                    (cflow_task_id)0u);
    check_equal(scheduler_order_count, (size_t)1u);
    check_equal(scheduler_order[0], 17);
    check_equal(cflow_scheduler_pending(&scheduler), (size_t)0u);
    check_equal(cflow_scheduler_run_until_idle(
                    &scheduler, 0u), (size_t)0u);

    delayed = cflow_scheduler_try_post_after(
        &scheduler, 1u, record_order, &value);
    check_equal(delayed.status, CFLOW_ADMISSION_INVALID_ARGUMENT);
    check_equal(delayed.task_id, (cflow_task_id)0u);
    check_equal(scheduler_order_count, (size_t)1u);

    check_true(cflow_scheduler_shutdown(&scheduler));
    check_equal(cflow_scheduler_try_post_after(
                    &scheduler, 0u, record_order, &value).status,
                CFLOW_ADMISSION_CLOSED);
    cflow_scheduler_destroy(&scheduler);
    check_false(cflow_scheduler_valid(&scheduler));
  }

  it("bounds caller-driven zero-delay work and reuses capacity") {
    cflow_scheduler scheduler = {0};
    cflow_schedule_result first;
    cflow_schedule_result full;
    cflow_schedule_result delayed;
    int value = 23;

    scheduler_order_count = 0u;
    check_false(cflow_scheduler_manual_init_with_capacity(
        &scheduler, 0u));
    check_true(cflow_scheduler_manual_init_with_capacity(
        &scheduler, 1u));
    check_true((cflow_scheduler_capabilities(&scheduler) &
                CMETA_SCHED_CAP_CALLER_DRIVEN_ZERO_DELAY) != 0u);
    first = cflow_scheduler_try_post_after(
        &scheduler, 0u, record_order, &value);
    full = cflow_scheduler_try_post_after(
        &scheduler, 0u, record_order, &value);
    delayed = cflow_scheduler_try_post_after(
        &scheduler, 1u, record_order, &value);

    check_equal(first.status, CFLOW_ADMISSION_ACCEPTED);
    check_not_equal(first.task_id, (cflow_task_id)0u);
    check_equal(full.status, CFLOW_ADMISSION_FULL);
    check_equal(full.task_id, (cflow_task_id)0u);
    check_equal(delayed.status, CFLOW_ADMISSION_INVALID_ARGUMENT);
    check_equal(cflow_scheduler_pending(&scheduler), (size_t)1u);
    check_equal(scheduler_order_count, (size_t)0u);
    check_equal(cflow_scheduler_run_ready(&scheduler), (size_t)1u);
    check_equal(scheduler_order_count, (size_t)1u);
    check_not_equal(cflow_scheduler_post(
                        &scheduler, record_order, &value),
                    (cflow_task_id)0u);
    check_true(cflow_scheduler_shutdown(&scheduler));
    check_equal(cflow_scheduler_run_ready(&scheduler), (size_t)1u);
    check_equal(scheduler_order_count, (size_t)2u);
    check_equal(cflow_scheduler_try_post_after(
                    &scheduler, 0u, record_order, &value).status,
                CFLOW_ADMISSION_CLOSED);
    cflow_scheduler_destroy(&scheduler);
    check_false(cflow_scheduler_valid(&scheduler));
  }

  it("keeps legacy ticks in milliseconds") {
    cflow_scheduler scheduler = {0};

    check_true(cflow_scheduler_test_init(&scheduler));
    check_equal(cflow_scheduler_now(&scheduler), 0u);
    check_equal(cflow_scheduler_advance(&scheduler, 25u), (size_t)0u);
    check_equal(cflow_scheduler_now(&scheduler), 25u);
    cflow_scheduler_destroy(&scheduler);
  }

  it("reports checked scheduling status with a zero id on failure") {
    cflow_scheduler scheduler = {0};
    cflow_schedule_result accepted;
    cflow_schedule_result invalid;
    int value = 9;

    check_true(cflow_scheduler_test_init(&scheduler));
    accepted = cflow_scheduler_try_post_after(&scheduler, 5u,
                                               record_order, &value);
    check_equal(accepted.status, CFLOW_ADMISSION_ACCEPTED);
    check(accepted.task_id != 0u);

    invalid = cflow_scheduler_try_post_after(&scheduler, 5u, NULL, NULL);
    check_equal(invalid.status, CFLOW_ADMISSION_INVALID_ARGUMENT);
    check_equal(invalid.task_id, (cflow_task_id)0u);
    cflow_scheduler_destroy(&scheduler);
  }

  it("bounds delayed admission and reuses timer capacity") {
    cflow_scheduler scheduler = {0};
    cflow_scheduler_stats stats = {0};
    cflow_schedule_result first;
    cflow_schedule_result full;
    cflow_schedule_result reused;
    int value = 3;

    scheduler_order_count = 0u;
    check_false(cflow_scheduler_test_init_with_capacity(&scheduler, 0u, 1u));
    check_false(cflow_scheduler_test_init_with_capacity(&scheduler, 1u, 0u));
    check_false(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 1u, 0u, 1u));
    check_false(cflow_scheduler_worker_init_with_capacity(
        &scheduler, 1u, 1u, 0u));
    check_true(cflow_scheduler_test_init_with_capacity(&scheduler, 1u, 1u));
    first = cflow_scheduler_try_post_after(&scheduler, 1u,
                                           record_order, &value);
    full = cflow_scheduler_try_post_after(&scheduler, 2u,
                                          record_order, &value);
    check_equal(first.status, CFLOW_ADMISSION_ACCEPTED);
    check_equal(full.status, CFLOW_ADMISSION_FULL);
    check_equal(full.task_id, (cflow_task_id)0u);
    check_true(cflow_scheduler_get_stats(&scheduler, &stats));
    check_equal(stats.ready_capacity, (size_t)1u);
    check_equal(stats.timer_capacity, (size_t)1u);
    check_equal(stats.timer_pending, (size_t)1u);
    check_equal(stats.peak_pending, (size_t)1u);
    check_equal(stats.rejected_full, (size_t)1u);
    check_equal(cflow_scheduler_advance(&scheduler, 1u), (size_t)1u);
    reused = cflow_scheduler_try_post_after(&scheduler, 1u,
                                            record_order, &value);
    check_equal(reused.status, CFLOW_ADMISSION_ACCEPTED);
    cflow_scheduler_destroy(&scheduler);
  }

  it("preserves FIFO order for equal deadlines") {
    cflow_scheduler scheduler = {0};
    int first = 1;
    int second = 2;
    int later = 3;

    scheduler_order_count = 0u;
    check_true(cflow_scheduler_test_init(&scheduler));
    check(cflow_scheduler_post_after(&scheduler, 10u, record_order, &first) != 0u);
    check(cflow_scheduler_post_after(&scheduler, 10u, record_order, &second) != 0u);
    check(cflow_scheduler_post_after(&scheduler, 20u, record_order, &later) != 0u);

    check_equal(cflow_scheduler_advance(&scheduler, 10u), (size_t)2u);
    check_equal(scheduler_order_count, (size_t)2u);
    check_equal(scheduler_order[0], 1);
    check_equal(scheduler_order[1], 2);

    check_equal(cflow_scheduler_run_until_idle(&scheduler, 0u), (size_t)1u);
    check_equal(scheduler_order_count, (size_t)3u);
    check_equal(scheduler_order[2], 3);
    check_equal(cflow_scheduler_now(&scheduler), 20u);
    cflow_scheduler_destroy(&scheduler);
  }

  it("cancels only pending delayed work") {
    cflow_scheduler scheduler = {0};
    int value = 7;
    cflow_task_id id;

    scheduler_order_count = 0u;
    check_true(cflow_scheduler_test_init(&scheduler));
    id = cflow_scheduler_post_after(&scheduler, 10u, record_order, &value);
    check(id != 0u);
    check_true(cflow_scheduler_cancel(&scheduler, id));
    check_false(cflow_scheduler_cancel(&scheduler, id));
    check_equal(cflow_scheduler_run_until_idle(&scheduler, 0u), (size_t)0u);
    check_equal(scheduler_order_count, (size_t)0u);
    cflow_scheduler_destroy(&scheduler);
  }

  it("accepts wake work while its owner drains the test loop") {
    cflow_scheduler scheduler = {0};
    cross_thread_post_state state = {0};
    cmeta_thread_t producer = NULL;

    state.scheduler = &scheduler;
    cmeta_mutex_init(&state.mutex);
    cmeta_cond_init(&state.changed);
    check_not_null(state.mutex);
    check_not_null(state.changed);
    check_true(cflow_scheduler_test_init_with_capacity(
        &scheduler, CROSS_THREAD_POST_COUNT, CROSS_THREAD_POST_COUNT));
    check_equal(cmeta_thread_create(
        &producer, post_from_external_callback_thread, &state), 0);

    cmeta_mutex_lock(&state.mutex);
    state.start = true;
    cmeta_cond_broadcast(&state.changed);
    cmeta_mutex_unlock(&state.mutex);

    while (!atomic_load(&state.producer_done)) {
      (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
      cmeta_thread_yield();
    }
    check_equal(cmeta_thread_join(&producer), 0);
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);

    check_equal(atomic_load(&state.accepted),
                (size_t)CROSS_THREAD_POST_COUNT);
    check_equal(atomic_load(&state.executed),
                (size_t)CROSS_THREAD_POST_COUNT);
    check_equal(cflow_scheduler_pending(&scheduler), (size_t)0u);

    cflow_scheduler_destroy(&scheduler);
    cmeta_cond_destroy(&state.changed);
    cmeta_mutex_destroy(&state.mutex);
  }
}
