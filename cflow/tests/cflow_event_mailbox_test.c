#include <cflow/event.h>
#include <thread.h>

#include "tinytest.h"

#include <stdatomic.h>

static bool owned_payload_copy(void *destination, const void *source) {
    if (destination == NULL || source == NULL) return false;
    *(int *)destination = *(const int *)source;
    return true;
}

static void owned_payload_destroy(void *value) {
    (void)value;
}

static const cmeta_type_traits owned_payload_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY,
    NULL,
    NULL,
    NULL,
    owned_payload_copy,
    NULL,
    owned_payload_destroy
};

static const cmeta_type_desc owned_payload_type = {
    "owned_payload",
    sizeof(int),
    CMETA_ALIGNOF(int),
    CMETA_T_OBJECT,
    NULL,
    &owned_payload_traits,
    NULL
};

typedef struct mailbox_wake_probe {
    size_t wakes;
} mailbox_wake_probe;

static void mailbox_count_wake(void *user) {
    mailbox_wake_probe *probe = (mailbox_wake_probe *)user;
    if (probe != NULL) ++probe->wakes;
}

enum {
    MAILBOX_PRODUCER_COUNT = 4,
    MAILBOX_EVENTS_PER_PRODUCER = 64,
    MAILBOX_CONCURRENT_EVENT_COUNT =
        MAILBOX_PRODUCER_COUNT * MAILBOX_EVENTS_PER_PRODUCER
};

typedef struct mailbox_producer_context {
    cflow_mailbox *mailbox;
    int producer_index;
    atomic_int *finished;
    atomic_int *failures;
} mailbox_producer_context;

static void mailbox_producer(void *user) {
    mailbox_producer_context *context = (mailbox_producer_context *)user;
    int index;

    if (context == NULL) return;
    for (index = 0; index < MAILBOX_EVENTS_PER_PRODUCER; ++index) {
        const int payload =
            context->producer_index * MAILBOX_EVENTS_PER_PRODUCER + index;
        const cflow_event_view event = {1u, &cmeta_type_int, &payload};

        for (;;) {
            const cflow_mailbox_status status =
                cflow_mailbox_try_send(context->mailbox, &event);
            if (status == CFLOW_MAILBOX_OK) break;
            if (status == CFLOW_MAILBOX_FULL) {
                cmeta_thread_yield();
                continue;
            }
            atomic_fetch_add(context->failures, 1);
            atomic_fetch_add(context->finished, 1);
            return;
        }
    }
    atomic_fetch_add(context->finished, 1);
}

