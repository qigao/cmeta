#include <cflow/actor.h>
#include <clock.h>
#include <thread.h>
#include <fmt.h>
#include <tlog.h>
#include "tinytest.h"

#include <stdlib.h>
#include <string.h>

/* Benchmark-local CPU dispatchers, not an exported scheduler. The elected
 * leader is the sole mailbox consumer; dequeue ownership transfers only under
 * the monitor. LF promotes before processing independent jobs. Token promotes
 * after processing, preserving FIFO settlement and one mutable-state owner.
 * Payloads are trivial copies. Producers retain their precomputed source until
 * joined. Capacity is fixed; FULL retries yield and have a sample deadline.
 * Destruction joins producers and workers before freeing callback storage.
 */
enum {
    MAX_THREADS = 4, MAX_BATCH = 32, MAX_PAYLOAD_BYTES = 65536,
    CAPACITY = 128, MAX_MESSAGES = 2048, FULL_SAMPLES = 16,
    QUICK_MESSAGES = 1024, QUICK_SAMPLES = 8
};
enum dispatch_mode { ACTOR_NONE, ACTOR_VALUE, LEADER_FOLLOWERS, SERIAL_TOKEN };
typedef struct completion { uint64_t id, digest; } completion;

static const cmeta_type_traits trivial_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
#define BENCH_PAYLOAD_TYPE(bytes) \
    typedef struct payload##bytes { uint64_t words[(bytes) / sizeof(uint64_t)]; } payload##bytes; \
    static const cmeta_type_desc type##bytes = { \
        "ace_bench_payload" #bytes, sizeof(payload##bytes), CMETA_ALIGNOF(payload##bytes), \
        CMETA_T_OBJECT, NULL, &trivial_traits, NULL}
BENCH_PAYLOAD_TYPE(16);
BENCH_PAYLOAD_TYPE(64);
BENCH_PAYLOAD_TYPE(256);
BENCH_PAYLOAD_TYPE(1024);
BENCH_PAYLOAD_TYPE(4096);
BENCH_PAYLOAD_TYPE(16384);
BENCH_PAYLOAD_TYPE(65536);
#undef BENCH_PAYLOAD_TYPE
static const cmeta_type_desc completion_type = {
    "ace_bench_completion", sizeof(completion), CMETA_ALIGNOF(completion),
    CMETA_T_OBJECT, NULL, &trivial_traits, NULL};

typedef struct bench_context bench_context;
typedef struct producer {
    bench_context *context;
    cmeta_thread_t thread;
    cflow_actor_ref ref;
    size_t id;
    uint64_t full_retries;
} producer;
typedef struct worker {
    bench_context *context;
    cmeta_thread_t thread;
    cmeta_cond_t wake;
    size_t id;
    bool eligible, waiting;
    unsigned char *payloads;
    completion results[MAX_BATCH];
} worker;
struct bench_context {
    cmeta_mutex_t lock;
    cmeta_cond_t changed;
    producer producers[MAX_THREADS];
    worker workers[MAX_THREADS];
    cflow_mailbox mailbox;
    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler scheduler;
    cflow_actor actor;
    enum dispatch_mode mode;
    const cmeta_type_desc *payload_type;
    size_t producer_count, worker_count, batch;
    size_t ready, producers_done, epoch, completed, queued, active, peak_active;
    size_t leader, last_leader, handoffs;
    size_t wake_signals, worker_waits, empty_wakes;
    size_t in_flight_items, peak_in_flight_items, max_claimed_batch;
    size_t accepted, dequeued, next_settlement;
    size_t observations[MAX_MESSAGES];
    uint64_t expected[MAX_MESSAGES];
    unsigned char *payloads;
    uint64_t deadline;
    bool closing, failed, hold_first, held, release, coalesce_wakes, dispatch_paused;
};

static bench_context *current;
/* Immutable while a case's threads are alive; full preserves the RC5 dataset. */
static bool full_profile;
static size_t message_count = QUICK_MESSAGES;
static size_t sample_count = QUICK_SAMPLES;
static tstr title;
static tlog_t *metrics_logger;
static cmeta_log_sink_t *metrics_sink;

