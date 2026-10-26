#include "disruptor.h"
#include "cmeta_thread.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <intrin.h>
  #include <malloc.h>
  #include <windows.h>
#else
  #include <sched.h>
#endif

#define DISRUPTOR_CACHE_LINE_SIZE 64U
#define DISRUPTOR_PAGE_SIZE 4096U

#define DISRUPTOR_WAIT_COUNT 256U
#define DISRUPTOR_YIELD_INTERVAL 1024U
#define DISRUPTOR_PRODUCER_YIELD_INTERVAL 64U
#define DISRUPTOR_WORKER_PARK_SPIN_ROUNDS 16U
#define DISRUPTOR_VACANT UINT_FAST64_MAX

#if defined(__GNUC__) || defined(__clang__)
  #define DISRUPTOR_LIKELY(x) __builtin_expect(!!(x), 1)
  #define DISRUPTOR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define DISRUPTOR_LIKELY(x) (x)
  #define DISRUPTOR_UNLIKELY(x) (x)
#endif

#ifdef _MSC_VER
__declspec(align(DISRUPTOR_CACHE_LINE_SIZE)) struct disruptor_count_s {
#else
struct disruptor_count_s {
#endif
  uint_fast64_t count;
#ifndef _MSC_VER
} __attribute__((aligned(DISRUPTOR_CACHE_LINE_SIZE)));
#else
};
#endif
typedef struct disruptor_count_s disruptor_count_t;

#ifdef _MSC_VER
__declspec(align(DISRUPTOR_CACHE_LINE_SIZE)) struct disruptor_cursor_state_s {
#else
struct disruptor_cursor_state_s {
#endif
  atomic_uint_fast64_t sequence;
#ifndef _MSC_VER
} __attribute__((aligned(DISRUPTOR_CACHE_LINE_SIZE)));
#else
};
#endif
typedef struct disruptor_cursor_state_s disruptor_cursor_state_t;

struct disruptor_s {
  disruptor_count_t ring_mask;
  disruptor_cursor_state_t slowest_consumer;
  disruptor_cursor_state_t max_read_cursor;
  disruptor_cursor_state_t write_cursor;
  disruptor_cursor_state_t worker_claim_cursor;
  disruptor_cursor_state_t worker_completed_cursor;
  uint64_t capacity;
  uint32_t consumer_capacity;
  disruptor_mode_t mode;
  size_t entry_size;
  disruptor_cursor_state_t *consumer_cursors;
  atomic_uint_fast64_t *published_sequences;
  atomic_uint_fast64_t *worker_completed_sequences;
  cmeta_mutex_t worker_wait_mutex;
  cmeta_cond_t worker_wait_cond;
  atomic_uint worker_waiters;
  uint32_t *consumer_dependencies;
  uint32_t *consumer_dependency_counts;
  uint8_t *buffer;
};

typedef struct {
  const char *name;
  disruptor_consumer_t consumer;
} disruptor_topology_stage_def_t;

struct disruptor_topology_s {
  disruptor_t *disruptor;
  uint32_t stage_capacity;
  uint32_t stage_count;
  uint32_t group_capacity;
  uint32_t group_count;
  disruptor_topology_stage_def_t *stages;
  const char **group_names;
  uint8_t *edges;
  uint8_t *group_members;
};

static int disruptor_is_power_of_two(uint64_t value) {
  return value != 0U && (value & (value - 1U)) == 0U;
}

static void disruptor_cpu_pause(void) {
#if defined(_MSC_VER)
  _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
  #if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
  #elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
  #endif
#endif
}

static void disruptor_thread_yield(void) {
#ifdef _WIN32
  SwitchToThread();
#else
  sched_yield();
#endif
}

static void disruptor_spin_pause(void) {
  unsigned int i;
  for (i = 0; i < DISRUPTOR_WAIT_COUNT; ++i) {
    disruptor_cpu_pause();
  }
}

static void disruptor_spin_backoff(unsigned int *wait_rounds) {
  if (++(*wait_rounds) >= DISRUPTOR_YIELD_INTERVAL) {
    *wait_rounds = 0U;
    disruptor_thread_yield();
    return;
  }
  disruptor_spin_pause();
}

static void disruptor_spin_backoff_producer(unsigned int *wait_rounds) {
  if (++(*wait_rounds) >= DISRUPTOR_PRODUCER_YIELD_INTERVAL) {
    *wait_rounds = 0U;
    disruptor_thread_yield();
    return;
  }
  disruptor_spin_pause();
}

static void *disruptor_aligned_malloc(size_t alignment, size_t size) {
#ifdef _WIN32
  return _aligned_malloc(size, alignment);
#else
  void *ptr = NULL;
  if (posix_memalign(&ptr, alignment, size) != 0) {
    return NULL;
  }
  return ptr;
#endif
}

static void disruptor_aligned_free(void *ptr) {
  if (ptr == NULL) {
    return;
  }
#ifdef _WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static uint64_t disruptor_ring_index(const disruptor_t *disruptor, uint64_t sequence) {
  return disruptor->ring_mask.count & sequence;
}

static size_t disruptor_topology_index(uint32_t row, uint32_t col, uint32_t width) {
  return ((size_t)row * (size_t)width) + (size_t)col;
}

static int disruptor_publisher_has_capacity(const disruptor_t *disruptor, uint64_t writer_sequence,
                                            uint64_t slowest_sequence) {
  return (writer_sequence - slowest_sequence) <= disruptor->capacity;
}

static int disruptor_consumer_is_registered(const disruptor_t *disruptor,
                                            const disruptor_consumer_t *consumer) {
  if (disruptor == NULL || consumer == NULL || consumer->slot >= disruptor->consumer_capacity) {
    return 0;
  }
  return atomic_load_explicit(&disruptor->consumer_cursors[consumer->slot].sequence,
                              memory_order_acquire) != DISRUPTOR_VACANT;
}

