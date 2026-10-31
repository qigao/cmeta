#include <cflow/cflow.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public, separately built CMeta::CFlow only: no source-tree test fixtures. */
cmeta_function(filter, value, bool, ace_installed_even, (int v)) {
    return v % 2 == 0;
}

cmeta_function(map, value, int, ace_installed_plus_ten, (int v)) {
    return v + 10;
}

typedef struct installed_pipe_sink {
    int received[3];
    size_t count;
    size_t errors;
    size_t done;
} installed_pipe_sink;

static bool installed_pipe_value(void *self, const cmeta_type_desc *type,
                                 const void *value) {
    installed_pipe_sink *sink = (installed_pipe_sink *)self;
    if (sink == NULL || !cmeta_type_equal(type, &cmeta_type_int) ||
        value == NULL || sink->count >= 3u)
        return false;
    sink->received[sink->count++] = *(const int *)value;
    return true;
}

static void installed_pipe_error(void *self, const char *message) {
    installed_pipe_sink *sink = (installed_pipe_sink *)self;
    if (sink != NULL && message != NULL)
        ++sink->errors;
}

static void installed_pipe_done(void *self) {
    installed_pipe_sink *sink = (installed_pipe_sink *)self;
    if (sink != NULL)
        ++sink->done;
}

CMETA_IMPLEMENTS(cflow_subscriber, ace_installed_pipe_sink_impl, 0u,
    .value = installed_pipe_value,
    .error = installed_pipe_error,
    .done = installed_pipe_done);

int main(void) {
    const int input[] = {1, 2, 3, 4};
    const int pending[] = {1, 2, 3};
    cflow_graph surface = {0};
    cflow_graph normalized = {0};
    cflow_scheduler scheduler = {0};
    cflow_subscription run = {0};
    cflow_publisher publisher = {0};
    cflow_channel channel = {0};
    cflow_channel_stats stats = {0};
    installed_pipe_sink state = {0};
    cflow_subscriber sink =
        ace_installed_pipe_sink_impl_as_cflow_subscriber(&state);
    int result = 0;

    normalized.root = CMETA_INVALID_ID;
    if (!cmeta_interface_desc_valid(cflow_subscriber_interface()) ||
        !cflow_subscriber_valid(&sink)) {
        result = 1; goto finish;
    }
    cflow_graph_init(&surface, &cmeta_type_int);
    if (!cflow_graph_add(&surface, CFLOW_OP_FILTER,
                         ace_installed_even.fn, NULL) ||
        !cflow_graph_add(&surface, CFLOW_OP_MAP,
                         ace_installed_plus_ten.fn, NULL) ||
        !cflow_graph_normalize(&normalized, &surface)) {
        result = 2; goto finish;
    }
    if (!cflow_scheduler_test_init(&scheduler)) {
        result = 3; goto finish;
    }
    if (!cflow_publisher_from_array(&publisher, &cmeta_type_int,
                                   input, sizeof(input)/sizeof(input[0]))) {
        result = 4; goto finish;
    }
    if (!cflow_subscribe(&run, &normalized, &publisher, &scheduler, &sink) ||
        publisher.self != NULL) {
        result = 5; goto finish;
    }

    /* One downstream request consumes filtered upstream items, no eagerness. */
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    if (state.count != 0u) { result = 6; goto finish; }
    if (!cflow_subscription_request(&run, 1u)) {
        result = 7; goto finish;
    }
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    if (state.count != 1u || state.received[0] != 12 ||
        state.errors != 0u ||
        cflow_subscription_outstanding_demand(&run) != 0u) {
        result = 8; goto finish;
    }

    cflow_subscription_cancel(&run);
    (void)cflow_scheduler_run_until_idle(&scheduler, 0u);
    cflow_subscription_close(&run);
    if (run.impl != NULL || state.count != 1u) {
        result = 9; goto finish;
    }

    if (!cflow_channel_init(&channel, &cmeta_type_int, 2u)) {
        result = 10; goto finish;
    }
    if (cflow_channel_try_push(&channel, &pending[0]) != CFLOW_CHANNEL_OK ||
        cflow_channel_try_push(&channel, &pending[1]) != CFLOW_CHANNEL_OK ||
        cflow_channel_try_push(&channel, &pending[2]) != CFLOW_CHANNEL_FULL ||
        !cflow_channel_get_stats(&channel, &stats) ||
        stats.capacity != 2u || stats.pending != 2u ||
        stats.peak_pending != 2u || stats.rejected_full != 1u) {
        result = 11; goto finish;
    }
    cflow_channel_close(&channel);
    if (cflow_channel_try_push(&channel, &pending[2]) !=
        CFLOW_CHANNEL_CLOSED) {
        result = 12; goto finish;
    }

finish:
    cflow_subscription_close(&run);
    if (publisher.self != NULL)
        cflow_publisher_destroy(&publisher);
    cflow_channel_destroy(&channel);
    if (cflow_scheduler_valid(&scheduler))
        cflow_scheduler_destroy(&scheduler);
    cflow_graph_destroy(&normalized);
    cflow_graph_destroy(&surface);
    return result;
}
