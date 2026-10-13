/*
 * Exact Salts.Native v2.3.0-rc.15 async logger release/acquire TSan oracle.
 * Four genuinely concurrent callers publish copied stack-owned payloads into
 * one bounded logger and one independent consumer verifies every message.
 * The callback borrows message storage only while invoked. Never suppress
 * ThreadSanitizer and do not serialize the producer hot path to fake a pass.
 */
#define _POSIX_C_SOURCE 200809L
#include "tlog.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PRODUCERS = 4, PER_PRODUCER = 512, MESSAGE_CAPACITY = 96 };

typedef struct {
  unsigned char seen[PRODUCERS][PER_PRODUCER];
  unsigned count;
  unsigned invalid;
} consumer_probe;

typedef struct {
  tlog_t *logger;
  atomic_int *ready;
  atomic_int *start;
  unsigned owner;
  int error;
} publisher_context;

static uint32_t message_digest(unsigned owner, unsigned seq) {
  return UINT32_C(0xa5a5a5a5) ^ ((uint32_t)owner * UINT32_C(0x9e3779b9)) ^
         ((uint32_t)seq * UINT32_C(0x85ebca6b));
}

static void consume_one(const cmeta_log_entry_t *entry, void *userdata) {
  consumer_probe *probe = (consumer_probe *)userdata;
  unsigned owner = 0;
  unsigned sequence = 0;
  unsigned checksum = 0;
  char extra = 0;
  char borrowed[MESSAGE_CAPACITY];
  if (entry == NULL || entry->message.data == NULL ||
      entry->message.len == 0 || entry->message.len >= sizeof(borrowed)) {
    ++probe->invalid;
    return;
  }
  memcpy(borrowed, entry->message.data, entry->message.len);
  borrowed[entry->message.len] = '\0';
  if (sscanf(borrowed, "owner=%u sequence=%u hash=%x%c",
             &owner, &sequence, &checksum, &extra) != 3 ||
      owner >= PRODUCERS || sequence >= PER_PRODUCER ||
      checksum != message_digest(owner, sequence) ||
      entry->component.len != 4 ||
      memcmp(entry->component.data, "tsan", 4) != 0) {
    ++probe->invalid;
    return;
  }
  if (probe->seen[owner][sequence] != 0) {
    ++probe->invalid;
    return;
  }
  probe->seen[owner][sequence] = 1;
  ++probe->count;
}

static void *produce_entries(void *userdata) {
  publisher_context *ctx = (publisher_context *)userdata;
  unsigned sequence;
  atomic_fetch_add_explicit(ctx->ready, 1, memory_order_release);
  while (!atomic_load_explicit(ctx->start, memory_order_acquire))
    sched_yield();

  for (sequence = 0; sequence < PER_PRODUCER; ++sequence) {
    char text[MESSAGE_CAPACITY];
    int n = snprintf(text, sizeof(text),
                     "owner=%u sequence=%u hash=%08x",
                     ctx->owner, sequence,
                     message_digest(ctx->owner, sequence));
    if (n <= 0 || (size_t)n >= sizeof(text)) {
      ctx->error = 1;
      return NULL;
    }
    cmeta_log_str(ctx->logger, SALTS_LOG_LEVEL_INFO,
                  vstr_from_buf("tsan", 4),
                  vstr_from_buf("tlog_async_release_acquire_tsan.c",
                                sizeof("tlog_async_release_acquire_tsan.c") - 1),
                  1, vstr_from_buf(text, (size_t)n));
    /* The logger must have copied this caller-owned stack buffer before
     * returning. The consumer must never borrow it across this mutation. */
    memset(text, '#', sizeof(text));
  }
  return NULL;
}

int main(void) {
  tlog_config_t config = {
      .min_level = SALTS_LOG_LEVEL_DEBUG,
      .buffer_size = 8 * 1024,
      .pool_size = 16 * 1024};
  consumer_probe probe = {0};
  atomic_int ready = ATOMIC_VAR_INIT(0);
  atomic_int start = ATOMIC_VAR_INIT(0);
  publisher_context publishers[PRODUCERS] = {{0}};
  pthread_t threads[PRODUCERS];
  unsigned created = 0;
  unsigned owner;
  int failed = 0;

  tlog_t *logger = tlog_create(&config);
  if (logger == NULL) {
    fputs("TLOG_SOURCE_TSAN: logger create failed\n", stderr);
    return 2;
  }
  cmeta_log_sink_t *sink = cmeta_sink_callback_create(consume_one, &probe);
  if (sink == NULL) {
    tlog_destroy(logger);
    return 3;
  }
  if (tlog_add_sink(logger, sink) != 0) {
    cmeta_sink_destroy(sink);
    tlog_destroy(logger);
    return 4;
  }

  for (owner = 0; owner < PRODUCERS; ++owner) {
    publisher_context *ctx = &publishers[owner];
    ctx->logger = logger;
    ctx->ready = &ready;
    ctx->start = &start;
    ctx->owner = owner;
    if (pthread_create(&threads[owner], NULL, produce_entries, ctx) != 0) {
      failed = 1;
      break;
    }
    ++created;
  }
  while (atomic_load_explicit(&ready, memory_order_acquire) < (int)created)
    sched_yield();
  atomic_store_explicit(&start, 1, memory_order_release);
  for (owner = 0; owner < created; ++owner)
    (void)pthread_join(threads[owner], NULL);
  for (owner = 0; owner < created; ++owner)
    if (publishers[owner].error != 0) failed = 1;

  tlog_flush(logger);
  uint64_t written = tlog_get_written(logger);
  uint64_t dropped = tlog_get_dropped(logger);
  tlog_destroy(logger); /* Joins the worker before inspecting callback state. */

  if (created != PRODUCERS || written != PRODUCERS * PER_PRODUCER ||
      dropped != 0 || probe.count != PRODUCERS * PER_PRODUCER ||
      probe.invalid != 0) {
    fprintf(stderr,
            "TLOG_SOURCE_TSAN: created=%u written=%llu dropped=%llu "
            "received=%u invalid=%u\n",
            created, (unsigned long long)written, (unsigned long long)dropped,
            probe.count, probe.invalid);
    failed = 1;
  }
  for (owner = 0; owner < PRODUCERS; ++owner) {
    for (unsigned sequence = 0; sequence < PER_PRODUCER; ++sequence) {
      if (probe.seen[owner][sequence] != 1) failed = 1;
    }
  }
  if (!failed)
    puts("TLOG_RC15_SOURCE_TSAN_PASS: 4 concurrent publishers, 2048 copied entries, exact single-consumer delivery");
  return failed ? 5 : 0;
}