/* O(payload bytes), O(1) scratch; exactly the same checksum in all modes. */
static completion process_payload(const void *data, size_t bytes) {
    completion result = {0u, UINT64_C(0x9e3779b97f4a7c15)};
    for (size_t offset = 0u; offset < bytes; offset += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, (const unsigned char *)data + offset, sizeof(word));
        if (offset == 0u) result.id = word;
        result.digest = (result.digest << 7) | (result.digest >> 57);
        result.digest ^= word;
    }
    return result;
}

static void signal_worker_locked(bench_context *context, size_t index) {
    ++context->wake_signals;
    cmeta_cond_signal(&context->workers[index].wake);
}

/* With the monitor held, waiting is published before atomic unlock-and-wait.
 * Admission cannot miss the sleep transition; running leaders inspect queued
 * again before sleeping. Eager mode retains the original comparison policy. */
static void notify_leader_locked(bench_context *context) {
    if (context->leader == SIZE_MAX) return;
    if (!context->coalesce_wakes ||
        (context->queued != 0u && context->workers[context->leader].waiting))
        signal_worker_locked(context, context->leader);
}

static void wake_all_locked(bench_context *context) {
    cmeta_cond_broadcast(&context->changed);
    for (size_t index = 0u; index < context->worker_count; ++index)
        if (context->workers[index].wake != NULL)
            signal_worker_locked(context, index);
}

static void fail_locked(bench_context *context) {
    context->failed = true;
    wake_all_locked(context);
}

static bool wait_locked(bench_context *context) {
    if (cmeta_monotonic_ms() >= context->deadline) {
        fail_locked(context);
        return false;
    }
    (void)cmeta_cond_timedwait(&context->changed, &context->lock, UINT64_C(10000000));
    return !context->failed;
}

/* O(worker_count), bounded by four. Election is explicit, before LF handling
 * and after token handling; workers never race for an unassigned leader slot. */
static void elect_locked(bench_context *context) {
    if (context->leader != SIZE_MAX) return;
    for (size_t step = 1u; step <= context->worker_count; ++step) {
        const size_t index = (context->last_leader + step) % context->worker_count;
        if (!context->workers[index].eligible) continue;
        context->leader = context->last_leader = index;
        ++context->handoffs;
        notify_leader_locked(context);
        return;
    }
}

static void settle_locked(bench_context *context, completion result) {
    if (result.id >= message_count || result.digest != context->expected[result.id] ||
        context->observations[result.id] >= context->epoch) {
        fail_locked(context);
        return;
    }
    ++context->observations[result.id];
    ++context->completed;
    if (context->completed % message_count == 0u)
        cmeta_cond_broadcast(&context->changed);
}

static bool actor_action(void *user, const void *state, const void *event,
                         void *target, void *observation, const char **error) {
    bench_context *context = (bench_context *)user;
    const completion result = process_payload(event, context->payload_type->size);
    (void)error;
    *(int *)target = *(const int *)state + 1;
    if (context->mode == ACTOR_VALUE) {
        *(completion *)observation = result;
    } else {
        cmeta_mutex_lock(&context->lock);
        settle_locked(context, result);
        cmeta_mutex_unlock(&context->lock);
    }
    return true;
}

static bool actor_value(void *user, const cmeta_type_desc *type, const void *value) {
    bench_context *context = (bench_context *)user;
    cmeta_mutex_lock(&context->lock);
    if (type != &completion_type) fail_locked(context);
    else settle_locked(context, *(const completion *)value);
    cmeta_mutex_unlock(&context->lock);
    return true;
}

static void actor_error(void *user, const char *message) {
    bench_context *context = (bench_context *)user;
    (void)message;
    cmeta_mutex_lock(&context->lock);
    fail_locked(context);
    cmeta_mutex_unlock(&context->lock);
}

