#include <cflow/cflow.h>
#include <cflow/publishers.h>
#include <cflow/reactive.h>
#include <cflow/scheduler.h>
#include <cflow/stream.h>
#include <cmeta/cmeta.h>
#include <clock.h>
#include <deadline_queue.h>
#include <error_codes.h>
#include <thread.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct android_thread_probe {
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  int ready;
} android_thread_probe;

typedef struct android_reactive_probe {
  size_t values;
  size_t errors;
  size_t dones;
  int last_value;
} android_reactive_probe;

static atomic_int android_executor_runs;
static atomic_int android_timer_runs;

static int android_fail(const char *stage, int code) {
  fprintf(stderr,
          "SALTS_ANDROID_RUNTIME_FAILURE stage=%s code=%d\n",
          stage != NULL ? stage : "unknown", code);
  return code != 0 ? code : 1;
}

static void android_signal_thread(void *user) {
  android_thread_probe *probe = (android_thread_probe *)user;
  cmeta_sleep_ms(5u);
  cmeta_mutex_lock(&probe->mutex);
  probe->ready = 1;
  cmeta_cond_broadcast(&probe->changed);
  cmeta_mutex_unlock(&probe->mutex);
}

static void android_count_executor(void *user) {
  (void)user;
  atomic_fetch_add_explicit(
      &android_executor_runs, 1, memory_order_relaxed);
}

static void android_count_timer(void *user) {
  (void)user;
  atomic_fetch_add_explicit(
      &android_timer_runs, 1, memory_order_relaxed);
}

static bool android_on_value(
    void *user, const cmeta_type_desc *type, const void *value) {
  android_reactive_probe *probe = (android_reactive_probe *)user;
  if (probe == NULL || value == NULL ||
      !cmeta_type_equal(type, &cmeta_type_int))
    return false;
  ++probe->values;
  probe->last_value = *(const int *)value;
  return true;
}

static void android_on_error(void *user, const char *message) {
  android_reactive_probe *probe = (android_reactive_probe *)user;
  (void)message;
  if (probe != NULL) ++probe->errors;
}

static void android_on_done(void *user) {
  android_reactive_probe *probe = (android_reactive_probe *)user;
  if (probe != NULL) ++probe->dones;
}

static int android_check_cmeta(void) {
  if (!cmeta_type_desc_valid(&cmeta_type_int))
    return SALTS_EPROTO;
  if (!cmeta_type_equal(&cmeta_type_int, &cmeta_type_int))
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int android_check_clock(void) {
  const uint64_t before_ns = cmeta_hrtime();
  const uint64_t before_ms = cmeta_monotonic_ms();
  uint64_t after_ns;
  uint64_t after_ms;

  cmeta_sleep_ms(5u);
  after_ns = cmeta_hrtime();
  after_ms = cmeta_monotonic_ms();
  if (after_ns <= before_ns || after_ms < before_ms)
    return SALTS_EPROTO;
  return SALTS_OK;
}

static int android_check_thread_condition(void) {
  android_thread_probe probe = {0};
  cmeta_thread_t thread = NULL;
  int status = SALTS_OK;
  int wait_status = 0;

  cmeta_mutex_init(&probe.mutex);
  cmeta_cond_init(&probe.changed);
  if (probe.mutex == NULL || probe.changed == NULL) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }

  status = cmeta_thread_create(&thread, android_signal_thread, &probe);
  if (status != SALTS_OK)
    goto cleanup;

  cmeta_mutex_lock(&probe.mutex);
  while (!probe.ready && wait_status == 0)
    wait_status = cmeta_cond_timedwait(
        &probe.changed, &probe.mutex,
        UINT64_C(5000000000));
  cmeta_mutex_unlock(&probe.mutex);
  if (!probe.ready || wait_status != 0) {
    status = wait_status != 0 ? wait_status : SALTS_ETIMEDOUT;
    goto cleanup;
  }

  status = cmeta_thread_join(&thread);
  if (status != SALTS_OK)
    goto cleanup;
  if (probe.ready != 1)
    status = SALTS_EPROTO;

cleanup:
  if (thread != NULL) {
    cmeta_thread_destroy(&thread);
  }
  if (probe.changed != NULL)
    cmeta_cond_destroy(&probe.changed);
  if (probe.mutex != NULL)
    cmeta_mutex_destroy(&probe.mutex);
  return status;
}

