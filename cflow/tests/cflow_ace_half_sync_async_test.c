#include <cflow/cflow.h>
#include <cnet/cnet.h>
#include <clock.h>
#include "tinytest.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { ACE_HANDOFF_TIMEOUT_MS = 7000u };

typedef struct ace_handoff_fixture ace_handoff_fixture;

typedef struct ace_net_binding {
    ace_handoff_fixture *owner;
    bool incoming;
} ace_net_binding;

/*
 * ACE Half-Sync/Half-Async composition test, not a new adapter/runtime:
 * CNet on_receive (borrowed synchronous bytes) -> CFlow bounded Channel
 * -> manually driven CFlow Subscription (downstream demand)
 * -> retained CFlow Actor producer -> Actor worker + serialized Machine.
 */
struct ace_handoff_fixture {
    cnet_client connector;
    cnet_client acceptor;
    cnet_listener listener;
    cnet_connection outbound;
    cnet_connection accepted;
    ace_net_binding connector_binding;
    ace_net_binding acceptor_binding;

    cflow_channel channel;
    cflow_graph surface;
    cflow_graph normalized;
    cflow_scheduler pump_scheduler;
    cflow_publisher publisher;
    cflow_subscriber subscriber;
    cflow_subscriber_callbacks callbacks;
    cflow_subscription subscription;

    cflow_machine machine;
    cflow_executor executor;
    cflow_scheduler actor_scheduler;
    cflow_actor actor;
    cflow_actor_ref producer;
    cflow_machine_action_binding action_binding;
    int initial_state;

    atomic_bool gate_entered;
    atomic_bool gate_release;
    atomic_int actor_action_calls;
    atomic_int actor_values;
    atomic_int actor_last_value;
    atomic_int actor_errors;
    atomic_int actor_done;
    atomic_int expired_callbacks;
    bool actor_context_live;
    bool sink_context_live;
    bool net_context_live;
    size_t source_values;
    size_t source_accepted;
    size_t source_full;
    size_t source_unexpected;
    size_t sink_values;
    size_t sink_accepted;
    size_t sink_full;
    size_t sink_unexpected;
    size_t sink_errors;
    size_t sink_done;
    size_t connector_connected;
    size_t acceptor_connected;
    size_t connector_terminal;
    size_t acceptor_terminal;
    size_t network_errors;
};

static native_io_backend_kind ace_handoff_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static cnet_client_config ace_handoff_client_config(void) {
    cnet_client_config config = {0};
    config.backend = ace_handoff_backend();
    config.connection_capacity = 2u;
    config.command_capacity = 8u;
    config.request_capacity = 8u;
    config.completion_batch_capacity = 8u;
    config.event_capacity = 8u;
    config.max_send_bytes = 1024u;
    config.receive_buffer_bytes = 1024u;
    config.connect_timeout_ms = ACE_HANDOFF_TIMEOUT_MS;
    return config;
}

static bool ace_actor_action(void *user, const void *state, const void *event,
                             void *out_state, void *out_observation,
                             const char **out_error) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    const int payload = event != NULL ? *(const int *)event : 0;
    if (fixture == NULL || state == NULL || event == NULL ||
        out_state == NULL || out_observation == NULL || out_error == NULL)
        return false;
    if (!fixture->actor_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    atomic_fetch_add(&fixture->actor_action_calls, 1);

    /* The actor worker blocks on a test gate, not the CNet poll owner.
     * 99 is a control event already in flight before any TCP bytes arrive. */
    if (payload == 99) {
        const uint64_t deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;
        atomic_store(&fixture->gate_entered, true);
        while (!atomic_load(&fixture->gate_release) &&
               cmeta_monotonic_ms() < deadline)
            cmeta_sleep_ms(1u);
        if (!atomic_load(&fixture->gate_release)) {
            *out_error = "actor gate timed out";
            return false;
        }
    }
    *(int *)out_state = *(const int *)state + 1;
    *(int *)out_observation = payload;
    *out_error = NULL;
    return true;
}

static bool ace_actor_value(void *user, const cmeta_type_desc *type,
                            const void *value) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    if (fixture == NULL || type == NULL || value == NULL ||
        !cmeta_type_equal(type, &cmeta_type_int))
        return false;
    if (!fixture->actor_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    atomic_store(&fixture->actor_last_value, *(const int *)value);
    atomic_fetch_add(&fixture->actor_values, 1);
    return true;
}