static void run_worker(void *user) {
    worker *self = (worker *)user;
    bench_context *context = self->context;
    size_t held = 0u;
    cmeta_mutex_lock(&context->lock);
    self->eligible = true;
    ++context->ready;
    elect_locked(context);
    cmeta_cond_broadcast(&context->changed);
    while (!context->closing && !context->failed) {
        size_t count = 0u;
        const size_t first_ticket = context->dequeued;
        if (context->leader != self->id || context->queued == 0u || context->dispatch_paused) {
            self->waiting = true;
            ++context->worker_waits;
            cmeta_cond_wait(&self->wake, &context->lock);
            self->waiting = false;
            if (context->queued == 0u) ++context->empty_wakes;
            continue;
        }
        /* O(batch * payload bytes) copy, O(MAX_BATCH) bounded per-worker
         * scratch reserved during setup. Only this elected leader dequeues.
         * LF hands off BEFORE processing the claimed independent batch. */
        const size_t limit = context->mode == LEADER_FOLLOWERS ? context->batch : 1u;
        while (count < limit && context->queued != 0u) {
            cflow_event_id id;
            const cmeta_type_desc *type;
            if (cflow_mailbox_try_receive(&context->mailbox, &id, &type,
                                          self->payloads + count * context->payload_type->size,
                                          context->payload_type->size) !=
                CFLOW_MAILBOX_OK || id != 1u || type != context->payload_type) {
                fail_locked(context);
                break;
            }
            ++count;
            ++context->dequeued;
            --context->queued;
        }
        if (count == 0u) break;
        ++context->active;
        context->in_flight_items += count;
        if (context->in_flight_items > context->peak_in_flight_items)
            context->peak_in_flight_items = context->in_flight_items;
        if (count > context->max_claimed_batch) context->max_claimed_batch = count;
        if (context->active > context->peak_active) context->peak_active = context->active;
        self->eligible = false;
        if (context->mode == LEADER_FOLLOWERS) {
            context->leader = SIZE_MAX;
            elect_locked(context);
        }
        cmeta_mutex_unlock(&context->lock);
        if (context->hold_first && first_ticket == 0u) {
            cmeta_mutex_lock(&context->lock);
            context->held = true;
            cmeta_cond_broadcast(&context->changed);
            while (!context->release && !context->closing && !context->failed)
                (void)wait_locked(context);
            cmeta_mutex_unlock(&context->lock);
        }
        for (size_t index = 0u; index < count; ++index)
            self->results[index] = process_payload(self->payloads + index * context->payload_type->size,
                                                    context->payload_type->size);
        cmeta_mutex_lock(&context->lock);
        if (context->mode == SERIAL_TOKEN && first_ticket != context->next_settlement++)
            fail_locked(context);
        for (size_t index = 0u; index < count; ++index)
            settle_locked(context, self->results[index]);
        context->in_flight_items -= count;
        --context->active;
        self->eligible = true;
        if (context->mode == SERIAL_TOKEN) {
            ++held;
            if (held >= context->batch || context->queued == 0u) {
                held = 0u;
                context->leader = SIZE_MAX;
                elect_locked(context);
            }
        } else elect_locked(context);
    }
    cmeta_mutex_unlock(&context->lock);
}

/* Only one logical dequeue owner is active even when LF handlers overlap.
 * The prototype's monitor additionally serializes enqueue with election. */
static int send_payload(producer *self, const cflow_event_view *event) {
    bench_context *context = self->context;
    if (context->mode == ACTOR_NONE || context->mode == ACTOR_VALUE) {
        const cflow_actor_send_status result = cflow_actor_ref_try_send(&self->ref, event);
        if (result == CFLOW_ACTOR_SEND_ACCEPTED) return 1;
        if (result == CFLOW_ACTOR_SEND_FULL) return 0;
        return -1;
    }
    cmeta_mutex_lock(&context->lock);
    int result = -1;
    if (!context->failed && !context->closing) {
        const cflow_mailbox_status status = cflow_mailbox_try_send(&context->mailbox, event);
        if (status == CFLOW_MAILBOX_OK) {
            const bool was_empty = context->queued == 0u;
            ++context->queued;
            ++context->accepted;
            if (!context->coalesce_wakes || was_empty) notify_leader_locked(context);
            result = 1;
        } else if (status == CFLOW_MAILBOX_FULL) result = 0;
    }
    cmeta_mutex_unlock(&context->lock);
    return result;
}

