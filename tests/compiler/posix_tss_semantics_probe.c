/* Independent POSIX TSS conformance inspired by zelang-dev/c-threads.
 * Unlike that project's lazy TLS macros, key creation is guarded by
 * pthread_once and does not implicitly alter Salts' TLS ABI or public API.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define WORKERS 16
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "TSS probe failed: %s (line %d)\n", #expr, __LINE__); \
        return 1; \
    } \
} while (0)

typedef struct tss_payload {
    unsigned id;
    unsigned marker;
} tss_payload;

static pthread_key_t tss_key;
static pthread_once_t tss_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static unsigned once_calls, destructor_calls, waiting, initialized;
static int init_success, begin, finish;
static tss_payload *addresses[WORKERS];
static int worker_ok[WORKERS];
static unsigned worker_indices[WORKERS];

static void free_thread_payload(void *value) {
    free(value);
    (void)pthread_mutex_lock(&gate);
    ++destructor_calls;
    (void)pthread_cond_broadcast(&gate_cv);
    (void)pthread_mutex_unlock(&gate);
}

static void init_key_once(void) {
    ++once_calls;
    init_success = pthread_key_create(&tss_key, free_thread_payload) == 0;
}

static tss_payload *thread_payload(unsigned id) {
    if (pthread_once(&tss_once, init_key_once) != 0 || !init_success)
        return NULL;
    tss_payload *value = (tss_payload *)pthread_getspecific(tss_key);
    if (value != NULL)
        return value;
    value = (tss_payload *)calloc(1, sizeof(*value));
    if (value == NULL)
        return NULL;
    value->id = id;
    value->marker = 1000u + id;
    if (pthread_setspecific(tss_key, value) != 0) {
        free(value);
        return NULL;
    }
    return value;
}

static void *worker(void *arg) {
    const unsigned id = *(const unsigned *)arg;
    (void)pthread_mutex_lock(&gate);
    ++waiting;
    (void)pthread_cond_broadcast(&gate_cv);
    while (!begin)
        (void)pthread_cond_wait(&gate_cv, &gate);
    (void)pthread_mutex_unlock(&gate);

    tss_payload *value = thread_payload(id);
    const int valid = value != NULL &&
                      value == thread_payload(id) &&
                      value->id == id && value->marker == 1000u + id;

    (void)pthread_mutex_lock(&gate);
    addresses[id] = value;
    worker_ok[id] = valid;
    ++initialized;
    (void)pthread_cond_broadcast(&gate_cv);
    while (!finish)
        (void)pthread_cond_wait(&gate_cv, &gate);
    (void)pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void) {
    pthread_t threads[WORKERS];
    unsigned created = 0;
    for (unsigned i = 0; i < WORKERS; ++i) {
        worker_indices[i] = i;
        if (pthread_create(&threads[i], NULL, worker, &worker_indices[i]) != 0)
            break;
        ++created;
    }
    if (created != WORKERS) {
        (void)pthread_mutex_lock(&gate);
        begin = finish = 1;
        (void)pthread_cond_broadcast(&gate_cv);
        (void)pthread_mutex_unlock(&gate);
        for (unsigned i = 0; i < created; ++i)
            (void)pthread_join(threads[i], NULL);
        fprintf(stderr, "TSS probe thread creation failed\n");
        return 1;
    }
    (void)pthread_mutex_lock(&gate);
    while (waiting != WORKERS)
        (void)pthread_cond_wait(&gate_cv, &gate);
    begin = 1;  /* all workers race into pthread_once on their first get */
    (void)pthread_cond_broadcast(&gate_cv);
    while (initialized != WORKERS)
        (void)pthread_cond_wait(&gate_cv, &gate);
    int distinct = 1;
    for (unsigned i = 0; i < WORKERS; ++i) {
        if (!worker_ok[i] || addresses[i] == NULL)
            distinct = 0;
        for (unsigned j = 0; j < i; ++j)
            if (addresses[i] == addresses[j])
                distinct = 0;
    }
    finish = 1;
    (void)pthread_cond_broadcast(&gate_cv);
    (void)pthread_mutex_unlock(&gate);

    for (unsigned i = 0; i < WORKERS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(distinct);
    CHECK(once_calls == 1);
    CHECK(init_success);
    CHECK(destructor_calls == WORKERS);

    /* The caller still owns its value and must explicitly clear it before
     * deleting the shared key. pthread_key_delete does not run destructors. */
    tss_payload *self = thread_payload(200u);
    CHECK(self != NULL && self->id == 200u && self->marker == 1200u);
    CHECK(pthread_setspecific(tss_key, NULL) == 0);
    free(self);
    CHECK(pthread_key_delete(tss_key) == 0);
    CHECK(destructor_calls == WORKERS);
    CHECK(pthread_cond_destroy(&gate_cv) == 0);
    CHECK(pthread_mutex_destroy(&gate) == 0);
    puts("POSIX pthread_once/TSS first-use, isolation and destructor: PASS (diagnostic only)");
    return 0;
}