static void ace_actor_error(void *user, const char *message) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    if (!fixture->actor_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    if (message != NULL) atomic_fetch_add(&fixture->actor_errors, 1);
}

static void ace_actor_done(void *user) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    if (!fixture->actor_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    atomic_fetch_add(&fixture->actor_done, 1);
}

static bool ace_handoff_sink_value(void *user, const cmeta_type_desc *type,
                                   const void *value) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    cflow_actor_send_status status;
    cflow_event_view event;
    if (!fixture->sink_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    if (!cmeta_type_equal(type, &cmeta_type_int) || value == NULL) {
        ++fixture->sink_unexpected;
        return false;
    }
    /* This is a synchronous, nonblocking copied admission. The borrowed
     * Subscription value cannot be held by Actor after this call. */
    event = (cflow_event_view){100u, &cmeta_type_int, value};
    status = cflow_actor_ref_try_send(&fixture->producer, &event);
    ++fixture->sink_values;
    if (status == CFLOW_ACTOR_SEND_ACCEPTED) {
        ++fixture->sink_accepted;
        return true;
    }
    if (status == CFLOW_ACTOR_SEND_FULL)
        ++fixture->sink_full;
    else
        ++fixture->sink_unexpected;
    /* Propagate backpressure as a failed downstream Subscriber, never retry. */
    return false;
}

static void ace_handoff_sink_error(void *user, const char *message) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    if (!fixture->sink_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    if (message != NULL) ++fixture->sink_errors;
}