static uint64_t disruptor_refresh_slowest_reader(disruptor_t *disruptor, uint64_t target_sequence) {
  uint32_t i;
  uint64_t slowest_sequence = DISRUPTOR_VACANT;
  uint64_t cached_sequence;

  /* A publisher can reuse a payload slot solely from its acquire load of
   * slowest_consumer, without refreshing the underlying consumer/worker
   * cursors. Every update advancing this cached retirement frontier must
   * therefore be a RELEASE store, completing the happens-before chain:
   *
   * payload read -> consumer cursor/worker-completion release ->
   * refresh acquire -> cached frontier release -> publisher acquire ->
   * next payload write.
   *
   * Relaxed publication of this cache allows an unsynchronized payload
   * read/write across wraps; source TSan reported exactly that race.
   * This is a private memory-order fix, not a public ABI/storage change.
   */
  if (disruptor->mode == DISRUPTOR_MODE_WORKER_POOL) {
    slowest_sequence =
        atomic_load_explicit(&disruptor->worker_completed_cursor.sequence, memory_order_acquire);
    cached_sequence =
        atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_relaxed);
    if (slowest_sequence > cached_sequence) {
      atomic_store_explicit(&disruptor->slowest_consumer.sequence, slowest_sequence,
                            memory_order_release);
    }
    return slowest_sequence;
  }

  for (i = 0; i < disruptor->consumer_capacity; ++i) {
    uint64_t seq =
        atomic_load_explicit(&disruptor->consumer_cursors[i].sequence, memory_order_acquire);
    if (seq < slowest_sequence) {
      slowest_sequence = seq;
    }
  }

  if (DISRUPTOR_UNLIKELY(slowest_sequence == DISRUPTOR_VACANT)) {
    slowest_sequence = target_sequence - disruptor_ring_index(disruptor, target_sequence);
  }

  cached_sequence =
      atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_relaxed);
  if (slowest_sequence > cached_sequence) {
    atomic_store_explicit(&disruptor->slowest_consumer.sequence, slowest_sequence,
                          memory_order_release);
  }

  return slowest_sequence;
}

static void disruptor_mark_published(disruptor_t *disruptor, uint64_t sequence) {
  uint64_t index = disruptor_ring_index(disruptor, sequence);
  atomic_store_explicit(&disruptor->published_sequences[index], sequence, memory_order_release);
}

static uint64_t disruptor_try_advance_published_cursor(disruptor_t *disruptor) {
  while (1) {
    /* Serialize marker publication with other advancers before scanning.
     * A load can miss their cursor/marker stores: both publishers then return
     * without closing a gap. An acquire/release RMW reads the latest cursor
     * modification and carries earlier markers through its release sequence,
     * even when this publisher cannot advance the cursor itself. */
    uint64_t current =
        atomic_fetch_add_explicit(&disruptor->max_read_cursor.sequence, 0U, memory_order_acq_rel);
    uint64_t next = current + 1U;
    uint64_t probe = next;

    while (atomic_load_explicit(
               &disruptor->published_sequences[disruptor_ring_index(disruptor, probe)],
               memory_order_acquire) == probe) {
      ++probe;
    }

    if (probe == next) {
      return current;
    }

    {
      uint64_t desired = probe - 1U;
      uint64_t expected = current;
      if (atomic_compare_exchange_weak_explicit(&disruptor->max_read_cursor.sequence, &expected,
                                                desired, memory_order_release,
                                                memory_order_relaxed)) {
        return desired;
      }
    }
  }
}

static void disruptor_worker_notify(disruptor_t *disruptor) {
  if (disruptor == NULL ||
      atomic_load_explicit(&disruptor->worker_waiters, memory_order_acquire) == 0U) {
    return;
  }
  cmeta_mutex_lock(&disruptor->worker_wait_mutex);
  cmeta_cond_broadcast(&disruptor->worker_wait_cond);
  cmeta_mutex_unlock(&disruptor->worker_wait_mutex);
}

static void disruptor_mark_worker_completed(disruptor_t *disruptor, uint64_t sequence) {
  uint64_t index = disruptor_ring_index(disruptor, sequence);
  atomic_store_explicit(&disruptor->worker_completed_sequences[index], sequence,
                        memory_order_release);
}

static uint64_t disruptor_try_advance_worker_completed_cursor(disruptor_t *disruptor) {
  while (1) {
    /* Completion markers need the same handoff as publication markers; every
     * released slot must become reusable without waiting for another release. */
    uint64_t current =
        atomic_fetch_add_explicit(&disruptor->worker_completed_cursor.sequence, 0U,
                                  memory_order_acq_rel);
    uint64_t next = current + 1U;
    uint64_t probe = next;

    while (atomic_load_explicit(
               &disruptor->worker_completed_sequences[disruptor_ring_index(disruptor, probe)],
               memory_order_acquire) == probe) {
      ++probe;
    }

    if (probe == next) {
      return current;
    }

    {
      uint64_t desired = probe - 1U;
      uint64_t expected = current;
      if (atomic_compare_exchange_weak_explicit(&disruptor->worker_completed_cursor.sequence,
                                                &expected, desired, memory_order_release,
                                                memory_order_relaxed)) {
        atomic_store_explicit(&disruptor->slowest_consumer.sequence, desired,
                              memory_order_release);
        return desired;
      }
    }
  }
}

static void disruptor_init(disruptor_t *disruptor) {
  uint32_t i;
  uint64_t s;

  for (i = 0; i < disruptor->consumer_capacity; ++i) {
    atomic_store_explicit(&disruptor->consumer_cursors[i].sequence, DISRUPTOR_VACANT,
                          memory_order_relaxed);
  }
  for (s = 0; s < disruptor->capacity; ++s) {
    atomic_store_explicit(&disruptor->published_sequences[s], 0U, memory_order_relaxed);
    if (disruptor->mode == DISRUPTOR_MODE_WORKER_POOL) {
      atomic_store_explicit(&disruptor->worker_completed_sequences[s], 0U, memory_order_relaxed);
    }
  }
  memset(disruptor->consumer_dependencies, 0,
         sizeof(uint32_t) * disruptor->consumer_capacity * disruptor->consumer_capacity);
  memset(disruptor->consumer_dependency_counts, 0,
         sizeof(uint32_t) * disruptor->consumer_capacity);

  disruptor->ring_mask.count = disruptor->capacity - 1U;
  atomic_store_explicit(&disruptor->slowest_consumer.sequence, 0U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->max_read_cursor.sequence, 0U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->write_cursor.sequence, 0U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->worker_claim_cursor.sequence, 1U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->worker_completed_cursor.sequence, 0U, memory_order_relaxed);
  atomic_store_explicit(&disruptor->worker_waiters, 0U, memory_order_relaxed);
}

