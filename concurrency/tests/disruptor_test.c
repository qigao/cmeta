#include "disruptor.h"
#include "tinytest.h"
#include "cmeta_thread.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>


#define ENTRY_SIZE sizeof(uint64_t)
#define CAPACITY 32
#define CONSUMERS 2

typedef struct disruptor_worker_wait_test_s {
  disruptor_t *disruptor;
  atomic_int running;
  atomic_int entered;
  atomic_int result;
  disruptor_cursor_t cursor;
} disruptor_worker_wait_test_t;

static int disruptor_test_keep_running(void *ctx) {
  disruptor_worker_wait_test_t *test = (disruptor_worker_wait_test_t *)ctx;
  atomic_store_explicit(&test->entered, 1, memory_order_release);
  return atomic_load_explicit(&test->running, memory_order_acquire);
}

static void disruptor_test_worker_wait(void *ctx) {
  disruptor_worker_wait_test_t *test = (disruptor_worker_wait_test_t *)ctx;
  int result = disruptor_worker_claim_wait(test->disruptor, &test->cursor,
                                           disruptor_test_keep_running, test);
  atomic_store_explicit(&test->result, result, memory_order_release);
}

enum { HANDOFF_THREADS = 4, HANDOFF_ROUNDS = 1024, HANDOFF_DEADLINE_MS = 10000 };
typedef struct disruptor_handoff_lane {
  disruptor_cursor_t cursor;
  cmeta_thread_t thread;
  int result;
} disruptor_handoff_lane;
static struct {
  disruptor_t *ring;
  disruptor_mode_t mode;
  atomic_uint phase;
  atomic_size_t finished;
  atomic_bool abort;
  disruptor_handoff_lane lane[HANDOFF_THREADS];
  size_t started;
} handoff;

static void disruptor_test_handoff(void *ctx) {
  disruptor_handoff_lane *lane = ctx;
  unsigned int previous = 0u;
  for (;;) {
    unsigned int phase;
    do {
      if (atomic_load_explicit(&handoff.abort, memory_order_acquire)) return;
      phase = atomic_load_explicit(&handoff.phase, memory_order_acquire);
      if (phase == previous) cmeta_thread_yield();
    } while (phase == previous);
    previous = phase;
    lane->result = 1;
    if ((phase & 1u) != 0u) {
      *(uint64_t *)disruptor_acquire_entry(handoff.ring, &lane->cursor) = lane->cursor.sequence;
      if ((lane->cursor.sequence & 1u) != 0u) cmeta_thread_yield();
      lane->result = disruptor_publisher_publish(handoff.ring, &lane->cursor);
    } else if (handoff.mode == DISRUPTOR_MODE_WORKER_POOL) {
      if ((lane->cursor.sequence & 1u) != 0u) cmeta_thread_yield();
      disruptor_worker_release_entry(handoff.ring, &lane->cursor);
    }
    atomic_fetch_add_explicit(&handoff.finished, 1u, memory_order_acq_rel);
  }
}