static void ace_handoff_sink_done(void *user) {
    ace_handoff_fixture *fixture = (ace_handoff_fixture *)user;
    if (!fixture->sink_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    ++fixture->sink_done;
}

static void ace_handoff_net_state(void *user, cnet_connection connection,
                                  cnet_connection_state state,
                                  const cnet_error *error) {
    const ace_net_binding *binding = (const ace_net_binding *)user;
    ace_handoff_fixture *fixture = binding->owner;
    (void)connection;
    if (!fixture->net_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    if (state == CNET_CONNECTION_CONNECTED) {
        if (binding->incoming) ++fixture->acceptor_connected;
        else ++fixture->connector_connected;
    }
    if (state == CNET_CONNECTION_CLOSED ||
        state == CNET_CONNECTION_FAILED) {
        if (binding->incoming) ++fixture->acceptor_terminal;
        else ++fixture->connector_terminal;
    }
    if (error != NULL || state == CNET_CONNECTION_FAILED)
        ++fixture->network_errors;
}

static void ace_handoff_net_receive(void *user, cnet_connection connection,
                                    const cnet_receive_view *view) {
    const ace_net_binding *binding = (const ace_net_binding *)user;
    ace_handoff_fixture *fixture = binding->owner;
    size_t index;
    (void)connection;
    if (!fixture->net_context_live)
        atomic_fetch_add(&fixture->expired_callbacks, 1);
    if (view == NULL || view->kind != CNET_MESSAGE_BYTES ||
        view->data == NULL || view->size == 0u) {
        ++fixture->source_unexpected;
        return;
    }
    for (index = 0u; index < view->size; ++index) {
        const int payload = (int)((const unsigned char *)view->data)[index];
        const cflow_channel_status status =
            cflow_channel_try_push(&fixture->channel, &payload);
        ++fixture->source_values;
        if (status == CFLOW_CHANNEL_OK)
            ++fixture->source_accepted;
        else if (status == CFLOW_CHANNEL_FULL)
            ++fixture->source_full;
        else
            ++fixture->source_unexpected;
    }
}

static bool ace_handoff_init(ace_handoff_fixture *fixture) {
    const cflow_machine_state state = {
        10u, &cmeta_type_int, CFLOW_MACHINE_STATE_ACTIVE
    };
    const cflow_event_type event = {100u, &cmeta_type_int};
    const cflow_machine_action action = {
        300u, &cmeta_type_int, 100u, &cmeta_type_int,
        &cmeta_type_int, CMETA_EFFECT_MAY_FAIL,
        CMETA_PROP_DETERMINISTIC | CMETA_PROP_NO_ALIAS,
        CFLOW_MACHINE_ACTION_VALUE, &cmeta_type_int, 0u
    };
    const cflow_machine_transition transition = {
        10u, 100u, 0u, 300u, 10u, 1u
    };
    const cflow_machine_definition definition = {
        &state, 1u, 10u,
        &event, 1u,
        NULL, 0u,
        &action, 1u,
        &transition, 1u
    };
    cflow_actor_config actor_config = {0};
    const int kickoff = 99;
    const cflow_event_view control = {100u, &cmeta_type_int, &kickoff};
    const uint64_t deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;

    memset(fixture, 0, sizeof(*fixture));
    fixture->actor_context_live = true;
    fixture->sink_context_live = true;
    fixture->net_context_live = true;
    fixture->normalized.root = CMETA_INVALID_ID;
    atomic_init(&fixture->gate_entered, false);
    atomic_init(&fixture->gate_release, false);
    atomic_init(&fixture->actor_action_calls, 0);
    atomic_init(&fixture->actor_values, 0);
    atomic_init(&fixture->actor_last_value, 0);
    atomic_init(&fixture->actor_errors, 0);
    atomic_init(&fixture->actor_done, 0);
    atomic_init(&fixture->expired_callbacks, 0);

    fixture->action_binding =
        (cflow_machine_action_binding){300u, ace_actor_action, fixture};
    fixture->callbacks = (cflow_subscriber_callbacks){
        ace_handoff_sink_value,
        ace_handoff_sink_error,
        ace_handoff_sink_done, fixture
    };
    fixture->subscriber = cflow_subscriber_from_callbacks(&fixture->callbacks);
    if (!cmeta_interface_desc_valid(cflow_subscriber_interface()) ||
        !cmeta_interface_desc_valid(cflow_publisher_interface()))
        return false;
    if (cflow_machine_build(&fixture->machine, &definition) != CFLOW_MACHINE_OK)
        return false;
    if (!cflow_executor_serial_init(&fixture->executor))
        return false;
    if (!cflow_scheduler_worker_init(&fixture->actor_scheduler, 1u))
        return false;

    actor_config.machine = (cflow_machine_instance_config){
        &fixture->machine, &fixture->initial_state, &cmeta_type_int,
        NULL, 0u,
        &fixture->action_binding, 1u,
        1u, &fixture->executor
    };
    actor_config.scheduler = &fixture->actor_scheduler;
    actor_config.callbacks = (cflow_subscriber_callbacks){
        ace_actor_value, ace_actor_error, ace_actor_done, fixture
    };
    if (cflow_actor_init(&fixture->actor, &actor_config).status != CFLOW_ACTOR_OK ||
        cflow_actor_start(&fixture->actor) != CFLOW_ACTOR_OK ||
        !cflow_actor_ref_acquire(&fixture->actor, &fixture->producer))
        return false;
    if (cflow_actor_ref_try_send(&fixture->producer, &control) !=
        CFLOW_ACTOR_SEND_ACCEPTED)
        return false;

    while (!atomic_load(&fixture->gate_entered) &&
           cmeta_monotonic_ms() < deadline)
        cmeta_sleep_ms(1u);
    if (!atomic_load(&fixture->gate_entered)) return false;

    cflow_graph_init(&fixture->surface, &cmeta_type_int);
    if (!cflow_graph_normalize(&fixture->normalized, &fixture->surface) ||
        !cflow_scheduler_test_init(&fixture->pump_scheduler) ||
        !cflow_channel_init(&fixture->channel, &cmeta_type_int, 2u) ||
        !cflow_publisher_from_channel(&fixture->publisher, &fixture->channel) ||
        !cflow_subscribe(&fixture->subscription, &fixture->normalized,
                         &fixture->publisher, &fixture->pump_scheduler,
                         &fixture->subscriber))
        return false;
    return fixture->publisher.self == NULL;
}

static bool ace_handoff_network(ace_handoff_fixture *fixture) {
    const cnet_client_config config = ace_handoff_client_config();
    const cnet_listener_config listen_config = {
        ace_handoff_backend(), "127.0.0.1", 0u, 8u
    };
    cnet_connect_options connect = {0};
    cnet_observer incoming_observer = {0};
    cnet_observer outgoing_observer = {0};
    uint16_t port = 0u;
    uint64_t deadline;
    int ready = 0;
    char uri[64];
    static const unsigned char messages[] = {1u, 2u, 3u};
    mem_buffer_t *buffer;
    int send_status;

    fixture->connector_binding =
        (ace_net_binding){fixture, false};
    fixture->acceptor_binding =
        (ace_net_binding){fixture, true};
    outgoing_observer.on_state = ace_handoff_net_state;
    outgoing_observer.user = &fixture->connector_binding;
    incoming_observer.on_state = ace_handoff_net_state;
    incoming_observer.on_receive = ace_handoff_net_receive;
    incoming_observer.user = &fixture->acceptor_binding;

    if (cnet_client_init(&fixture->connector, &config) != SALTS_OK ||
        cnet_client_init(&fixture->acceptor, &config) != SALTS_OK ||
        cnet_listener_init(&fixture->listener, &listen_config) != SALTS_OK ||
        cnet_listener_port(&fixture->listener, &port) != SALTS_OK ||
        port == 0u)
        return false;
    if (snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                 (unsigned)port) <= 0)
        return false;
    connect.uri = uri;
    connect.observer = outgoing_observer;
    if (cnet_connect(&fixture->connector, &connect, &fixture->outbound) !=
        SALTS_OK)
        return false;

    deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;
    while (fixture->connector_connected == 0u &&
           cmeta_monotonic_ms() < deadline) {
        size_t events = 0u;
        if (cnet_client_poll(&fixture->connector, 1u, &events) != SALTS_OK)
            return false;
    }
    if (fixture->connector_connected != 1u ||
        cnet_listener_wait(&fixture->listener, ACE_HANDOFF_TIMEOUT_MS,
                           &ready) != SALTS_OK || ready != 1)
        return false;
    if (cnet_listener_accept(&fixture->listener, &fixture->acceptor,
                             &incoming_observer, &fixture->accepted) != SALTS_OK)
        return false;
    deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;
    while (fixture->acceptor_connected == 0u &&
           cmeta_monotonic_ms() < deadline) {
        size_t events = 0u;
        if (cnet_client_poll(&fixture->acceptor, 1u, &events) != SALTS_OK)
            return false;
    }
    if (fixture->acceptor_connected != 1u ||
        cnet_receive(&fixture->acceptor, fixture->accepted, 3u) != SALTS_OK)
        return false;

    buffer = mem_get_buffer(mem_global(), sizeof(messages));
    if (buffer == NULL) return false;
    memcpy(mem_buffer_data(buffer), messages, sizeof(messages));
    mem_set_used(buffer, sizeof(messages));
    send_status = cnet_send_buffer(
        &fixture->connector, fixture->outbound, buffer);
    mem_buffer_release(buffer);
    if (send_status != SALTS_OK) return false;

    deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;
    while (fixture->source_values < 3u &&
           cmeta_monotonic_ms() < deadline) {
        size_t events = 0u;
        if (cnet_client_poll(&fixture->connector, 1u, &events) != SALTS_OK ||
            cnet_client_poll(&fixture->acceptor, 1u, &events) != SALTS_OK)
            return false;
    }
    return fixture->source_values == 3u &&
           fixture->source_unexpected == 0u &&
           fixture->network_errors == 0u;
}

static void ace_handoff_network_stop(ace_handoff_fixture *fixture) {
    const uint64_t deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;
    if (fixture->connector.impl != NULL && fixture->outbound.slot != 0u)
        check_equal(cnet_close(&fixture->connector, fixture->outbound), SALTS_OK);
    if (fixture->acceptor.impl != NULL && fixture->accepted.slot != 0u)
        check_equal(cnet_close(&fixture->acceptor, fixture->accepted), SALTS_OK);
    while (fixture->connector.impl != NULL &&
           fixture->acceptor.impl != NULL &&
           (fixture->connector_terminal == 0u ||
            fixture->acceptor_terminal == 0u) &&
           cmeta_monotonic_ms() < deadline) {
        size_t events = 0u;
        check_equal(cnet_client_poll(&fixture->connector, 1u, &events),
                    SALTS_OK);
        check_equal(cnet_client_poll(&fixture->acceptor, 1u, &events),
                    SALTS_OK);
    }
    if (fixture->connector.impl != NULL) {
        check_equal(cnet_client_stop(&fixture->connector,
                                     ACE_HANDOFF_TIMEOUT_MS), SALTS_OK);
        check_equal(cnet_client_destroy(&fixture->connector), SALTS_OK);
    }
    if (fixture->acceptor.impl != NULL) {
        check_equal(cnet_client_stop(&fixture->acceptor,
                                     ACE_HANDOFF_TIMEOUT_MS), SALTS_OK);
        check_equal(cnet_client_destroy(&fixture->acceptor), SALTS_OK);
    }
    if (fixture->listener.impl != NULL) {
        check_equal(cnet_listener_close(&fixture->listener), SALTS_OK);
        check_equal(cnet_listener_destroy(&fixture->listener), SALTS_OK);
    }
    /* CNet owns exactly one terminal per connection. Both owner handles
     * must be empty before their callback context can expire. */
    check_equal(fixture->connector_terminal, (size_t)1u);
    check_equal(fixture->acceptor_terminal, (size_t)1u);
    check_null(fixture->connector.impl);
    check_null(fixture->acceptor.impl);
    check_null(fixture->listener.impl);
    fixture->net_context_live = false;
}

static void ace_handoff_finish(ace_handoff_fixture *fixture) {
    const int final = 7;
    const cflow_event_view event = {100u, &cmeta_type_int, &final};
    cflow_actor_status stop;

    /* Never block Actor destruction behind an unreleased test action. */
    atomic_store(&fixture->gate_release, true);

    /* CNet callback dispatch is quiesced before its borrowed Channel is
     * closed/destroyed. NativeIO requests and listener remain CNet-owned. */
    ace_handoff_network_stop(fixture);

    cflow_subscription_cancel(&fixture->subscription);
    cflow_subscription_close(&fixture->subscription);
    check_null(fixture->subscription.impl);
    /* Idempotent close: the moved Publisher must not be settled twice. */
    cflow_subscription_close(&fixture->subscription);
    check_null(fixture->subscription.impl);
    check_null(fixture->publisher.self);
    fixture->sink_context_live = false;

    if (fixture->publisher.self != NULL)
        cflow_publisher_destroy(&fixture->publisher);
    if (fixture->channel.impl != NULL) {
        cflow_channel_close(&fixture->channel);
        check_equal(cflow_channel_try_push(&fixture->channel, &final),
                    CFLOW_CHANNEL_CLOSED);
        cflow_channel_destroy(&fixture->channel);
        check_null(fixture->channel.impl);
    }
    if (fixture->actor.impl != NULL) {
        stop = cflow_actor_request_stop(&fixture->actor);
        check_true(stop == CFLOW_ACTOR_OK || stop == CFLOW_ACTOR_STOPPED);
        check_equal(cflow_actor_wait(&fixture->actor),
                    CFLOW_ACTOR_STATE_STOPPED);
        if (fixture->producer.impl != NULL)
            check_equal(cflow_actor_ref_try_send(&fixture->producer, &event),
                        CFLOW_ACTOR_SEND_STOPPED);
        check_equal(atomic_load(&fixture->actor_done), 1);
        check_equal(atomic_load(&fixture->actor_errors), 0);
        cflow_actor_destroy(&fixture->actor);
        check_null(fixture->actor.impl);
    }
    if (fixture->producer.impl != NULL) {
        check_equal(cflow_actor_ref_try_send(&fixture->producer, &event),
                    CFLOW_ACTOR_SEND_STALE);
        cflow_actor_ref_release(&fixture->producer);
        check_null(fixture->producer.impl);
    }
    fixture->actor_context_live = false;
    if (cflow_scheduler_valid(&fixture->pump_scheduler))
        cflow_scheduler_destroy(&fixture->pump_scheduler);
    if (cflow_scheduler_valid(&fixture->actor_scheduler))
        cflow_scheduler_destroy(&fixture->actor_scheduler);
    if (cflow_executor_valid(&fixture->executor))
        cflow_executor_destroy(&fixture->executor);
    cflow_graph_destroy(&fixture->normalized);
    cflow_graph_destroy(&fixture->surface);
    cflow_machine_destroy(&fixture->machine);
    check_equal(atomic_load(&fixture->expired_callbacks), 0);
}

static void ace_handoff_assert_network_capacity(ace_handoff_fixture *fixture) {
    cflow_channel_stats stats = {0};
    check_equal(fixture->source_values, (size_t)3u);
    check_equal(fixture->source_accepted, (size_t)2u);
    check_equal(fixture->source_full, (size_t)1u);
    check_equal(fixture->source_unexpected, (size_t)0u);
    check_true(cflow_channel_get_stats(&fixture->channel, &stats));
    check_equal(stats.capacity, (size_t)2u);
    check_equal(stats.pending, (size_t)2u);
    check_equal(stats.peak_pending, (size_t)2u);
    check_equal(stats.accepted, (uint64_t)2u);
    check_equal(stats.rejected_full, (uint64_t)1u);
    check_equal(fixture->sink_values, (size_t)0u);
    check_equal(atomic_load(&fixture->actor_action_calls), 1);
    check_equal(atomic_load(&fixture->actor_values), 0);
}

static bool ace_handoff_wait_actor_values(ace_handoff_fixture *fixture,
                                         int minimum) {
    const uint64_t deadline = cmeta_monotonic_ms() + ACE_HANDOFF_TIMEOUT_MS;
    while (atomic_load(&fixture->actor_values) < minimum &&
           cmeta_monotonic_ms() < deadline)
        cmeta_sleep_ms(1u);
    return atomic_load(&fixture->actor_values) >= minimum;
}

suite("ACE Half-Sync/Half-Async CNet-to-CFlow owner handoff") {
    it("propagates both bounded Channel FULL and Actor mailbox FULL") {
        ace_handoff_fixture fixture;
        const char *error;

        check_true(ace_handoff_init(&fixture));
        check_true(ace_handoff_network(&fixture));
        ace_handoff_assert_network_capacity(&fixture);

        check_true(cflow_subscription_request(&fixture.subscription, 1u));
        (void)cflow_scheduler_run_until_idle(&fixture.pump_scheduler, 0u);
        check_equal(fixture.sink_values, (size_t)1u);
        check_equal(fixture.sink_accepted, (size_t)1u);
        check_equal(fixture.sink_full, (size_t)0u);

        /* Kickoff still executes on the Actor worker; the one-slot Actor
         * mailbox now holds the first TCP-sourced typed message. */
        check_true(cflow_subscription_request(&fixture.subscription, 1u));
        (void)cflow_scheduler_run_until_idle(&fixture.pump_scheduler, 0u);
        check_equal(fixture.sink_values, (size_t)2u);
        check_equal(fixture.sink_accepted, (size_t)1u);
        check_equal(fixture.sink_full, (size_t)1u);
        check_equal(fixture.sink_unexpected, (size_t)0u);
        check_equal(fixture.sink_errors, (size_t)1u);
        error = cflow_subscription_error(&fixture.subscription);
        check_not_null(error);
        if (error != NULL) check_contains(error, "observer rejected value");

        atomic_store(&fixture.gate_release, true);
        check_true(ace_handoff_wait_actor_values(&fixture, 2));
        check_equal(atomic_load(&fixture.actor_action_calls), 2);
        check_equal(atomic_load(&fixture.actor_last_value), 1);
        check_equal(atomic_load(&fixture.actor_errors), 0);
        ace_handoff_finish(&fixture);
    }

    it("cancels pending demand, rejects STOPPING sends and drains one in-flight action") {
        ace_handoff_fixture fixture;
        const int rejected = 8;
        const cflow_event_view rejected_event = {
            100u, &cmeta_type_int, &rejected
        };

        check_true(ace_handoff_init(&fixture));
        check_true(ace_handoff_network(&fixture));
        ace_handoff_assert_network_capacity(&fixture);
        check_true(cflow_subscription_request(&fixture.subscription, 1u));
        (void)cflow_scheduler_run_until_idle(&fixture.pump_scheduler, 0u);
        check_equal(fixture.sink_accepted, (size_t)1u);

        /* The second channel item is not replayed on cancellation. */
        cflow_subscription_cancel(&fixture.subscription);
        (void)cflow_scheduler_run_until_idle(&fixture.pump_scheduler, 0u);
        check_true(cflow_subscription_is_cancelled(&fixture.subscription));
        check_equal(fixture.sink_values, (size_t)1u);
        check_equal(fixture.sink_full, (size_t)0u);

        /* Stop admission while the kickoff action is still blocked.
         * The one queued TCP-sourced Event is cancelled, never replayed. */
        check_equal(cflow_actor_request_stop(&fixture.actor), CFLOW_ACTOR_OK);
        check_equal(cflow_actor_current_state(&fixture.actor),
                    CFLOW_ACTOR_STATE_STOPPING);
        check_equal(cflow_actor_ref_try_send(
            &fixture.producer, &rejected_event), CFLOW_ACTOR_SEND_STOPPING);

        atomic_store(&fixture.gate_release, true);
        check_equal(cflow_actor_wait(&fixture.actor), CFLOW_ACTOR_STATE_STOPPED);
        check_true(ace_handoff_wait_actor_values(&fixture, 1));
        check_equal(atomic_load(&fixture.actor_action_calls), 1);
        check_equal(atomic_load(&fixture.actor_values), 1);
        check_equal(atomic_load(&fixture.actor_last_value), 99);
        check_equal(fixture.sink_accepted, (size_t)1u);
        ace_handoff_finish(&fixture);
    }
}