disruptor_t *disruptor_create(const disruptor_config_t *config) {
  disruptor_t *disruptor;
  size_t buffer_bytes;
  size_t cursors_bytes;
  size_t published_bytes;
  size_t dependencies_bytes;

  if (config == NULL) {
    return NULL;
  }
  if (config->entry_size == 0U || config->capacity == 0U || config->consumer_capacity == 0U) {
    return NULL;
  }
  if (!disruptor_is_power_of_two(config->capacity)) {
    return NULL;
  }
  if (config->entry_size > (SIZE_MAX / config->capacity)) {
    return NULL;
  }
  if (config->capacity > ((uint64_t)SIZE_MAX / sizeof(atomic_uint_fast64_t))) {
    return NULL;
  }
  if (config->mode != DISRUPTOR_MODE_BROADCAST &&
      config->mode != DISRUPTOR_MODE_WORKER_POOL) {
    return NULL;
  }
  if ((size_t)config->consumer_capacity >
      ((SIZE_MAX / sizeof(uint32_t)) / (size_t)config->consumer_capacity)) {
    return NULL;
  }

  buffer_bytes = config->entry_size * (size_t)config->capacity;
  cursors_bytes = sizeof(disruptor_cursor_state_t) * config->consumer_capacity;
  published_bytes = sizeof(atomic_uint_fast64_t) * (size_t)config->capacity;
  dependencies_bytes = sizeof(uint32_t) * config->consumer_capacity * config->consumer_capacity;

  disruptor = (disruptor_t *)disruptor_aligned_malloc(DISRUPTOR_PAGE_SIZE, sizeof(*disruptor));
  if (disruptor == NULL) {
    return NULL;
  }
  memset(disruptor, 0, sizeof(*disruptor));

  disruptor->consumer_cursors = (disruptor_cursor_state_t *)disruptor_aligned_malloc(
      DISRUPTOR_CACHE_LINE_SIZE, cursors_bytes);
  if (disruptor->consumer_cursors == NULL) {
    disruptor_aligned_free(disruptor);
    return NULL;
  }
  disruptor->published_sequences =
      (atomic_uint_fast64_t *)disruptor_aligned_malloc(DISRUPTOR_CACHE_LINE_SIZE, published_bytes);
  if (disruptor->published_sequences == NULL) {
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }
  if (config->mode == DISRUPTOR_MODE_WORKER_POOL) {
    disruptor->worker_completed_sequences = (atomic_uint_fast64_t *)disruptor_aligned_malloc(
        DISRUPTOR_CACHE_LINE_SIZE, published_bytes);
    if (disruptor->worker_completed_sequences == NULL) {
      disruptor_aligned_free((void *)disruptor->published_sequences);
      disruptor_aligned_free(disruptor->consumer_cursors);
      disruptor_aligned_free(disruptor);
      return NULL;
    }
  }
  disruptor->consumer_dependencies = (uint32_t *)calloc(1U, dependencies_bytes);
  if (disruptor->consumer_dependencies == NULL) {
    disruptor_aligned_free((void *)disruptor->worker_completed_sequences);
    disruptor_aligned_free((void *)disruptor->published_sequences);
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }
  disruptor->consumer_dependency_counts =
      (uint32_t *)calloc(config->consumer_capacity, sizeof(uint32_t));
  if (disruptor->consumer_dependency_counts == NULL) {
    free(disruptor->consumer_dependencies);
    disruptor_aligned_free((void *)disruptor->worker_completed_sequences);
    disruptor_aligned_free((void *)disruptor->published_sequences);
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }

  disruptor->buffer = (uint8_t *)disruptor_aligned_malloc(DISRUPTOR_CACHE_LINE_SIZE, buffer_bytes);
  if (disruptor->buffer == NULL) {
    free(disruptor->consumer_dependency_counts);
    free(disruptor->consumer_dependencies);
    disruptor_aligned_free((void *)disruptor->worker_completed_sequences);
    disruptor_aligned_free((void *)disruptor->published_sequences);
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }

  disruptor->entry_size = config->entry_size;
  disruptor->capacity = config->capacity;
  disruptor->consumer_capacity = config->consumer_capacity;
  disruptor->mode = config->mode;
  cmeta_mutex_init(&disruptor->worker_wait_mutex);
  cmeta_cond_init(&disruptor->worker_wait_cond);
  if (!disruptor->worker_wait_mutex || !disruptor->worker_wait_cond) {
    cmeta_cond_destroy(&disruptor->worker_wait_cond);
    cmeta_mutex_destroy(&disruptor->worker_wait_mutex);
    disruptor_aligned_free(disruptor->buffer);
    free(disruptor->consumer_dependency_counts);
    free(disruptor->consumer_dependencies);
    disruptor_aligned_free((void *)disruptor->worker_completed_sequences);
    disruptor_aligned_free((void *)disruptor->published_sequences);
    disruptor_aligned_free(disruptor->consumer_cursors);
    disruptor_aligned_free(disruptor);
    return NULL;
  }
  disruptor_init(disruptor);

  return disruptor;
}

void disruptor_destroy(disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return;
  }
  cmeta_cond_destroy(&disruptor->worker_wait_cond);
  cmeta_mutex_destroy(&disruptor->worker_wait_mutex);
  disruptor_aligned_free(disruptor->buffer);
  free(disruptor->consumer_dependency_counts);
  free(disruptor->consumer_dependencies);
  disruptor_aligned_free((void *)disruptor->worker_completed_sequences);
  disruptor_aligned_free((void *)disruptor->published_sequences);
  disruptor_aligned_free(disruptor->consumer_cursors);
  disruptor_aligned_free(disruptor);
}

int disruptor_reset(disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return 0;
  }
  disruptor_init(disruptor);
  return 1;
}

uint64_t disruptor_capacity(const disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return 0U;
  }
  return disruptor->capacity;
}

size_t disruptor_entry_size(const disruptor_t *disruptor) {
  if (disruptor == NULL) {
    return 0U;
  }
  return disruptor->entry_size;
}

void *disruptor_acquire_entry(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  uint64_t index;
  if (disruptor == NULL || cursor == NULL) {
    return NULL;
  }
  index = disruptor_ring_index(disruptor, cursor->sequence);
  return disruptor->buffer + (index * disruptor->entry_size);
}