static void run_producer(void *user) {
    producer *self = (producer *)user;
    bench_context *context = self->context;
    size_t last_epoch = 0u;
    cmeta_mutex_lock(&context->lock);
    ++context->ready;
    cmeta_cond_broadcast(&context->changed);
    while (!context->closing && !context->failed) {
        while (context->epoch == last_epoch && !context->closing && !context->failed)
            cmeta_cond_wait(&context->changed, &context->lock);
        if (context->closing || context->failed) break;
        last_epoch = context->epoch;
        const uint64_t deadline = context->deadline;
        cmeta_mutex_unlock(&context->lock);
        bool valid = true;
        for (size_t id = self->id; id < message_count && valid; id += context->producer_count) {
            const cflow_event_view event = {
                1u, context->payload_type, context->payloads + id * context->payload_type->size};
            for (;;) {
                const int result = send_payload(self, &event);
                if (result == 1) break;
                if (result < 0 || cmeta_monotonic_ms() >= deadline) {
                    valid = false;
                    break;
                }
                ++self->full_retries;
                cmeta_thread_yield();
            }
        }
        cmeta_mutex_lock(&context->lock);
        if (!valid) fail_locked(context);
        ++context->producers_done;
        cmeta_cond_broadcast(&context->changed);
    }
    cmeta_mutex_unlock(&context->lock);
}

static bool init_actor(bench_context *context) {
    const bool emit = context->mode == ACTOR_VALUE;
    const cflow_machine_state state = {1u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE};
    const cflow_event_type event = {1u, context->payload_type};
    const cflow_machine_action action = {
        1u, &cmeta_type_int, 1u, context->payload_type, &cmeta_type_int,
        CMETA_EFFECT_STATEFUL, CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
        emit ? CFLOW_MACHINE_ACTION_VALUE : CFLOW_MACHINE_ACTION_NONE,
        emit ? &completion_type : NULL, 0u};
    const cflow_machine_transition transition = {1u, 1u, 0u, 1u, 1u, 0u};
    const cflow_machine_definition definition = {
        &state, 1u, 1u, &event, 1u, NULL, 0u, &action, 1u, &transition, 1u};
    const cflow_machine_action_binding binding = {1u, actor_action, context};
    const int initial = 0;
    cflow_actor_config config = {0};
    if (cflow_machine_build(&context->machine, &definition) != CFLOW_MACHINE_OK ||
        !cflow_executor_serial_init(&context->executor) ||
        !cflow_scheduler_worker_init(&context->scheduler, 1u)) return false;
    config.machine = (cflow_machine_instance_config){
        &context->machine, &initial, emit ? &completion_type : &cmeta_type_int, NULL, 0u,
        &binding, 1u, CAPACITY, &context->executor};
    config.scheduler = &context->scheduler;
    config.callbacks = (cflow_subscriber_callbacks){actor_value, actor_error, NULL, context};
    if (cflow_actor_init(&context->actor, &config).status != CFLOW_ACTOR_OK ||
        cflow_actor_start(&context->actor) != CFLOW_ACTOR_OK) return false;
    return cflow_scheduler_wait_idle(&context->scheduler) &&
           cflow_executor_wait_idle(&context->executor);
}

