#include <cflow/reactive.h>
#include <cflow/publishers.h>
#include <cflow/stream.h>
#include <thread.h>
#include "ops.h"

#include <stdatomic.h>
#include <stdio.h>

typedef struct state {
    atomic_int callbacks;
    atomic_int overlap;
    atomic_long sum;
    atomic_int done;
    atomic_int errors;
} state;

static bool on_value(void *user, const cmeta_type_desc *type, const void *value) {
    state *s = (state *)user;
    if (!cmeta_type_equal(type, &cmeta_type_long)) return false;
    if (atomic_fetch_add(&s->callbacks, 1) != 0) atomic_fetch_add(&s->overlap, 1);
    atomic_fetch_add(&s->sum, *(const long *)value);
    atomic_fetch_sub(&s->callbacks, 1);
    return true;
}
static void on_error(void *user, const char *message) {
    (void)message; atomic_fetch_add(&((state *)user)->errors, 1);
}
static void on_done(void *user) { atomic_store(&((state *)user)->done, 1); }

typedef struct producer {
    cflow_channel *ch;
    int start;
    int count;
} producer;
static void produce(void *arg) {
    producer *p = (producer *)arg;
    for (int i = 0; i < p->count; ++i) {
        int v = p->start + i;
        while (!cflow_channel_push(p->ch, &v)) cmeta_thread_yield();
    }
}

int main(void) {
    cflow_stream s;
    cflow_stream_init(&s, &cmeta_type_int);
    s.map(&s, square);

    cflow_scheduler workers;
    if (!cflow_scheduler_worker_init(&workers, 4)) return 1;
    cflow_channel ch = {0};
    if (!cflow_channel_init(&ch, &cmeta_type_int, 1024)) return 2;
    cflow_publisher source;
    if (!cflow_publisher_from_channel(&source, &ch)) return 3;

    state st = {0};
    cflow_subscriber_callbacks obs_cb = { on_value, on_error, on_done, &st };
    cflow_subscriber obs = cflow_subscriber_from_callbacks(&obs_cb);
    cflow_subscription sub;
    if (!cflow_subscribe(&sub, &s.graph, &source, &workers, &obs)) return 4;

    cmeta_thread_t threads[4] = {0}; producer ps[4];
    for (int t = 0; t < 4; ++t) {
        ps[t] = (producer){ &ch, t * 250 + 1, 250 };
        if (cmeta_thread_create(&threads[t], produce, &ps[t]) != 0) return 6;
    }
    for (int t = 0; t < 4; ++t) (void)cmeta_thread_join(&threads[t]);
    cflow_channel_close(&ch);
    if (!cflow_subscription_request(&sub, 1000)) return 5;
    if (!cflow_scheduler_wait_idle(&workers)) return 7;

    long expected = 0;
    for (long i = 1; i <= 1000; ++i) expected += i * i;
    if (!atomic_load(&st.done) || atomic_load(&st.errors) || atomic_load(&st.overlap) ||
        atomic_load(&st.sum) != expected) return 8;
    printf("worker reactive facade: 1000 values, serialized observer, sum=%ld\n",
           atomic_load(&st.sum));

    cflow_subscription_close(&sub);
    cflow_channel_destroy(&ch);
    cflow_scheduler_destroy(&workers);
    cflow_stream_destroy(&s);
    return 0;
}