const void *disruptor_show_entry(const disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  uint64_t index;
  if (disruptor == NULL || cursor == NULL) {
    return NULL;
  }
  index = disruptor_ring_index(disruptor, cursor->sequence);
  return disruptor->buffer + (index * disruptor->entry_size);
}

int disruptor_consumer_try_register(disruptor_t *disruptor, disruptor_consumer_t *consumer,
                                    uint64_t *next_sequence) {
  uint32_t i;
  uint64_t vacant;
  uint64_t start_sequence;

  if (disruptor == NULL || consumer == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST) {
    return 0;
  }

  for (i = 0; i < disruptor->consumer_capacity; ++i) {
    vacant = DISRUPTOR_VACANT;
    start_sequence =
        atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_acquire);
    if (atomic_compare_exchange_weak_explicit(&disruptor->consumer_cursors[i].sequence, &vacant,
                                              start_sequence, memory_order_release,
                                              memory_order_relaxed)) {
      consumer->slot = i;
      if (next_sequence != NULL) {
        *next_sequence = start_sequence + 1U;
      }
      return 1;
    }
  }

  return 0;
}

uint64_t disruptor_consumer_register(disruptor_t *disruptor, disruptor_consumer_t *consumer) {
  unsigned int wait_rounds = 0U;
  uint64_t next_sequence = 0U;

  if (disruptor == NULL || consumer == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST) {
    return 0U;
  }

  while (!disruptor_consumer_try_register(disruptor, consumer, &next_sequence)) {
    disruptor_spin_backoff(&wait_rounds);
  }
  return next_sequence;
}

void disruptor_consumer_unregister(disruptor_t *disruptor, const disruptor_consumer_t *consumer) {
  if (disruptor == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST ||
      !disruptor_consumer_is_registered(disruptor, consumer)) {
    return;
  }

  atomic_store_explicit(&disruptor->consumer_cursors[consumer->slot].sequence, DISRUPTOR_VACANT,
                        memory_order_release);
  disruptor_refresh_slowest_reader(
      disruptor,
      1U + atomic_load_explicit(&disruptor->write_cursor.sequence, memory_order_relaxed));
}

int disruptor_consumer_wait_for_nonblocking(const disruptor_t *disruptor,
                                            disruptor_cursor_t *cursor) {
  return disruptor_consumer_wait_for_nonblocking_for(disruptor, NULL, cursor);
}

static uint64_t disruptor_consumer_available_sequence(const disruptor_t *disruptor,
                                                      const disruptor_consumer_t *consumer) {
  uint64_t available;
  uint32_t dep_count;

  available = atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire);
  if (consumer == NULL || consumer->slot >= disruptor->consumer_capacity) {
    return available;
  }

  dep_count = disruptor->consumer_dependency_counts[consumer->slot];
  for (uint32_t i = 0; i < dep_count; ++i) {
    uint32_t dep_slot =
        disruptor->consumer_dependencies[(consumer->slot * disruptor->consumer_capacity) + i];
    uint64_t dep_sequence =
        atomic_load_explicit(&disruptor->consumer_cursors[dep_slot].sequence,
                             memory_order_acquire);
    if (dep_sequence < available) {
      available = dep_sequence;
    }
  }

  return available;
}

int disruptor_consumer_wait_for_nonblocking_for(const disruptor_t *disruptor,
                                                const disruptor_consumer_t *consumer,
                                                disruptor_cursor_t *cursor) {
  uint64_t required_sequence;
  if (disruptor == NULL || cursor == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST ||
      (consumer != NULL && !disruptor_consumer_is_registered(disruptor, consumer))) {
    return 0;
  }

  required_sequence = cursor->sequence;
  {
    uint64_t available_sequence = disruptor_consumer_available_sequence(disruptor, consumer);
    if (required_sequence > available_sequence) {
      return 0;
    }
    cursor->sequence = available_sequence;
  }
  return 1;
}

void disruptor_consumer_wait_for_blocking(const disruptor_t *disruptor,
                                          disruptor_cursor_t *cursor) {
  disruptor_consumer_wait_for_blocking_for(disruptor, NULL, cursor);
}

void disruptor_consumer_wait_for_blocking_for(const disruptor_t *disruptor,
                                              const disruptor_consumer_t *consumer,
                                              disruptor_cursor_t *cursor) {
  uint64_t required_sequence;
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || cursor == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST ||
      (consumer != NULL && !disruptor_consumer_is_registered(disruptor, consumer))) {
    return;
  }

  required_sequence = cursor->sequence;
  while (required_sequence > disruptor_consumer_available_sequence(disruptor, consumer)) {
    disruptor_spin_backoff(&wait_rounds);
  }

  cursor->sequence = disruptor_consumer_available_sequence(disruptor, consumer);
}

void disruptor_consumer_release_entry(disruptor_t *disruptor, const disruptor_consumer_t *consumer,
                                      const disruptor_cursor_t *cursor) {
  if (disruptor == NULL || cursor == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST ||
      !disruptor_consumer_is_registered(disruptor, consumer)) {
    return;
  }

  atomic_store_explicit(&disruptor->consumer_cursors[consumer->slot].sequence, cursor->sequence,
                        memory_order_release);
}