static bool init_case(enum dispatch_mode mode, size_t producers, size_t workers,
                       size_t batch, bool coalesce_wakes, const cmeta_type_desc *type) {
    if (batch == 0u || batch > MAX_BATCH || producers > MAX_THREADS ||
        workers > MAX_THREADS || type == NULL || type->size < sizeof(uint64_t) ||
        type->size > MAX_PAYLOAD_BYTES || type->size % sizeof(uint64_t) != 0u) return false;
    current = (bench_context *)calloc(1u, sizeof(*current));
    if (current == NULL) return false;
    bench_context *context = current;
    context->mode = mode;
    context->producer_count = producers;
    context->worker_count = workers;
    context->batch = batch;
    context->coalesce_wakes = coalesce_wakes;
    context->payload_type = type;
    context->leader = SIZE_MAX;
    context->last_leader = workers == 0u ? 0u : workers - 1u;
    context->deadline = cmeta_monotonic_ms() + 10000u;
    cmeta_mutex_init(&context->lock);
    cmeta_cond_init(&context->changed);
    if (context->lock == NULL || context->changed == NULL) return false;
    /* Packed, size-specific pools reserve at most 64 MiB in quick mode,
     * 128 MiB in full mode. Profile constants bound the multiplication. */
    context->payloads = (unsigned char *)calloc(message_count, type->size);
    if (context->payloads == NULL) return false;
    for (size_t id = 0u; id < message_count; ++id) {
        unsigned char *payload = context->payloads + id * type->size;
        for (size_t index = 0u; index < type->size / sizeof(uint64_t); ++index) {
            const uint64_t word = index == 0u ? id :
                UINT64_C(0xd6e8feb86659fd93) * (id + index);
            memcpy(payload + index * sizeof(word), &word, sizeof(word));
        }
        context->expected[id] = process_payload(payload, type->size).digest;
    }
    if (mode == ACTOR_NONE || mode == ACTOR_VALUE) {
        if (!init_actor(context)) return false;
    } else {
        const cflow_event_type event = {1u, type};
        if (cflow_mailbox_init(&context->mailbox, &event, 1u, CAPACITY) != CFLOW_MAILBOX_OK)
            return false;
        for (size_t index = 0u; index < workers; ++index) {
            worker *self = &context->workers[index];
            self->context = context;
            self->id = index;
            const size_t slots = mode == LEADER_FOLLOWERS ? batch : 1u;
            self->payloads = (unsigned char *)calloc(slots, type->size);
            if (self->payloads == NULL) return false;
            cmeta_cond_init(&self->wake);
            if (self->wake == NULL ||
                cmeta_thread_create(&self->thread, run_worker, self) != 0) return false;
        }
    }
    for (size_t index = 0u; index < producers; ++index) {
        producer *self = &context->producers[index];
        self->context = context;
        self->id = index;
        if (context->actor.impl != NULL && !cflow_actor_ref_acquire(&context->actor, &self->ref))
            return false;
        if (cmeta_thread_create(&self->thread, run_producer, self) != 0) return false;
    }
    cmeta_mutex_lock(&context->lock);
    while (context->ready < producers + workers && !context->failed) (void)wait_locked(context);
    const bool valid = !context->failed;
    cmeta_mutex_unlock(&context->lock);
    return valid;
}

static void run_sample(void) {
    bench_context *context = current;
    cmeta_mutex_lock(&context->lock);
    context->deadline = cmeta_monotonic_ms() + 10000u;
    context->producers_done = 0u;
    ++context->epoch;
    cmeta_cond_broadcast(&context->changed);
    while (!context->failed &&
           (context->producers_done < context->producer_count ||
            context->completed < context->epoch * message_count)) (void)wait_locked(context);
    cmeta_mutex_unlock(&context->lock);
    /* Do not report a partial batch as a full throughput sample. */
    check_false(context->failed);
}

static void destroy_case(void) {
    bench_context *context = current;
    if (context == NULL) return;
    if (context->lock != NULL) {
        cmeta_mutex_lock(&context->lock);
        context->closing = true;
        if (context->changed != NULL) wake_all_locked(context);
        cmeta_mutex_unlock(&context->lock);
    }
    for (size_t index = 0u; index < context->producer_count; ++index) {
        producer *self = &context->producers[index];
        if (self->thread != NULL && cmeta_thread_join(&self->thread) != 0) abort();
        cflow_actor_ref_release(&self->ref);
    }
    for (size_t index = 0u; index < context->worker_count; ++index) {
        worker *self = &context->workers[index];
        if (self->thread != NULL && cmeta_thread_join(&self->thread) != 0) abort();
        cmeta_cond_destroy(&self->wake);
        free(self->payloads);
    }
    if (context->actor.impl != NULL) {
        (void)cflow_actor_request_stop(&context->actor);
        (void)cflow_actor_wait(&context->actor);
        cflow_actor_destroy(&context->actor);
    }
    cflow_scheduler_destroy(&context->scheduler);
    cflow_executor_destroy(&context->executor);
    cflow_machine_destroy(&context->machine);
    cflow_mailbox_destroy(&context->mailbox);
    cmeta_cond_destroy(&context->changed);
    cmeta_mutex_destroy(&context->lock);
    free(context->payloads);
    free(context);
    current = NULL;
    tstr_freep(&title);
}