suite("CFlow typed event mailbox") {
    it("initializes a finite heterogeneous schema") {
        const cflow_event_type schema[] = {
            {1u, &cmeta_type_int},
            {2u, &cmeta_type_double}
        };
        cflow_mailbox mailbox = {0};

        check_equal(cflow_mailbox_init(
                        &mailbox, schema,
                        sizeof(schema) / sizeof(schema[0]), 4u),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_payload_capacity(&mailbox), sizeof(double));

        cflow_mailbox_destroy(&mailbox);
        check_null(mailbox.impl);
    }

    it("rejects invalid schema and capacity without publishing state") {
        const cflow_event_type zero_id[] = {{0u, &cmeta_type_int}};
        const cflow_event_type duplicate_id[] = {
            {1u, &cmeta_type_int},
            {1u, &cmeta_type_double}
        };
        const cflow_event_type nontrivial[] = {{1u, &owned_payload_type}};
        cmeta_type_desc invalid_alignment_type = cmeta_type_int;
        cflow_event_type invalid_alignment[] = {{1u, &invalid_alignment_type}};
        const cflow_event_type valid[] = {{1u, &cmeta_type_int}};
        cflow_mailbox mailbox = {0};

        invalid_alignment_type.align = 3u;

        check_equal(cflow_mailbox_init(&mailbox, zero_id, 1u, 1u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);
        check_equal(cflow_mailbox_init(&mailbox, duplicate_id, 2u, 1u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);
        check_equal(cflow_mailbox_init(&mailbox, nontrivial, 1u, 1u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);
        check_equal(cflow_mailbox_init(&mailbox, zero_id, 1u, 0u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);
        check_equal(cflow_mailbox_init(&mailbox, invalid_alignment, 1u, 1u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);
        check_equal(cflow_mailbox_init(&mailbox, valid, 1u, SIZE_MAX),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);
        check_equal(cflow_mailbox_init(&mailbox, valid, SIZE_MAX, 1u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_null(mailbox.impl);

        check_equal(cflow_mailbox_init(&mailbox, valid, 1u, 1u),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_init(&mailbox, valid, 1u, 1u),
                    CFLOW_MAILBOX_INVALID_ARGUMENT);
        check_not_null(mailbox.impl);
        cflow_mailbox_destroy(&mailbox);
    }

    it("admits and observes heterogeneous events in FIFO order") {
        const cflow_event_type schema[] = {
            {1u, &cmeta_type_int},
            {2u, &cmeta_type_double}
        };
        const int integer_payload = 17;
        const double double_payload = 3.5;
        const cflow_event_view integer_event = {
            1u, &cmeta_type_int, &integer_payload
        };
        const cflow_event_view double_event = {
            2u, &cmeta_type_double, &double_payload
        };
        const cflow_event_view mismatched_event = {
            1u, &cmeta_type_double, &double_payload
        };
        cflow_mailbox mailbox = {0};
        cflow_mailbox_stats stats = {0};
        cflow_event_id observed_id = 99u;
        const cmeta_type_desc *observed_type = &cmeta_type_double;
        int integer_output = -1;
        double double_output = -1.0;

        check_equal(cflow_mailbox_init(&mailbox, schema, 2u, 2u),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &mismatched_event),
                    CFLOW_MAILBOX_TYPE_MISMATCH);
        check_equal(cflow_mailbox_try_send(&mailbox, &integer_event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &double_event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &integer_event),
                    CFLOW_MAILBOX_FULL);

        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &observed_id, &observed_type,
                        &integer_output, sizeof(integer_output) - 1u),
                    CFLOW_MAILBOX_BUFFER_TOO_SMALL);
        check_equal(observed_id, (cflow_event_id)0u);
        check_null(observed_type);
        check_equal(integer_output, -1);

        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &observed_id, &observed_type,
                        &integer_output, sizeof(integer_output)),
                    CFLOW_MAILBOX_OK);
        check_equal(observed_id, (cflow_event_id)1u);
        check_true(observed_type == &cmeta_type_int);
        check_equal(integer_output, integer_payload);

        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &observed_id, &observed_type,
                        &double_output, sizeof(double_output)),
                    CFLOW_MAILBOX_OK);
        check_equal(observed_id, (cflow_event_id)2u);
        check_true(observed_type == &cmeta_type_double);
        check_true(double_output == double_payload);

        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &observed_id, &observed_type,
                        &integer_output, sizeof(integer_output)),
                    CFLOW_MAILBOX_EMPTY);
        check_equal(observed_id, (cflow_event_id)0u);
        check_null(observed_type);

        check_true(cflow_mailbox_get_stats(&mailbox, &stats));
        check_equal(stats.schema_count, (size_t)2u);
        check_equal(stats.capacity, (size_t)2u);
        check_equal(stats.pending, (size_t)0u);
        check_equal(stats.peak_pending, (size_t)2u);
        check_equal(stats.accepted, (uint64_t)2u);
        check_equal(stats.received, (uint64_t)2u);
        check_equal(stats.rejected_full, (uint64_t)1u);

        cflow_mailbox_destroy(&mailbox);
    }

    it("indexes unsorted schema IDs and accepts an equivalent descriptor") {
        const cflow_event_type schema[] = {
            {UINT64_MAX, &cmeta_type_double}, {7u, &cmeta_type_int},
            {1u, &cmeta_type_bool}};
        const cmeta_type_desc equivalent = cmeta_type_int;
        const int payload = 19;
        const cflow_event_view event = {7u, &equivalent, &payload};
        const cflow_event_view unknown = {8u, &equivalent, &payload};
        cflow_mailbox mailbox = {0};
        cflow_event_id id;
        const cmeta_type_desc *type;
        int observed = 0;
        check_equal(cflow_mailbox_init(&mailbox, schema, 3u, 3u), CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &unknown), CFLOW_MAILBOX_INVALID_ARGUMENT);
        /* A non-power-of-two queue repeatedly crosses its wrap boundary. */
        for (size_t index = 0u; index < 20u; ++index) {
            check_equal(cflow_mailbox_try_send(&mailbox, &event), CFLOW_MAILBOX_OK);
            check_equal(cflow_mailbox_try_receive(
                &mailbox, &id, &type, &observed, sizeof(observed)), CFLOW_MAILBOX_OK);
            check_equal(id, (cflow_event_id)7u);
            check_true(type == &cmeta_type_int);
            check_equal(observed, payload);
        }
        const double double_payload = 2.5;
        const bool bool_payload = true;
        const cflow_event_view mixed[] = {
            event, {UINT64_MAX, &cmeta_type_double, &double_payload},
            {1u, &cmeta_type_bool, &bool_payload}};
        union { int integer; double decimal; bool boolean; } output;
        for (size_t round = 0u; round < 4u; ++round) {
            for (size_t index = 0u; index < 3u; ++index)
                check_equal(cflow_mailbox_try_send(&mailbox, &mixed[index]), CFLOW_MAILBOX_OK);
            check_equal(cflow_mailbox_try_send(&mailbox, &event), CFLOW_MAILBOX_FULL);
            for (size_t index = 0u; index < 3u; ++index) {
                check_equal(cflow_mailbox_try_receive(
                    &mailbox, &id, &type, &output, sizeof(output)), CFLOW_MAILBOX_OK);
                check_equal(id, mixed[index].id);
                check_true(type == schema[index == 0u ? 1u : index == 1u ? 0u : 2u].payload_type);
                if (index == 0u) check_equal(output.integer, payload);
                else if (index == 1u) check_true(output.decimal == double_payload);
                else check_true(output.boolean);
            }
        }
        cflow_mailbox_destroy(&mailbox);
    }

    it("drains after close and discards on cancellation") {
        const cflow_event_type schema[] = {{1u, &cmeta_type_int}};
        const int first = 11;
        const int second = 12;
        const cflow_event_view first_event = {1u, &cmeta_type_int, &first};
        const cflow_event_view second_event = {1u, &cmeta_type_int, &second};
        cflow_mailbox mailbox = {0};
        cflow_mailbox_stats stats = {0};
        cflow_event_id id = 0u;
        const cmeta_type_desc *type = NULL;
        int output = 0;

        check_equal(cflow_mailbox_init(&mailbox, schema, 1u, 2u),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &first_event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &second_event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_close(&mailbox), CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_close(&mailbox), CFLOW_MAILBOX_CLOSED);
        check_equal(cflow_mailbox_try_send(&mailbox, &first_event),
                    CFLOW_MAILBOX_CLOSED);
        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &id, &type, &output, sizeof(output)),
                    CFLOW_MAILBOX_OK);
        check_equal(output, first);
        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &id, &type, &output, sizeof(output)),
                    CFLOW_MAILBOX_OK);
        check_equal(output, second);
        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &id, &type, &output, sizeof(output)),
                    CFLOW_MAILBOX_CLOSED);
        check_true(cflow_mailbox_get_stats(&mailbox, &stats));
        check_equal(stats.rejected_closed, (uint64_t)1u);
        check_equal(stats.cancelled, (uint64_t)0u);
        cflow_mailbox_destroy(&mailbox);

        check_equal(cflow_mailbox_init(&mailbox, schema, 1u, 2u),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &first_event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &second_event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_cancel(&mailbox), CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_cancel(&mailbox), CFLOW_MAILBOX_CANCELLED);
        check_equal(cflow_mailbox_close(&mailbox), CFLOW_MAILBOX_CANCELLED);
        check_equal(cflow_mailbox_try_send(&mailbox, &first_event),
                    CFLOW_MAILBOX_CANCELLED);
        check_equal(cflow_mailbox_try_receive(
                        &mailbox, &id, &type, &output, sizeof(output)),
                    CFLOW_MAILBOX_CANCELLED);
        check_true(cflow_mailbox_get_stats(&mailbox, &stats));
        check_equal(stats.pending, (size_t)0u);
        check_equal(stats.cancelled, (uint64_t)2u);
        check_equal(stats.rejected_cancelled, (uint64_t)1u);
        cflow_mailbox_destroy(&mailbox);
    }

    it("coalesces wakes until the single consumer rearms") {
        const cflow_event_type schema[] = {{1u, &cmeta_type_int}};
        const int payload = 7;
        const cflow_event_view event = {1u, &cmeta_type_int, &payload};
        cflow_mailbox mailbox = {0};
        cflow_waitable waitable = {0};
        mailbox_wake_probe probe = {0};
        const cflow_waker waker = {mailbox_count_wake, &probe};

        check_equal(cflow_mailbox_init(&mailbox, schema, 1u, 3u),
                    CFLOW_MAILBOX_OK);
        waitable = cflow_mailbox_as_waitable(&mailbox);
        check_true(cflow_waitable_valid(&waitable));
        check_true(cflow_waitable_arm(&waitable, waker));
        check_false(cflow_waitable_arm(&waitable, waker));
        check_equal(cflow_mailbox_try_send(&mailbox, &event),
                    CFLOW_MAILBOX_OK);
        check_equal(cflow_mailbox_try_send(&mailbox, &event),
                    CFLOW_MAILBOX_OK);
        check_equal(probe.wakes, (size_t)1u);

        check_true(cflow_waitable_arm(&waitable, waker));
        check_equal(probe.wakes, (size_t)2u);
        cflow_waitable_cancel(&waitable);
        check_equal(cflow_mailbox_close(&mailbox), CFLOW_MAILBOX_OK);
        check_equal(probe.wakes, (size_t)2u);

        check_true(cflow_waitable_arm(&waitable, waker));
        check_equal(probe.wakes, (size_t)3u);
        cflow_mailbox_destroy(&mailbox);
    }

    it("admits concurrent producers and observes every value once") {
        const cflow_event_type schema[] = {{1u, &cmeta_type_int}};
        cflow_mailbox mailbox = {0};
        cmeta_thread_t producers[MAILBOX_PRODUCER_COUNT] = {0};
        bool producer_started[MAILBOX_PRODUCER_COUNT] = {false};
        mailbox_producer_context contexts[MAILBOX_PRODUCER_COUNT];
        bool seen[MAILBOX_CONCURRENT_EVENT_COUNT] = {false};
        atomic_int finished;
        atomic_int failures;
        bool producers_quiescent = false;
        size_t received = 0u;
        int producer_index;

        atomic_init(&finished, 0);
        atomic_init(&failures, 0);
        check_equal(cflow_mailbox_init(&mailbox, schema, 1u, 8u),
                    CFLOW_MAILBOX_OK);
        for (producer_index = 0;
             producer_index < MAILBOX_PRODUCER_COUNT;
             ++producer_index) {
            contexts[producer_index].mailbox = &mailbox;
            contexts[producer_index].producer_index = producer_index;
            contexts[producer_index].finished = &finished;
            contexts[producer_index].failures = &failures;
            {
                const int create_status = cmeta_thread_create(
                    &producers[producer_index], mailbox_producer,
                    &contexts[producer_index]);
                check_equal(create_status, 0);
                producer_started[producer_index] = create_status == 0;
                if (create_status != 0) {
                    atomic_fetch_add(&failures, 1);
                    atomic_fetch_add(&finished, 1);
                }
            }
        }

        while (received < MAILBOX_CONCURRENT_EVENT_COUNT) {
            cflow_event_id id = 0u;
            const cmeta_type_desc *type = NULL;
            int output = -1;
            const cflow_mailbox_status status = cflow_mailbox_try_receive(
                &mailbox, &id, &type, &output, sizeof(output));

            if (status == CFLOW_MAILBOX_EMPTY) {
                if (producers_quiescent) break;
                /* A producer may commit after EMPTY but before this load. */
                producers_quiescent =
                    atomic_load(&finished) == MAILBOX_PRODUCER_COUNT;
                if (!producers_quiescent) cmeta_thread_yield();
                continue;
            }
            if (status != CFLOW_MAILBOX_OK) {
                atomic_fetch_add(&failures, 1);
                (void)cflow_mailbox_cancel(&mailbox);
                break;
            }
            if (id != 1u || type != &cmeta_type_int || output < 0 ||
                output >= MAILBOX_CONCURRENT_EVENT_COUNT || seen[output]) {
                atomic_fetch_add(&failures, 1);
                (void)cflow_mailbox_cancel(&mailbox);
                break;
            }
            seen[output] = true;
            ++received;
        }

        for (producer_index = 0;
             producer_index < MAILBOX_PRODUCER_COUNT;
             ++producer_index)
            if (producer_started[producer_index])
                check_equal(cmeta_thread_join(&producers[producer_index]), 0);
        check_equal(atomic_load(&failures), 0);
        check_equal(received, (size_t)MAILBOX_CONCURRENT_EVENT_COUNT);
        for (producer_index = 0;
             producer_index < MAILBOX_CONCURRENT_EVENT_COUNT;
             ++producer_index)
            check_true(seen[producer_index]);

        cflow_mailbox_destroy(&mailbox);
    }
}