/* Dependency configuration is O(V + E) time and O(V) temporary space. */
static int disruptor_dependencies_would_cycle(
    const disruptor_t *disruptor, uint32_t proposed_slot,
    const disruptor_consumer_t *proposed_dependencies, uint32_t proposed_dependency_count) {
  uint32_t capacity = disruptor->consumer_capacity;
  uint32_t *indegrees = (uint32_t *)calloc(capacity, sizeof(uint32_t));
  uint32_t *queue = (uint32_t *)malloc(sizeof(uint32_t) * capacity);
  uint32_t head = 0U;
  uint32_t tail = 0U;
  uint32_t visited = 0U;

  if (indegrees == NULL || queue == NULL) {
    free(queue);
    free(indegrees);
    return 1;
  }

  for (uint32_t slot = 0; slot < capacity; ++slot) {
    uint32_t dependency_count = slot == proposed_slot
                                    ? proposed_dependency_count
                                    : disruptor->consumer_dependency_counts[slot];
    for (uint32_t i = 0; i < dependency_count; ++i) {
      uint32_t dependency_slot =
          slot == proposed_slot
              ? proposed_dependencies[i].slot
              : disruptor->consumer_dependencies[(slot * capacity) + i];
      ++indegrees[dependency_slot];
    }
  }
  for (uint32_t slot = 0; slot < capacity; ++slot) {
    if (indegrees[slot] == 0U) {
      queue[tail++] = slot;
    }
  }

  while (head < tail) {
    uint32_t slot = queue[head++];
    uint32_t dependency_count = slot == proposed_slot
                                    ? proposed_dependency_count
                                    : disruptor->consumer_dependency_counts[slot];
    ++visited;
    for (uint32_t i = 0; i < dependency_count; ++i) {
      uint32_t dependency_slot =
          slot == proposed_slot
              ? proposed_dependencies[i].slot
              : disruptor->consumer_dependencies[(slot * capacity) + i];
      if (--indegrees[dependency_slot] == 0U) {
        queue[tail++] = dependency_slot;
      }
    }
  }

  free(queue);
  free(indegrees);
  return visited != capacity;
}

int disruptor_consumer_set_dependencies(disruptor_t *disruptor,
                                        const disruptor_consumer_t *consumer,
                                        const disruptor_consumer_t *dependencies,
                                        uint32_t dependency_count) {
  uint32_t base;

  if (disruptor == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST ||
      !disruptor_consumer_is_registered(disruptor, consumer)) {
    return 0;
  }
  if (dependency_count > disruptor->consumer_capacity) {
    return 0;
  }
  if (dependency_count != 0U && dependencies == NULL) {
    return 0;
  }

  base = consumer->slot * disruptor->consumer_capacity;
  for (uint32_t i = 0; i < dependency_count; ++i) {
    if (!disruptor_consumer_is_registered(disruptor, &dependencies[i]) ||
        dependencies[i].slot == consumer->slot) {
      return 0;
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (dependencies[j].slot == dependencies[i].slot) {
        return 0;
      }
    }
  }
  if (disruptor_dependencies_would_cycle(disruptor, consumer->slot, dependencies,
                                         dependency_count)) {
    return 0;
  }

  memset(&disruptor->consumer_dependencies[base], 0,
         sizeof(uint32_t) * disruptor->consumer_capacity);
  for (uint32_t i = 0; i < dependency_count; ++i) {
    disruptor->consumer_dependencies[base + i] = dependencies[i].slot;
  }
  disruptor->consumer_dependency_counts[consumer->slot] = dependency_count;
  return 1;
}

disruptor_topology_t *disruptor_topology_create(disruptor_t *disruptor) {
  disruptor_topology_t *topology;
  uint32_t capacity;
  size_t matrix_bytes;

  if (disruptor == NULL || disruptor->mode != DISRUPTOR_MODE_BROADCAST ||
      disruptor->consumer_capacity == 0U) {
    return NULL;
  }

  capacity = disruptor->consumer_capacity;
  if ((size_t)capacity > (SIZE_MAX / (size_t)capacity)) {
    return NULL;
  }
  matrix_bytes = (size_t)capacity * (size_t)capacity;

  topology = (disruptor_topology_t *)calloc(1, sizeof(*topology));
  if (topology == NULL) {
    return NULL;
  }

  topology->stages =
      (disruptor_topology_stage_def_t *)calloc(capacity, sizeof(*topology->stages));
  topology->group_names = (const char **)calloc(capacity, sizeof(*topology->group_names));
  topology->edges = (uint8_t *)calloc(1U, matrix_bytes);
  topology->group_members = (uint8_t *)calloc(1U, matrix_bytes);
  if (topology->stages == NULL || topology->group_names == NULL || topology->edges == NULL ||
      topology->group_members == NULL) {
    disruptor_topology_destroy(topology);
    return NULL;
  }

  topology->disruptor = disruptor;
  topology->stage_capacity = capacity;
  topology->group_capacity = capacity;
  return topology;
}

void disruptor_topology_destroy(disruptor_topology_t *topology) {
  if (topology == NULL) {
    return;
  }

  free(topology->group_members);
  free(topology->edges);
  free(topology->group_names);
  free(topology->stages);
  free(topology);
}

static int disruptor_topology_stage_valid(const disruptor_topology_t *topology,
                                          disruptor_stage_t stage) {
  return topology != NULL && stage < topology->stage_count;
}

static int disruptor_topology_group_valid(const disruptor_topology_t *topology,
                                          disruptor_group_t group) {
  return topology != NULL && group < topology->group_count;
}

disruptor_stage_t disruptor_topology_stage(disruptor_topology_t *topology,
                                           const char *name,
                                           const disruptor_consumer_t *consumer) {
  disruptor_stage_t stage;

  if (topology == NULL || !disruptor_consumer_is_registered(topology->disruptor, consumer) ||
      topology->stage_count >= topology->stage_capacity) {
    return DISRUPTOR_STAGE_INVALID;
  }

  for (uint32_t i = 0; i < topology->stage_count; ++i) {
    if (topology->stages[i].consumer.slot == consumer->slot) {
      return DISRUPTOR_STAGE_INVALID;
    }
  }

  stage = topology->stage_count++;
  topology->stages[stage].name = name;
  topology->stages[stage].consumer = *consumer;
  return stage;
}

disruptor_group_t disruptor_topology_group(disruptor_topology_t *topology,
                                           const char *name,
                                           const disruptor_stage_t *stages,
                                           uint32_t stage_count) {
  disruptor_group_t group;
  uint32_t capacity;

  if (topology == NULL || stages == NULL || stage_count == 0U ||
      topology->group_count >= topology->group_capacity) {
    return DISRUPTOR_GROUP_INVALID;
  }

  for (uint32_t i = 0; i < stage_count; ++i) {
    if (!disruptor_topology_stage_valid(topology, stages[i])) {
      return DISRUPTOR_GROUP_INVALID;
    }
  }

  group = topology->group_count++;
  capacity = topology->stage_capacity;
  topology->group_names[group] = name;
  for (uint32_t i = 0; i < stage_count; ++i) {
    topology->group_members[disruptor_topology_index(group, stages[i], capacity)] = 1U;
  }
  return group;
}