static void verify_case(size_t samples) {
    bench_context *context = current;
    cflow_mailbox_stats mailbox = {0};
    const size_t expected = samples * message_count;
    /* NONE completion is counted within the action, before Machine accounting
     * commits. Wait for real executor/scheduler quiescence before snapshot. */
    if (context->actor.impl != NULL) {
        cflow_actor_stats stats = {0};
        check_true(cflow_executor_wait_idle(&context->executor));
        check_true(cflow_scheduler_wait_idle(&context->scheduler));
        check_true(cflow_actor_get_stats(&context->actor, &stats));
        check_equal(stats.machine.completed, (uint64_t)expected);
        check_equal(stats.machine.failed, (uint64_t)0u);
        check_equal(stats.machine.accepted, (uint64_t)expected);
        check_equal(stats.machine.pending, (size_t)0u);
        check_equal(stats.machine.in_flight, (size_t)0u);
        check_equal(stats.machine.emitted_values,
                    context->mode == ACTOR_VALUE ? (uint64_t)expected : (uint64_t)0u);
    } else {
        check_equal(context->accepted, expected);
        check_equal(context->dequeued, expected);
        check_equal(context->active, (size_t)0u);
        check_equal(context->in_flight_items, (size_t)0u);
        check_equal(context->queued, (size_t)0u);
        check_true(cflow_mailbox_get_stats(&context->mailbox, &mailbox));
        check_less_equal(context->peak_active, context->worker_count);
        check_less_equal(context->max_claimed_batch, context->batch);
        check_less_equal(context->peak_in_flight_items, context->worker_count * context->batch);
        if (context->mode == SERIAL_TOKEN) {
            check_equal(context->peak_active, (size_t)1u);
            check_equal(context->next_settlement, expected);
        }
        check_greater(context->handoffs, (size_t)0u);
        check_equal(mailbox.accepted, (uint64_t)expected);
        check_equal(mailbox.received, (uint64_t)expected);
        check_equal(mailbox.pending, (size_t)0u);
        check_less_equal(mailbox.peak_pending, (size_t)CAPACITY);
        uint64_t full_retries = 0u;
        for (size_t index = 0u; index < context->producer_count; ++index)
            full_retries += context->producers[index].full_retries;
        check_equal(mailbox.rejected_full, full_retries);
    }
    check_equal(context->completed, expected);
    for (size_t id = 0u; id < message_count; ++id)
        check_equal(context->observations[id], samples);
}

/* Summaries are emitted after measurement and correctness checks. Creating,
 * flushing and destroying the logger here keeps its worker out of ALL timed
 * samples and the next case. Counters include setup and the warmup sample. */