static void disruptor_test_handoff_barrier(unsigned int phase) {
  const size_t expected = (size_t)phase * HANDOFF_THREADS;
  const uint64_t deadline = cmeta_monotonic_ms() + HANDOFF_DEADLINE_MS;
  atomic_store_explicit(&handoff.phase, phase, memory_order_release);
  while (atomic_load_explicit(&handoff.finished, memory_order_acquire) != expected &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
  check_equal(atomic_load_explicit(&handoff.finished, memory_order_acquire), expected);
  for (size_t i = 0u; i < HANDOFF_THREADS; ++i) check_equal(handoff.lane[i].result, 1);
}

spec("Disruptor Tests") {
  group("concurrent publication and completion cursor handoff") {
    before_each() {
      memset(&handoff, 0, sizeof(handoff));
      atomic_init(&handoff.phase, 0u);
      atomic_init(&handoff.finished, 0u);
      atomic_init(&handoff.abort, false);
    }
    after_each() {
      atomic_store_explicit(&handoff.abort, true, memory_order_release);
      while (handoff.started != 0u) {
        disruptor_handoff_lane *lane = &handoff.lane[handoff.started - 1u];
        check_equal(cmeta_thread_join(&lane->thread), SALTS_OK);
        cmeta_thread_destroy(&lane->thread);
        --handoff.started;
      }
      /* All publishers/releases have returned, including on fatal checks. */
      disruptor_destroy(handoff.ring);
      handoff.ring = NULL;
    }
    for (size_t worker_mode = 0u; worker_mode < 2u; ++worker_mode) {
      it("exposes and reuses every completed batch in %s mode",
         worker_mode != 0u ? "worker-pool" : "broadcast") {
        disruptor_consumer_t consumer = {0};
        handoff.mode = worker_mode != 0u ? DISRUPTOR_MODE_WORKER_POOL : DISRUPTOR_MODE_BROADCAST;
        const disruptor_config_t config = {sizeof(uint64_t), HANDOFF_THREADS, 1u, handoff.mode};
        handoff.ring = disruptor_create(&config);
        check_not_null(handoff.ring);
        if (handoff.mode == DISRUPTOR_MODE_BROADCAST)
          check_equal(disruptor_consumer_register(handoff.ring, &consumer), UINT64_C(1));
        for (size_t i = 0u; i < HANDOFF_THREADS; ++i) {
          check_equal(cmeta_thread_create(&handoff.lane[i].thread,
                                         disruptor_test_handoff, &handoff.lane[i]), SALTS_OK);
          ++handoff.started;
        }
        for (unsigned int round = 0u; round < HANDOFF_ROUNDS; ++round) {
          disruptor_sequence_range_t range = {0};
          /* Exclusive claims isolate publication/completion from try-claim
           * contention. Each full batch must drain without a later publisher
           * rescuing a cursor left behind by an out-of-order completion. */
          check_equal(disruptor_publisher_try_claim_n(handoff.ring, HANDOFF_THREADS, &range), 1);
          for (size_t i = 0u; i < HANDOFF_THREADS; ++i)
            handoff.lane[i].cursor.sequence = range.first_sequence + i;
          disruptor_test_handoff_barrier(round * 2u + 1u);
          if (handoff.mode == DISRUPTOR_MODE_WORKER_POOL) {
            for (size_t i = 0u; i < HANDOFF_THREADS; ++i) {
              disruptor_cursor_t read = {0};
              check_equal(disruptor_worker_try_claim(handoff.ring, &read), 1);
              check_equal(read.sequence, range.first_sequence + i);
              check_equal(*(const uint64_t *)disruptor_show_entry(handoff.ring, &read), read.sequence);
              handoff.lane[HANDOFF_THREADS - 1u - i].cursor = read;
            }
            disruptor_cursor_t empty = {0};
            check_equal(disruptor_worker_try_claim(handoff.ring, &empty), 0);
          } else {
            disruptor_cursor_t read = {range.first_sequence};
            check_equal(disruptor_consumer_wait_for_nonblocking_for(handoff.ring, &consumer, &read), 1);
            check_equal(read.sequence, range.last_sequence);
            for (size_t i = 0u; i < HANDOFF_THREADS; ++i) {
              const disruptor_cursor_t entry = {range.first_sequence + i};
              check_equal(*(const uint64_t *)disruptor_show_entry(handoff.ring, &entry), entry.sequence);
            }
            disruptor_consumer_release_entry(handoff.ring, &consumer, &read);
          }
          disruptor_test_handoff_barrier(round * 2u + 2u);
        }
      }
    }
  }
  it("should create and destroy cleanly") {
    disruptor_config_t cfg = {
        .entry_size = ENTRY_SIZE, .capacity = CAPACITY, .consumer_capacity = CONSUMERS};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);
    check_equal(disruptor_capacity(d), CAPACITY);
    check_equal(disruptor_entry_size(d), ENTRY_SIZE);

    disruptor_destroy(d);
  }

  it("should handle invalid config gracefully") {
    disruptor_config_t cfg1 = {
        .entry_size = 0, .capacity = CAPACITY, .consumer_capacity = CONSUMERS};
    check_null(disruptor_create(&cfg1));

    disruptor_config_t cfg2 = {.entry_size = ENTRY_SIZE,
                               .capacity = 31, // Not power of two
                               .consumer_capacity = CONSUMERS};
    check_null(disruptor_create(&cfg2));
  }

  it("should make every configured ring slot usable") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 1, .consumer_capacity = 1};
    disruptor_t *d = disruptor_create(&cfg);
    disruptor_consumer_t consumer;
    disruptor_cursor_t claimed = {0};
    disruptor_cursor_t extra = {0};
    disruptor_cursor_t read = {.sequence = 1};

    check_not_null(d);
    check_equal(disruptor_consumer_register(d, &consumer), 1);
    check_equal(disruptor_publisher_try_claim(d, &claimed), 1);
    check_equal(claimed.sequence, 1);
    check_equal(disruptor_publisher_try_claim(d, &extra), 0);
    *(uint64_t *)disruptor_acquire_entry(d, &claimed) = 17;
    check_equal(disruptor_publisher_publish(d, &claimed), 1);
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &read), 1);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &read), 17);
    disruptor_consumer_release_entry(d, &consumer, &read);
    check_equal(disruptor_publisher_try_claim(d, &claimed), 1);
    check_equal(claimed.sequence, 2);
    check_equal(disruptor_publisher_publish(d, &claimed), 1);
    read.sequence = 2;
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &read), 1);
    disruptor_consumer_release_entry(d, &consumer, &read);

    disruptor_destroy(d);
  }

  it("should publish and consume a single entry") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 1};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t c;
    disruptor_cursor_t w_cursor = {0};
    uint64_t next_seq;
    check_equal(disruptor_consumer_try_register(d, &c, &next_seq), 1);
    check_equal(next_seq, 1);
    check_equal(disruptor_worker_try_claim(d, &w_cursor), 0);

    w_cursor.sequence = 0;
    check_equal(disruptor_publisher_try_claim(d, &w_cursor), 1);
    check_equal(w_cursor.sequence, 1);

    uint64_t *entry = (uint64_t *)disruptor_acquire_entry(d, &w_cursor);
    check_not_null(entry);
    *entry = 42;

    check_equal(disruptor_publisher_publish(d, &w_cursor), 1);

    disruptor_cursor_t r_cursor = {.sequence = 1};
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &r_cursor), 1);
    check_equal(r_cursor.sequence, 1);

    const uint64_t *read_entry = (const uint64_t *)disruptor_show_entry(d, &r_cursor);
    check_not_null(read_entry);
    check_equal(*read_entry, 42);

    disruptor_consumer_release_entry(d, &c, &r_cursor);
    disruptor_consumer_unregister(d, &c);
    disruptor_destroy(d);
  }

  it("should publish and consume multiple entries block") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint32_t), .capacity = 8, .consumer_capacity = 1};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t c;
    disruptor_consumer_register(d, &c);

    disruptor_sequence_range_t range;
    check_equal(disruptor_publisher_try_claim_n(d, 4, &range), 1);
    check_equal(range.first_sequence, 1);
    check_equal(range.last_sequence, 4);

    for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
      disruptor_cursor_t wc = {.sequence = seq};
      uint32_t *entry = (uint32_t *)disruptor_acquire_entry(d, &wc);
      *entry = (uint32_t)seq * 10;
    }

    check_equal(disruptor_publisher_publish_range(d, &range), 1);

    disruptor_cursor_t rc = {.sequence = 1};
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &rc), 1);
    check_equal(rc.sequence, 4);

    for (uint64_t seq = 1; seq <= rc.sequence; ++seq) {
      disruptor_cursor_t cur = {.sequence = seq};
      const uint32_t *read_entry = (const uint32_t *)disruptor_show_entry(d, &cur);
      check_equal(*read_entry, seq * 10);
    }

    disruptor_consumer_release_entry(d, &c, &rc);
    disruptor_destroy(d);
  }

  it("should broadcast entries to every consumer") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 2};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t c1, c2;
    uint64_t next1 = 0, next2 = 0;
    check_equal(disruptor_consumer_try_register(d, &c1, &next1), 1);
    check_equal(disruptor_consumer_try_register(d, &c2, &next2), 1);
    check_equal(next1, 1);
    check_equal(next2, 1);

    disruptor_cursor_t w = {0};
    check_equal(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 7;
    check_equal(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t r1 = {.sequence = next1};
    disruptor_cursor_t r2 = {.sequence = next2};
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &r1), 1);
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &r2), 1);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &r1), 7);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &r2), 7);

    disruptor_consumer_release_entry(d, &c1, &r1);
    disruptor_consumer_release_entry(d, &c2, &r2);
    disruptor_destroy(d);
  }

  it("should expose contiguous entries after an out-of-order publish closes the gap") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 4, .consumer_capacity = 1};
    disruptor_t *d = disruptor_create(&cfg);
    disruptor_consumer_t consumer;
    disruptor_cursor_t first = {0};
    disruptor_cursor_t second = {0};
    disruptor_cursor_t read = {.sequence = 1};

    check_not_null(d);
    check_equal(disruptor_consumer_register(d, &consumer), 1);
    check_equal(disruptor_publisher_try_claim(d, &first), 1);
    check_equal(disruptor_publisher_try_claim(d, &second), 1);
    check_equal(first.sequence, 1);
    check_equal(second.sequence, 2);
    *(uint64_t *)disruptor_acquire_entry(d, &first) = 11;
    *(uint64_t *)disruptor_acquire_entry(d, &second) = 22;

    check_equal(disruptor_publisher_publish(d, &second), 1);
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &read), 0);
    check_equal(disruptor_publisher_publish(d, &first), 1);
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &read), 1);
    check_equal(read.sequence, 2);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &second), 22);

    disruptor_consumer_release_entry(d, &consumer, &read);
    disruptor_destroy(d);
  }

  it("should assign worker-pool entries to one worker only") {
    disruptor_config_t cfg = {.entry_size = sizeof(uint64_t),
                              .capacity = 16,
                              .consumer_capacity = 1,
                              .mode = DISRUPTOR_MODE_WORKER_POOL};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_sequence_range_t range;
    check_equal(disruptor_publisher_try_claim_n(d, 4, &range), 1);
    for (uint64_t seq = range.first_sequence; seq <= range.last_sequence; ++seq) {
      disruptor_cursor_t w = {.sequence = seq};
      *(uint64_t *)disruptor_acquire_entry(d, &w) = seq * 100;
    }
    check_equal(disruptor_publisher_publish_range(d, &range), 1);

    bool seen[5] = {false};
    for (int i = 0; i < 4; ++i) {
      disruptor_cursor_t claim = {0};
      check_equal(disruptor_worker_try_claim(d, &claim), 1);
      check(claim.sequence >= 1 && claim.sequence <= 4);
      check(!seen[claim.sequence]);
      seen[claim.sequence] = true;
      check_equal(*(const uint64_t *)disruptor_show_entry(d, &claim), claim.sequence * 100);
      disruptor_worker_release_entry(d, &claim);
    }

    disruptor_cursor_t extra = {0};
    check_equal(disruptor_worker_try_claim(d, &extra), 0);
    for (int i = 1; i <= 4; ++i) {
      check(seen[i]);
    }

    disruptor_destroy(d);
  }

  it("should reject broadcast APIs in worker-pool mode") {
    disruptor_config_t cfg = {.entry_size = sizeof(uint64_t),
                              .capacity = 4,
                              .consumer_capacity = 1,
                              .mode = DISRUPTOR_MODE_WORKER_POOL};
    disruptor_t *d = disruptor_create(&cfg);
    disruptor_consumer_t consumer = {0};
    disruptor_cursor_t cursor = {.sequence = 1};
    uint64_t next_sequence = 99;

    check_not_null(d);
    check_equal(disruptor_consumer_try_register(d, &consumer, &next_sequence), 0);
    check_equal(next_sequence, 99);
    check_equal(disruptor_consumer_register(d, &consumer), 0);
    check_equal(disruptor_consumer_wait_for_nonblocking(d, &cursor), 0);
    check_null(disruptor_topology_create(d));

    disruptor_destroy(d);
  }

  it("should wake a parked worker when an entry is published") {
    disruptor_config_t cfg = {.entry_size = sizeof(uint64_t),
                              .capacity = 16,
                              .consumer_capacity = 1,
                              .mode = DISRUPTOR_MODE_WORKER_POOL};
    disruptor_worker_wait_test_t test;
    cmeta_thread_t worker = NULL;
    disruptor_cursor_t published = {0};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);
    memset(&test, 0, sizeof(test));
    test.disruptor = d;
    atomic_init(&test.running, 1);
    atomic_init(&test.entered, 0);
    atomic_init(&test.result, 0);
    check_equal(cmeta_thread_create(&worker, disruptor_test_worker_wait, &test), 0);
    while (!atomic_load_explicit(&test.entered, memory_order_acquire)) cmeta_thread_yield();
    check_equal(disruptor_publisher_try_claim(d, &published), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &published) = 42;
    check_equal(disruptor_publisher_publish(d, &published), 1);
    check_equal(cmeta_thread_join(&worker), 0);
    check_equal(atomic_load_explicit(&test.result, memory_order_acquire), 1);
    check_equal(test.cursor.sequence, published.sequence);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &test.cursor), 42);
    disruptor_worker_release_entry(d, &test.cursor);
    disruptor_destroy(d);
  }

  it("should interrupt a parked worker without claiming an entry") {
    disruptor_config_t cfg = {.entry_size = sizeof(uint64_t),
                              .capacity = 16,
                              .consumer_capacity = 1,
                              .mode = DISRUPTOR_MODE_WORKER_POOL};
    disruptor_worker_wait_test_t test;
    cmeta_thread_t worker = NULL;
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);
    memset(&test, 0, sizeof(test));
    test.disruptor = d;
    atomic_init(&test.running, 1);
    atomic_init(&test.entered, 0);
    atomic_init(&test.result, 1);
    check_equal(cmeta_thread_create(&worker, disruptor_test_worker_wait, &test), 0);
    while (!atomic_load_explicit(&test.entered, memory_order_acquire)) cmeta_thread_yield();
    atomic_store_explicit(&test.running, 0, memory_order_release);
    disruptor_worker_wake_all(d);
    check_equal(cmeta_thread_join(&worker), 0);
    check_equal(atomic_load_explicit(&test.result, memory_order_acquire), 0);
    check_equal(test.cursor.sequence, 0);
    disruptor_destroy(d);
  }

  it("should gate dependent consumers until upstream consumers release") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 3};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t branch_a, branch_b, joined;
    uint64_t next_a = 0, next_b = 0, next_joined = 0;
    check_equal(disruptor_consumer_try_register(d, &branch_a, &next_a), 1);
    check_equal(disruptor_consumer_try_register(d, &branch_b, &next_b), 1);
    check_equal(disruptor_consumer_try_register(d, &joined, &next_joined), 1);

    disruptor_consumer_t deps[2] = {branch_a, branch_b};
    check_equal(disruptor_consumer_set_dependencies(d, &joined, deps, 2), 1);

    disruptor_cursor_t w = {0};
    check_equal(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 99;
    check_equal(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t joined_cursor = {.sequence = next_joined};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &joined, &joined_cursor), 0);

    disruptor_cursor_t a_cursor = {.sequence = next_a};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &branch_a, &a_cursor), 1);
    disruptor_consumer_release_entry(d, &branch_a, &a_cursor);
    joined_cursor.sequence = next_joined;
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &joined, &joined_cursor), 0);

    disruptor_cursor_t b_cursor = {.sequence = next_b};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &branch_b, &b_cursor), 1);
    disruptor_consumer_release_entry(d, &branch_b, &b_cursor);

    joined_cursor.sequence = next_joined;
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &joined, &joined_cursor), 1);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &joined_cursor), 99);

    disruptor_consumer_release_entry(d, &joined, &joined_cursor);
    disruptor_destroy(d);
  }

  it("should reject a cycle in direct consumer dependencies") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 4, .consumer_capacity = 2};
    disruptor_t *d = disruptor_create(&cfg);
    disruptor_consumer_t first, second;
    disruptor_cursor_t published = {0};
    disruptor_cursor_t first_read = {.sequence = 1};
    disruptor_cursor_t second_read = {.sequence = 1};

    check_not_null(d);
    check_equal(disruptor_consumer_register(d, &first), 1);
    check_equal(disruptor_consumer_register(d, &second), 1);
    check_equal(disruptor_consumer_set_dependencies(d, &first, &second, 1), 1);
    check_equal(disruptor_consumer_set_dependencies(d, &second, &first, 1), 0);

    check_equal(disruptor_publisher_try_claim(d, &published), 1);
    check_equal(disruptor_publisher_publish(d, &published), 1);
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &first, &first_read), 0);
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &second, &second_read), 1);
    disruptor_consumer_release_entry(d, &second, &second_read);
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &first, &first_read), 1);

    disruptor_consumer_release_entry(d, &first, &first_read);
    disruptor_destroy(d);
  }

  it("should configure dependencies through topology chain") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 3};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t parse, validate, persist;
    uint64_t next_parse = disruptor_consumer_register(d, &parse);
    uint64_t next_validate = disruptor_consumer_register(d, &validate);
    uint64_t next_persist = disruptor_consumer_register(d, &persist);

    disruptor_topology_t *topology = disruptor_topology_create(d);
    check_not_null(topology);
    disruptor_stage_t s_parse = disruptor_topology_stage(topology, "parse", &parse);
    disruptor_stage_t s_validate = disruptor_topology_stage(topology, "validate", &validate);
    disruptor_stage_t s_persist = disruptor_topology_stage(topology, "persist", &persist);
    disruptor_stage_t chain[] = {s_parse, s_validate, s_persist};
    check_equal(disruptor_topology_chain(topology, chain, 3), 1);
    check_equal(disruptor_topology_commit(topology), 1);

    disruptor_cursor_t w = {0};
    check_equal(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 123;
    check_equal(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t persist_cursor = {.sequence = next_persist};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &persist, &persist_cursor), 0);

    disruptor_cursor_t parse_cursor = {.sequence = next_parse};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &parse, &parse_cursor), 1);
    disruptor_consumer_release_entry(d, &parse, &parse_cursor);
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &persist, &persist_cursor), 0);

    disruptor_cursor_t validate_cursor = {.sequence = next_validate};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &validate, &validate_cursor), 1);
    disruptor_consumer_release_entry(d, &validate, &validate_cursor);

    persist_cursor.sequence = next_persist;
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &persist, &persist_cursor), 1);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &persist_cursor), 123);

    disruptor_topology_destroy(topology);
    disruptor_destroy(d);
  }

  it("should expand topology group dependencies") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 4};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t parse, validate, enrich, sink;
    uint64_t next_parse = disruptor_consumer_register(d, &parse);
    uint64_t next_validate = disruptor_consumer_register(d, &validate);
    uint64_t next_enrich = disruptor_consumer_register(d, &enrich);
    uint64_t next_sink = disruptor_consumer_register(d, &sink);

    disruptor_topology_t *topology = disruptor_topology_create(d);
    check_not_null(topology);
    disruptor_stage_t s_parse = disruptor_topology_stage(topology, "parse", &parse);
    disruptor_stage_t s_validate = disruptor_topology_stage(topology, "validate", &validate);
    disruptor_stage_t s_enrich = disruptor_topology_stage(topology, "enrich", &enrich);
    disruptor_stage_t s_sink = disruptor_topology_stage(topology, "sink", &sink);
    disruptor_stage_t middle_stages[] = {s_validate, s_enrich};
    disruptor_group_t middle = disruptor_topology_group(topology, "middle", middle_stages, 2);

    check_equal(disruptor_topology_group_after(topology, middle, s_parse), 1);
    check_equal(disruptor_topology_stage_after_group(topology, s_sink, middle), 1);
    check_equal(disruptor_topology_commit(topology), 1);

    disruptor_cursor_t w = {0};
    check_equal(disruptor_publisher_try_claim(d, &w), 1);
    *(uint64_t *)disruptor_acquire_entry(d, &w) = 456;
    check_equal(disruptor_publisher_publish(d, &w), 1);

    disruptor_cursor_t sink_cursor = {.sequence = next_sink};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &sink, &sink_cursor), 0);

    disruptor_cursor_t parse_cursor = {.sequence = next_parse};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &parse, &parse_cursor), 1);
    disruptor_consumer_release_entry(d, &parse, &parse_cursor);

    disruptor_cursor_t validate_cursor = {.sequence = next_validate};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &validate, &validate_cursor), 1);
    disruptor_consumer_release_entry(d, &validate, &validate_cursor);
    sink_cursor.sequence = next_sink;
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &sink, &sink_cursor), 0);

    disruptor_cursor_t enrich_cursor = {.sequence = next_enrich};
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &enrich, &enrich_cursor), 1);
    disruptor_consumer_release_entry(d, &enrich, &enrich_cursor);

    sink_cursor.sequence = next_sink;
    check_equal(disruptor_consumer_wait_for_nonblocking_for(d, &sink, &sink_cursor), 1);
    check_equal(*(const uint64_t *)disruptor_show_entry(d, &sink_cursor), 456);

    disruptor_topology_destroy(topology);
    disruptor_destroy(d);
  }

  it("should reject topology cycles") {
    disruptor_config_t cfg = {
        .entry_size = sizeof(uint64_t), .capacity = 16, .consumer_capacity = 2};
    disruptor_t *d = disruptor_create(&cfg);
    check_not_null(d);

    disruptor_consumer_t a, b;
    disruptor_consumer_register(d, &a);
    disruptor_consumer_register(d, &b);

    disruptor_topology_t *topology = disruptor_topology_create(d);
    check_not_null(topology);
    disruptor_stage_t s_a = disruptor_topology_stage(topology, "a", &a);
    disruptor_stage_t s_b = disruptor_topology_stage(topology, "b", &b);
    check_equal(disruptor_topology_after(topology, s_a, s_b), 1);
    check_equal(disruptor_topology_after(topology, s_b, s_a), 1);
    check_equal(disruptor_topology_commit(topology), 0);

    disruptor_topology_destroy(topology);
    disruptor_destroy(d);
  }
}