int disruptor_topology_after(disruptor_topology_t *topology,
                             disruptor_stage_t stage,
                             disruptor_stage_t dependency) {
  if (!disruptor_topology_stage_valid(topology, stage) ||
      !disruptor_topology_stage_valid(topology, dependency) || stage == dependency) {
    return 0;
  }

  topology->edges[disruptor_topology_index(stage, dependency, topology->stage_capacity)] = 1U;
  return 1;
}

int disruptor_topology_after_all(disruptor_topology_t *topology,
                                 disruptor_stage_t stage,
                                 const disruptor_stage_t *dependencies,
                                 uint32_t dependency_count) {
  if (topology == NULL || (dependency_count != 0U && dependencies == NULL)) {
    return 0;
  }

  for (uint32_t i = 0; i < dependency_count; ++i) {
    if (!disruptor_topology_after(topology, stage, dependencies[i])) {
      return 0;
    }
  }
  return 1;
}

int disruptor_topology_stage_after_group(disruptor_topology_t *topology,
                                         disruptor_stage_t stage,
                                         disruptor_group_t dependency_group) {
  uint32_t capacity;

  if (!disruptor_topology_stage_valid(topology, stage) ||
      !disruptor_topology_group_valid(topology, dependency_group)) {
    return 0;
  }

  capacity = topology->stage_capacity;
  for (uint32_t dep = 0; dep < topology->stage_count; ++dep) {
    if (topology->group_members[disruptor_topology_index(dependency_group, dep, capacity)] &&
        !disruptor_topology_after(topology, stage, dep)) {
      return 0;
    }
  }
  return 1;
}

int disruptor_topology_group_after(disruptor_topology_t *topology,
                                   disruptor_group_t group,
                                   disruptor_stage_t dependency) {
  uint32_t capacity;

  if (!disruptor_topology_group_valid(topology, group) ||
      !disruptor_topology_stage_valid(topology, dependency)) {
    return 0;
  }

  capacity = topology->stage_capacity;
  for (uint32_t stage = 0; stage < topology->stage_count; ++stage) {
    if (topology->group_members[disruptor_topology_index(group, stage, capacity)] &&
        !disruptor_topology_after(topology, stage, dependency)) {
      return 0;
    }
  }
  return 1;
}

int disruptor_topology_group_after_group(disruptor_topology_t *topology,
                                         disruptor_group_t group,
                                         disruptor_group_t dependency_group) {
  uint32_t capacity;

  if (!disruptor_topology_group_valid(topology, group) ||
      !disruptor_topology_group_valid(topology, dependency_group)) {
    return 0;
  }

  capacity = topology->stage_capacity;
  for (uint32_t stage = 0; stage < topology->stage_count; ++stage) {
    if (!topology->group_members[disruptor_topology_index(group, stage, capacity)]) {
      continue;
    }
    for (uint32_t dep = 0; dep < topology->stage_count; ++dep) {
      if (topology->group_members[disruptor_topology_index(dependency_group, dep, capacity)] &&
          !disruptor_topology_after(topology, stage, dep)) {
        return 0;
      }
    }
  }
  return 1;
}

int disruptor_topology_chain(disruptor_topology_t *topology,
                             const disruptor_stage_t *stages,
                             uint32_t stage_count) {
  if (topology == NULL || stages == NULL || stage_count == 0U) {
    return 0;
  }

  for (uint32_t i = 1; i < stage_count; ++i) {
    if (!disruptor_topology_after(topology, stages[i], stages[i - 1U])) {
      return 0;
    }
  }
  return 1;
}

/* The topology uses an adjacency matrix, so Kahn validation is O(V^2) time and O(V) space. */
static int disruptor_topology_has_cycle(const disruptor_topology_t *topology) {
  uint32_t count = topology->stage_count;
  uint32_t capacity = topology->stage_capacity;
  uint32_t *indegrees = (uint32_t *)calloc(count, sizeof(uint32_t));
  uint32_t *queue = (uint32_t *)malloc(sizeof(uint32_t) * count);
  uint32_t head = 0U;
  uint32_t tail = 0U;
  uint32_t visited = 0U;

  if (indegrees == NULL || queue == NULL) {
    free(queue);
    free(indegrees);
    return 1;
  }

  for (uint32_t stage = 0; stage < count; ++stage) {
    for (uint32_t dependency = 0; dependency < count; ++dependency) {
      if (topology->edges[disruptor_topology_index(stage, dependency, capacity)]) {
        ++indegrees[dependency];
      }
    }
  }
  for (uint32_t stage = 0; stage < count; ++stage) {
    if (indegrees[stage] == 0U) {
      queue[tail++] = stage;
    }
  }
  while (head < tail) {
    uint32_t stage = queue[head++];
    ++visited;
    for (uint32_t dependency = 0; dependency < count; ++dependency) {
      if (topology->edges[disruptor_topology_index(stage, dependency, capacity)] &&
          --indegrees[dependency] == 0U) {
        queue[tail++] = dependency;
      }
    }
  }

  free(queue);
  free(indegrees);
  return visited != count;
}

int disruptor_topology_commit(disruptor_topology_t *topology) {
  uint32_t capacity;

  if (topology == NULL || topology->stage_count == 0U) {
    return 0;
  }
  if (disruptor_topology_has_cycle(topology)) {
    return 0;
  }

  capacity = topology->stage_capacity;
  for (uint32_t stage = 0; stage < topology->stage_count; ++stage) {
    if (!disruptor_consumer_is_registered(topology->disruptor,
                                          &topology->stages[stage].consumer)) {
      return 0;
    }
  }

  for (uint32_t stage = 0; stage < topology->stage_count; ++stage) {
    uint32_t slot = topology->stages[stage].consumer.slot;
    uint32_t base = slot * capacity;
    uint32_t dependency_count = 0;
    memset(&topology->disruptor->consumer_dependencies[base], 0,
           sizeof(uint32_t) * capacity);
    for (uint32_t dep = 0; dep < topology->stage_count; ++dep) {
      if (topology->edges[disruptor_topology_index(stage, dep, capacity)]) {
        topology->disruptor->consumer_dependencies[base + dependency_count++] =
            topology->stages[dep].consumer.slot;
      }
    }
    topology->disruptor->consumer_dependency_counts[slot] = dependency_count;
  }

  return 1;
}