static void report_case(void) {
    bench_context *context = current;
    if (context->mode != LEADER_FOLLOWERS) return;
    cflow_mailbox_stats stats = {0};
    size_t wakes, waits, empty, handoffs, peak_in_flight;
    cmeta_mutex_lock(&context->lock);
    wakes = context->wake_signals;
    waits = context->worker_waits;
    empty = context->empty_wakes;
    handoffs = context->handoffs;
    peak_in_flight = context->peak_in_flight_items;
    cmeta_mutex_unlock(&context->lock);
    check_true(cflow_mailbox_get_stats(&context->mailbox, &stats));
    metrics_logger = tlog_create(NULL);
    check_not_null(metrics_logger);
    const cmeta_console_sink_opts_t options = {
        .output = stdout, .use_colors = 0, .pattern = "{message}"};
    metrics_sink = cmeta_sink_console_create(&options);
    check_not_null(metrics_sink);
    check_equal(tlog_add_sink(metrics_logger, metrics_sink), 0);
    metrics_sink = NULL; /* Ownership transferred only after success. */
    SALTS_LOG_INFOF(metrics_logger, "ace-benchmark",
        "ACE_METRICS {} messages={} wake_signals={} worker_waits={} empty_wakes={} "
        "elections={} peak_pending={} peak_in_flight={} full_retries={}",
        title, (size_t)stats.received, wakes, waits, empty, handoffs,
        stats.peak_pending, peak_in_flight, (size_t)stats.rejected_full);
    tlog_flush(metrics_logger);
    check_equal(tlog_get_dropped(metrics_logger), (uint64_t)0u);
    tlog_destroy(metrics_logger);
    metrics_logger = NULL;
}

