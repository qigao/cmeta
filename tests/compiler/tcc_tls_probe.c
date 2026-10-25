/* Probe native compiler TLS independently of the optional C11 threads library.
 * Do NOT undefine __STDC_NO_THREADS__ or create a fake threads.h. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "TLS probe failed at %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

enum { THREADS = 4 };
static _Thread_local unsigned per_thread_value;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t change = PTHREAD_COND_INITIALIZER;
static int ready;
static int start;
static void *tokens[THREADS];
static int failed[THREADS];
static unsigned indexes[THREADS];

static void *worker(void *arg) {
    unsigned index = *(const unsigned *)arg;
    per_thread_value = index + 1u;
    (void)pthread_mutex_lock(&lock);
    tokens[index] = &per_thread_value;
    ++ready;
    (void)pthread_cond_broadcast(&change);
    while (!start) (void)pthread_cond_wait(&change, &lock);
    (void)pthread_mutex_unlock(&lock);
    if (per_thread_value != index + 1u) failed[index] = 1;
    return NULL;
}

int main(void) {
#if defined(__STDC_NO_THREADS__)
    puts("compiler declares C11 threads unavailable; testing native TLS syntax separately");
#endif
    per_thread_value = 1000u;
    void *main_token = &per_thread_value;
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) {
        indexes[i] = i;
        CHECK(pthread_create(&threads[i], NULL, worker, &indexes[i]) == 0);
    }
    (void)pthread_mutex_lock(&lock);
    while (ready != THREADS) (void)pthread_cond_wait(&change, &lock);
    int distinct = 1;
    for (unsigned i = 0; i < THREADS; ++i) {
        if (tokens[i] == NULL || tokens[i] == main_token) distinct = 0;
        for (unsigned j = 0; j < i; ++j)
            if (tokens[i] == tokens[j]) distinct = 0;
    }
    start = 1;
    (void)pthread_cond_broadcast(&change);
    (void)pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < THREADS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(failed[i] == 0);
    }
    CHECK(distinct);
    CHECK(per_thread_value == 1000u);
    CHECK(pthread_cond_destroy(&change) == 0);
    CHECK(pthread_mutex_destroy(&lock) == 0);
    puts("native _Thread_local pthread isolation: PASS (not compiler admission)");
    return 0;
}