static int disruptor_range_is_valid(const disruptor_sequence_range_t *range) {
  return range != NULL && range->first_sequence != 0U &&
         range->last_sequence >= range->first_sequence;
}

static int disruptor_try_claim_range_internal(disruptor_t *disruptor, uint32_t count,
                                              disruptor_sequence_range_t *range) {
  uint64_t writer_last;
  uint64_t first_sequence;
  uint64_t last_sequence;
  uint64_t slowest_sequence;
  uint64_t expected;

  if (disruptor == NULL || range == NULL || count == 0U) {
    return 0;
  }
  if ((uint64_t)count > disruptor->capacity) {
    return 0;
  }

  writer_last = atomic_load_explicit(&disruptor->write_cursor.sequence, memory_order_relaxed);
  first_sequence = writer_last + 1U;
  if (first_sequence == 0U || first_sequence > (UINT64_MAX - ((uint64_t)count - 1U))) {
    return 0;
  }
  last_sequence = first_sequence + ((uint64_t)count - 1U);

  slowest_sequence =
      atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_acquire);
  if (!disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence)) {
    slowest_sequence = disruptor_refresh_slowest_reader(disruptor, last_sequence);
    if (!disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence)) {
      return 0;
    }
  }

  expected = writer_last;
  if (!atomic_compare_exchange_strong_explicit(&disruptor->write_cursor.sequence, &expected,
                                               writer_last + (uint64_t)count, memory_order_relaxed,
                                               memory_order_relaxed)) {
    return 0;
  }

  range->first_sequence = first_sequence;
  range->last_sequence = last_sequence;
  return 1;
}

static int disruptor_claim_range_blocking_internal(disruptor_t *disruptor, uint32_t count,
                                                   disruptor_sequence_range_t *range) {
  uint64_t first_sequence;
  uint64_t last_sequence;
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || range == NULL || count == 0U) {
    return 0;
  }
  if ((uint64_t)count > disruptor->capacity) {
    return 0;
  }

  first_sequence = 1U + atomic_fetch_add_explicit(&disruptor->write_cursor.sequence,
                                                  (uint64_t)count, memory_order_relaxed);
  if (first_sequence == 0U || first_sequence > (UINT64_MAX - ((uint64_t)count - 1U))) {
    return 0;
  }
  last_sequence = first_sequence + ((uint64_t)count - 1U);

  while (1) {
    uint64_t slowest_sequence =
        atomic_load_explicit(&disruptor->slowest_consumer.sequence, memory_order_acquire);
    if (DISRUPTOR_LIKELY(
            disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence))) {
      range->first_sequence = first_sequence;
      range->last_sequence = last_sequence;
      return 1;
    }

    if ((wait_rounds & 15U) == 0U) {
      slowest_sequence = disruptor_refresh_slowest_reader(disruptor, last_sequence);
      if (DISRUPTOR_LIKELY(
              disruptor_publisher_has_capacity(disruptor, last_sequence, slowest_sequence))) {
        range->first_sequence = first_sequence;
        range->last_sequence = last_sequence;
        return 1;
      }
    }

    disruptor_spin_backoff_producer(&wait_rounds);
  }
}

static int disruptor_publish_range_internal(disruptor_t *disruptor,
                                            const disruptor_sequence_range_t *range,
                                            int wait_until_visible, int report_visibility) {
  uint64_t expected;
  uint64_t sequence;
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || !disruptor_range_is_valid(range)) {
    return 0;
  }

  expected = range->first_sequence - 1U;
  if (atomic_compare_exchange_strong_explicit(&disruptor->max_read_cursor.sequence, &expected,
                                              range->last_sequence, memory_order_release,
                                              memory_order_relaxed)) {
    (void)disruptor_try_advance_published_cursor(disruptor);
    disruptor_worker_notify(disruptor);
    if (report_visibility) {
      return atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire) >=
             range->last_sequence;
    }
    return 1;
  }

  sequence = range->first_sequence;
  while (1) {
    disruptor_mark_published(disruptor, sequence);
    if (sequence == range->last_sequence) {
      break;
    }
    ++sequence;
  }

  if (wait_until_visible) {
    while (atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire) <
           range->last_sequence) {
      (void)disruptor_try_advance_published_cursor(disruptor);
      disruptor_spin_backoff_producer(&wait_rounds);
    }
    disruptor_worker_notify(disruptor);
    return 1;
  }

  (void)disruptor_try_advance_published_cursor(disruptor);
  disruptor_worker_notify(disruptor);
  if (report_visibility) {
    return atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire) >=
           range->last_sequence;
  }

  return 1;
}

int disruptor_publisher_try_claim(disruptor_t *disruptor, disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;

  if (cursor == NULL) {
    return 0;
  }
  if (!disruptor_try_claim_range_internal(disruptor, 1U, &range)) {
    return 0;
  }
  cursor->sequence = range.first_sequence;
  return 1;
}

int disruptor_publisher_try_claim_n(disruptor_t *disruptor, uint32_t count,
                                    disruptor_sequence_range_t *range) {
  return disruptor_try_claim_range_internal(disruptor, count, range);
}

void disruptor_publisher_next_entry_blocking(disruptor_t *disruptor, disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL) {
    return;
  }
  if (!disruptor_claim_range_blocking_internal(disruptor, 1U, &range)) {
    return;
  }
  cursor->sequence = range.first_sequence;
}

int disruptor_publisher_claim_n_blocking(disruptor_t *disruptor, uint32_t count,
                                         disruptor_sequence_range_t *range) {
  return disruptor_claim_range_blocking_internal(disruptor, count, range);
}

void *disruptor_publisher_next_entry_and_acquire_blocking(disruptor_t *disruptor,
                                                          disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  uint64_t index;

  if (disruptor == NULL || cursor == NULL) {
    return NULL;
  }
  if (!disruptor_claim_range_blocking_internal(disruptor, 1U, &range)) {
    return NULL;
  }

  cursor->sequence = range.first_sequence;
  index = disruptor_ring_index(disruptor, range.first_sequence);
  return disruptor->buffer + (index * disruptor->entry_size);
}

int disruptor_publisher_try_commit(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL || cursor->sequence == 0U) {
    return 0;
  }
  range.first_sequence = cursor->sequence;
  range.last_sequence = cursor->sequence;
  return disruptor_publish_range_internal(disruptor, &range, 0, 1);
}