suite("ACE CPU payload throughput, bounded admission to verified completion") {
    before_each() {
        const char *profile = getenv("CFLOW_ACE_BENCH_PROFILE");
        check_true(profile == NULL || strcmp(profile, "quick") == 0 ||
                                     strcmp(profile, "full") == 0);
        full_profile = profile != NULL && strcmp(profile, "full") == 0;
        message_count = full_profile ? MAX_MESSAGES : QUICK_MESSAGES;
        sample_count = full_profile ? FULL_SAMPLES : QUICK_SAMPLES;
    }
    after_each() {
        destroy_case();
        cmeta_sink_destroy(metrics_sink);
        metrics_sink = NULL;
        tlog_destroy(metrics_logger);
        metrics_logger = NULL;
    }
    it("promotes a follower before handling, but transfers a serial token after handling") {
        for (size_t mode = 0u; mode < 4u; ++mode) {
            const bool serial = mode == 3u;
            const size_t batch = mode == 2u ? MAX_BATCH : 1u;
            check_true(init_case(serial ? SERIAL_TOKEN : LEADER_FOLLOWERS,
                                  1u, 2u, batch, mode == 1u || mode == 2u, &type64));
            bench_context *context = current;
            cmeta_mutex_lock(&context->lock);
            context->hold_first = true;
            context->dispatch_paused = true;
            ++context->epoch;
            cmeta_cond_broadcast(&context->changed);
            while (context->queued < CAPACITY && !context->failed) (void)wait_locked(context);
            context->dispatch_paused = false;
            wake_all_locked(context);
            while (!context->held && !context->failed) (void)wait_locked(context);
            bool observed_contract;
            if (serial) {
                while (context->queued < CAPACITY && !context->failed) (void)wait_locked(context);
                observed_contract = context->completed == 0u && context->active == 1u;
            } else {
                while (context->completed == 0u && !context->failed) (void)wait_locked(context);
                observed_contract = context->completed > 0u && context->peak_active == 2u;
            }
            context->release = true;
            cmeta_cond_broadcast(&context->changed);
            while (!context->failed &&
                   (context->producers_done != 1u || context->completed != message_count))
                (void)wait_locked(context);
            cmeta_mutex_unlock(&context->lock);
            check_false(context->failed);
            check_true(observed_contract);
            verify_case(1u);
            check_equal(context->max_claimed_batch, batch);
            if (serial) {
                cflow_mailbox_stats stats = {0};
                check_true(cflow_mailbox_get_stats(&context->mailbox, &stats));
                check_equal(stats.peak_pending, (size_t)CAPACITY);
            }
            destroy_case();
        }
    }
    it("resumes a sleeping leader on sparse admission and drains partial batches") {
        check_true(init_case(LEADER_FOLLOWERS, 0u, MAX_THREADS,
                              MAX_BATCH, true, &type1024));
        bench_context *context = current;
        producer sender = {0};
        sender.context = context;
        cmeta_mutex_lock(&context->lock);
        ++context->epoch;
        cmeta_mutex_unlock(&context->lock);
        size_t submitted = 0u;
        while (submitted < message_count) {
            cmeta_mutex_lock(&context->lock);
            context->deadline = cmeta_monotonic_ms() + 10000u;
            for (;;) {
                size_t sleeping = 0u;
                for (size_t index = 0u; index < context->worker_count; ++index)
                    sleeping += context->workers[index].waiting ? 1u : 0u;
                if (sleeping == context->worker_count || !wait_locked(context)) break;
            }
            const bool idle = !context->failed && context->queued == 0u &&
                              context->in_flight_items == 0u;
            cmeta_mutex_unlock(&context->lock);
            check_true(idle);
            /* Singles force empty-to-nonempty wakeups; later waves contain at
             * most one batch, including a final 31-message partial batch. */
            size_t count = submitted < 65u ? 1u : MAX_BATCH;
            if (count > message_count - submitted) count = message_count - submitted;
            for (size_t index = 0u; index < count; ++index) {
                const cflow_event_view event = {
                    1u, &type1024, context->payloads + (submitted + index) * type1024.size};
                check_equal(send_payload(&sender, &event), 1);
            }
            submitted += count;
            cmeta_mutex_lock(&context->lock);
            while (context->completed < submitted && !context->failed)
                (void)wait_locked(context);
            const bool valid = !context->failed;
            cmeta_mutex_unlock(&context->lock);
            check_true(valid);
        }
        verify_case(1u);
    }
    bench("compares Active Object, Leader/Followers and serial token handoff") {
        static const struct {
            const char *name;
            enum dispatch_mode mode;
            size_t workers, batch;
            bool coalesce_wakes;
        } cases[] = {
            {"Actor NONE", ACTOR_NONE, 0u, 1u, false},
            {"Actor VALUE", ACTOR_VALUE, 0u, 1u, false},
            {"Leader/Followers eager", LEADER_FOLLOWERS, 1u, 1u, false},
            {"Leader/Followers coalesced", LEADER_FOLLOWERS, 1u, 1u, true},
            {"Leader/Followers coalesced", LEADER_FOLLOWERS, 1u, MAX_BATCH, true},
            {"Leader/Followers eager", LEADER_FOLLOWERS, 2u, 1u, false},
            {"Leader/Followers coalesced", LEADER_FOLLOWERS, 2u, 1u, true},
            {"Leader/Followers coalesced", LEADER_FOLLOWERS, 2u, MAX_BATCH, true},
            {"Leader/Followers eager", LEADER_FOLLOWERS, 4u, 1u, false},
            {"Leader/Followers coalesced", LEADER_FOLLOWERS, 4u, 1u, true},
            {"Leader/Followers coalesced", LEADER_FOLLOWERS, 4u, MAX_BATCH, true},
            {"Serial token", SERIAL_TOKEN, 4u, 1u, false},
            {"Serial token", SERIAL_TOKEN, 4u, MAX_BATCH, false}};
        const cmeta_type_desc *types[] = {
            &type16, &type64, &type256, &type1024, &type4096, &type16384, &type65536};
        for (size_t payload = 0u; payload < sizeof(types) / sizeof(types[0]); ++payload) {
            if (!full_profile && types[payload] != &type16 &&
                                 types[payload] != &type1024 &&
                                 types[payload] != &type65536) continue;
            for (size_t producers = 1u; producers <= MAX_THREADS; producers *= MAX_THREADS) {
                for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
                    check_true(init_case(cases[index].mode, producers, cases[index].workers,
                                          cases[index].batch, cases[index].coalesce_wakes,
                                          types[payload]));
                    run_sample(); /* Warm each topology once, outside measurement. */
                    title = tstr_format("{} p={} w={} batch={} bytes={}", cases[index].name,
                                        producers, cases[index].workers, cases[index].batch,
                                        types[payload]->size);
                    check_not_null(title);
                    benchmark_io(title, sample_count, message_count,
                                 message_count * types[payload]->size) {
                        run_sample();
                    }
                    verify_case(sample_count + 1u);
                    report_case();
                    destroy_case();
                }
            }
        }
    }
}