static int android_check_deadline_queue(void) {
  cmeta_deadline_queue queue = {0};
  cmeta_deadline_id first = 0u;
  cmeta_deadline_id second = 0u;
  cmeta_deadline_id rejected = UINT64_MAX;
  cmeta_deadline_event event = {0};
  int status;

  status = cmeta_deadline_queue_init(&queue, 2u);
  if (status != SALTS_OK) return status;

  status = cmeta_deadline_queue_schedule(
      &queue, 10u, 101u, &first);
  if (status != SALTS_OK) goto cleanup;
  status = cmeta_deadline_queue_schedule(
      &queue, 20u, 202u, &second);
  if (status != SALTS_OK) goto cleanup;
  status = cmeta_deadline_queue_schedule(
      &queue, 30u, 303u, &rejected);
  if (status != SALTS_ENOBUFS || rejected != 0u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  status = cmeta_deadline_queue_take_ready(&queue, 9u, &event);
  if (status != SALTS_ETIMEDOUT) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  status = cmeta_deadline_queue_take_ready(&queue, 10u, &event);
  if (status != SALTS_OK || event.id != first ||
      event.deadline_ms != 10u || event.token != 101u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  status = cmeta_deadline_queue_cancel(&queue, second, &event);
  if (status != SALTS_OK || event.id != second ||
      event.deadline_ms != 20u || event.token != 202u ||
      cmeta_deadline_queue_size(&queue) != 0u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  status = SALTS_OK;

cleanup: {
    const int destroy_status =
        cmeta_deadline_queue_destroy(&queue);
    if (status == SALTS_OK) status = destroy_status;
  }
  return status;
}

static int android_check_executor(void) {
  cflow_executor executor = {0};
  cflow_executor_stats stats = {0};
  int status = SALTS_OK;

  atomic_store_explicit(
      &android_executor_runs, 0, memory_order_relaxed);
  if (!cflow_executor_manual_init_with_capacity(&executor, 1u))
    return SALTS_ENOMEM;

  if (cflow_executor_try_post(
          &executor, android_count_executor, NULL) !=
      CFLOW_ADMISSION_ACCEPTED ||
      cflow_executor_try_post(
          &executor, android_count_executor, NULL) !=
      CFLOW_ADMISSION_FULL) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  if (!cflow_executor_get_stats(&executor, &stats) ||
      stats.capacity != 1u || stats.pending != 1u ||
      stats.rejected_full != 1u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  if (!cflow_executor_run_one(&executor) ||
      atomic_load_explicit(
          &android_executor_runs, memory_order_relaxed) != 1) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  if (cflow_executor_try_post(
          &executor, android_count_executor, NULL) !=
      CFLOW_ADMISSION_ACCEPTED ||
      !cflow_executor_run_one(&executor) ||
      atomic_load_explicit(
          &android_executor_runs, memory_order_relaxed) != 2) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  if (!cflow_executor_shutdown(&executor) ||
      cflow_executor_try_post(
          &executor, android_count_executor, NULL) !=
      CFLOW_ADMISSION_CLOSED) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

cleanup:
  cflow_executor_destroy(&executor);
  return status;
}

static int android_check_scheduler_timer(void) {
  cflow_scheduler scheduler = {0};
  cflow_schedule_result first;
  cflow_schedule_result full;
  cflow_schedule_result reused;
  int status = SALTS_OK;

  atomic_store_explicit(
      &android_timer_runs, 0, memory_order_relaxed);
  if (!cflow_scheduler_test_init_with_capacity(
          &scheduler, 1u, 1u))
    return SALTS_ENOMEM;

  first = cflow_scheduler_try_post_after(
      &scheduler, 1u, android_count_timer, NULL);
  full = cflow_scheduler_try_post_after(
      &scheduler, 2u, android_count_timer, NULL);
  if (first.status != CFLOW_ADMISSION_ACCEPTED ||
      first.task_id == 0u ||
      full.status != CFLOW_ADMISSION_FULL ||
      full.task_id != 0u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  if (cflow_scheduler_advance(&scheduler, 1u) != 1u ||
      atomic_load_explicit(
          &android_timer_runs, memory_order_relaxed) != 1) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  reused = cflow_scheduler_try_post_after(
      &scheduler, 1u, android_count_timer, NULL);
  if (reused.status != CFLOW_ADMISSION_ACCEPTED ||
      reused.task_id == 0u ||
      !cflow_scheduler_cancel(&scheduler, reused.task_id) ||
      cflow_scheduler_cancel(&scheduler, reused.task_id)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  if (!cflow_scheduler_shutdown(&scheduler) ||
      cflow_scheduler_try_post_after(
          &scheduler, 0u, android_count_timer, NULL).status !=
      CFLOW_ADMISSION_CLOSED) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

cleanup:
  cflow_scheduler_destroy(&scheduler);
  return status;
}

static int android_check_reactive_wait_wake_cancel(void) {
  cflow_stream stream = {0};
  cflow_scheduler scheduler = {0};
  cflow_channel channel = {0};
  cflow_publisher publisher = {0};
  cflow_subscription subscription = {0};
  android_reactive_probe probe = {0};
  cflow_subscriber_callbacks callbacks = {
      android_on_value, android_on_error, android_on_done, &probe};
  cflow_subscriber subscriber =
      cflow_subscriber_from_callbacks(&callbacks);
  int value = 42;
  int after_cancel = 99;
  int status = SALTS_OK;
  bool stream_initialized = false;
  bool scheduler_initialized = false;
  bool channel_initialized = false;
  bool subscription_open = false;

  if (cflow_stream_init(&stream, &cmeta_type_int) == NULL)
    return SALTS_ENOMEM;
  stream_initialized = true;
  if (!cflow_scheduler_test_init_with_capacity(
          &scheduler, 4u, 2u)) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }
  scheduler_initialized = true;
  if (!cflow_channel_init(&channel, &cmeta_type_int, 1u)) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }
  channel_initialized = true;
  if (!cflow_publisher_from_channel(&publisher, &channel)) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }
  if (!cflow_subscribe(
          &subscription, &stream.graph, &publisher,
          &scheduler, &subscriber)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  subscription_open = true;

  if (!cflow_subscription_request(&subscription, 1u)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  if (probe.values != 0u || probe.errors != 0u ||
      cflow_subscription_outstanding_demand(&subscription) != 1u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  if (cflow_channel_try_push(&channel, &value) !=
      CFLOW_CHANNEL_OK) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  if (probe.values != 1u || probe.last_value != 42 ||
      probe.errors != 0u ||
      cflow_subscription_outstanding_demand(&subscription) != 0u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  if (!cflow_subscription_request(&subscription, 1u)) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  if (cflow_subscription_outstanding_demand(&subscription) != 1u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  cflow_subscription_cancel(&subscription);
  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  if (!cflow_subscription_is_cancelled(&subscription) ||
      cflow_subscription_status(&subscription) !=
          CFLOW_STATUS_CANCELLED) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

  if (cflow_channel_try_push(&channel, &after_cancel) !=
      CFLOW_CHANNEL_OK) {
    status = SALTS_EPROTO;
    goto cleanup;
  }
  (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
  if (probe.values != 1u || probe.last_value != 42 ||
      probe.errors != 0u) {
    status = SALTS_EPROTO;
    goto cleanup;
  }

cleanup:
  if (subscription_open)
    cflow_subscription_close(&subscription);
  else if (cflow_publisher_valid(&publisher))
    cflow_publisher_destroy(&publisher);
  if (channel_initialized)
    cflow_channel_destroy(&channel);
  if (scheduler_initialized)
    cflow_scheduler_destroy(&scheduler);
  if (stream_initialized)
    cflow_stream_destroy(&stream);
  return status;
}

int main(void) {
  int status;

  status = android_check_cmeta();
  if (status != SALTS_OK) return android_fail("cmeta", status);
  status = android_check_clock();
  if (status != SALTS_OK) return android_fail("clock", status);
  status = android_check_thread_condition();
  if (status != SALTS_OK)
    return android_fail("thread_condition", status);
  status = android_check_deadline_queue();
  if (status != SALTS_OK)
    return android_fail("deadline_queue", status);
  status = android_check_executor();
  if (status != SALTS_OK)
    return android_fail("executor_bounded", status);
  status = android_check_scheduler_timer();
  if (status != SALTS_OK)
    return android_fail("scheduler_timer_shutdown", status);
  status = android_check_reactive_wait_wake_cancel();
  if (status != SALTS_OK)
    return android_fail("reactive_wait_wake_cancel", status);

  printf(
      "SALTS_ANDROID_RUNTIME_JSON "
      "{\"schema\":\"salts-android-runtime/v1\","
      "\"package_consumer\":true,"
      "\"cmeta\":true,"
      "\"clock\":true,"
      "\"thread_condition\":true,"
      "\"deadline_queue\":true,"
      "\"executor_bounded\":true,"
      "\"scheduler_timer\":true,"
      "\"scheduler_shutdown\":true,"
      "\"reactive_wait_wake_cancel\":true,"
      "\"executor_runs\":%d,"
      "\"timer_runs\":%d,"
      "\"reactive_values\":1,"
      "\"reactive_value\":42}\n",
      atomic_load_explicit(
          &android_executor_runs, memory_order_relaxed),
      atomic_load_explicit(
          &android_timer_runs, memory_order_relaxed));
  return 0;
}