void disruptor_publisher_commit_entry_blocking(disruptor_t *disruptor,
                                               const disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL || cursor->sequence == 0U) {
    return;
  }
  range.first_sequence = cursor->sequence;
  range.last_sequence = cursor->sequence;
  (void)disruptor_publish_range_internal(disruptor, &range, 1, 0);
}

int disruptor_publisher_publish_range(disruptor_t *disruptor,
                                      const disruptor_sequence_range_t *range) {
  return disruptor_publish_range_internal(disruptor, range, 0, 0);
}

void disruptor_publisher_commit_range_blocking(disruptor_t *disruptor,
                                               const disruptor_sequence_range_t *range) {
  (void)disruptor_publish_range_internal(disruptor, range, 1, 0);
}

int disruptor_publisher_publish(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  disruptor_sequence_range_t range;
  if (cursor == NULL || cursor->sequence == 0U) {
    return 0;
  }
  range.first_sequence = cursor->sequence;
  range.last_sequence = cursor->sequence;
  return disruptor_publish_range_internal(disruptor, &range, 0, 0);
}

int disruptor_worker_try_claim(disruptor_t *disruptor, disruptor_cursor_t *cursor) {
  uint64_t claim_sequence;
  uint64_t available_sequence;

  if (disruptor == NULL || cursor == NULL || disruptor->mode != DISRUPTOR_MODE_WORKER_POOL) {
    return 0;
  }

  claim_sequence =
      atomic_load_explicit(&disruptor->worker_claim_cursor.sequence, memory_order_relaxed);
  for (;;) {
    available_sequence =
        atomic_load_explicit(&disruptor->max_read_cursor.sequence, memory_order_acquire);
    if (claim_sequence > available_sequence) {
      return 0;
    }

    if (atomic_compare_exchange_weak_explicit(&disruptor->worker_claim_cursor.sequence,
                                              &claim_sequence, claim_sequence + 1U,
                                              memory_order_relaxed, memory_order_relaxed)) {
      cursor->sequence = claim_sequence;
      return 1;
    }
  }
}

void disruptor_worker_claim_blocking(disruptor_t *disruptor, disruptor_cursor_t *cursor) {
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || cursor == NULL || disruptor->mode != DISRUPTOR_MODE_WORKER_POOL) {
    return;
  }

  while (!disruptor_worker_try_claim(disruptor, cursor)) {
    disruptor_spin_backoff(&wait_rounds);
  }
}

int disruptor_worker_claim_wait(disruptor_t *disruptor, disruptor_cursor_t *cursor,
                                disruptor_keep_running_fn keep_running, void *ctx) {
  unsigned int wait_rounds = 0U;

  if (disruptor == NULL || cursor == NULL || keep_running == NULL ||
      disruptor->mode != DISRUPTOR_MODE_WORKER_POOL) {
    return 0;
  }

  while (wait_rounds < DISRUPTOR_WORKER_PARK_SPIN_ROUNDS) {
    if (disruptor_worker_try_claim(disruptor, cursor)) return 1;
    if (!keep_running(ctx)) return 0;
    ++wait_rounds;
    disruptor_spin_pause();
  }

  cmeta_mutex_lock(&disruptor->worker_wait_mutex);
  atomic_fetch_add_explicit(&disruptor->worker_waiters, 1U, memory_order_acq_rel);
  for (;;) {
    if (disruptor_worker_try_claim(disruptor, cursor)) {
      atomic_fetch_sub_explicit(&disruptor->worker_waiters, 1U, memory_order_acq_rel);
      cmeta_mutex_unlock(&disruptor->worker_wait_mutex);
      return 1;
    }
    if (!keep_running(ctx)) {
      atomic_fetch_sub_explicit(&disruptor->worker_waiters, 1U, memory_order_acq_rel);
      cmeta_mutex_unlock(&disruptor->worker_wait_mutex);
      return 0;
    }
    cmeta_cond_wait(&disruptor->worker_wait_cond, &disruptor->worker_wait_mutex);
  }
}

void disruptor_worker_wake_all(disruptor_t *disruptor) {
  disruptor_worker_notify(disruptor);
}

void disruptor_worker_release_entry(disruptor_t *disruptor, const disruptor_cursor_t *cursor) {
  if (disruptor == NULL || cursor == NULL || cursor->sequence == 0U ||
      disruptor->mode != DISRUPTOR_MODE_WORKER_POOL) {
    return;
  }

  disruptor_mark_worker_completed(disruptor, cursor->sequence);
  (void)disruptor_try_advance_worker_completed_cursor(disruptor);
}

// =============================================================================
// Generic Consumer Loop
// =============================================================================

static void disruptor_idle_sleep(void) {
#ifdef _WIN32
  Sleep(1);
#else
  usleep(1000);
#endif
}

void disruptor_consumer_run(disruptor_t *disruptor,
                            disruptor_consumer_t *consumer,
                            disruptor_keep_running_fn keep_running,
                            disruptor_batch_fn process_batch,
                            void *ctx) {
  if (!disruptor || disruptor->mode != DISRUPTOR_MODE_BROADCAST || !consumer || !keep_running ||
      !process_batch) {
    return;
  }

  uint64_t next_sequence = disruptor_consumer_register(disruptor, consumer);

  while (keep_running(ctx)) {
    disruptor_cursor_t cursor;
    cursor.sequence = next_sequence;

    if (!disruptor_consumer_wait_for_nonblocking_for(disruptor, consumer, &cursor)) {
      disruptor_idle_sleep();
      continue;
    }

    process_batch(ctx, next_sequence, cursor.sequence);

    disruptor_consumer_release_entry(disruptor, consumer, &cursor);
    next_sequence = cursor.sequence + 1;
  }

  // Final drain: process any entries published before shutdown was noticed
  {
    disruptor_cursor_t drain_cursor;
    drain_cursor.sequence = next_sequence;
    if (disruptor_consumer_wait_for_nonblocking_for(disruptor, consumer, &drain_cursor)) {
      process_batch(ctx, next_sequence, drain_cursor.sequence);
      disruptor_consumer_release_entry(disruptor, consumer, &drain_cursor);
    }
  }

  disruptor_consumer_unregister(disruptor, consumer);
}
