/* Standalone POSIX Salts::Platform runtime smoke; actual source compiled by
 * the chosen compiler. This does not enable TCC's excluded C11 atomics. */
#define _POSIX_C_SOURCE 200809L
#include <salts/thread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "Platform thread probe failure at %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static cmeta_once_t once_flag = SALTS_ONCE_INIT;
static int once_calls;
static void once_callback(void) { ++once_calls; }

typedef struct thread_ctx {
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    const void *caller_token;
    const void *worker_token;
    int completed;
} thread_ctx;

static void worker(void *opaque) {
    thread_ctx *ctx = (thread_ctx *)opaque;
    cmeta_once(&once_flag, once_callback);
    cmeta_mutex_lock(&ctx->mutex);
    ctx->worker_token = cmeta_thread_current_token();
    ctx->completed = 1;
    cmeta_cond_signal(&ctx->cond);
    cmeta_mutex_unlock(&ctx->mutex);
}

int main(void) {
    thread_ctx ctx = {0};
    cmeta_thread_t worker_thread = NULL;
    cmeta_rwlock_t rwlock = NULL;
    ctx.caller_token = cmeta_thread_current_token();
    CHECK(ctx.caller_token != NULL);
    cmeta_mutex_init(&ctx.mutex);
    CHECK(ctx.mutex != NULL);
    cmeta_cond_init(&ctx.cond);
    CHECK(ctx.cond != NULL);

    /* Confirm the monotonic relative deadline and mutex reacquisition. */
    cmeta_mutex_lock(&ctx.mutex);
    CHECK(cmeta_cond_timedwait(&ctx.cond, &ctx.mutex, UINT64_C(1000000)) == -ETIMEDOUT);
    cmeta_mutex_unlock(&ctx.mutex);

    CHECK(cmeta_thread_create(&worker_thread, worker, &ctx) == 0);
    CHECK(worker_thread != NULL);
    cmeta_mutex_lock(&ctx.mutex);
    while (!ctx.completed) {
        int rc = cmeta_cond_timedwait(&ctx.cond, &ctx.mutex,
                                      UINT64_C(5000000000));
        if (rc != 0 && !ctx.completed) {
            fprintf(stderr, "worker condition wait failed: %d\n", rc);
            cmeta_mutex_unlock(&ctx.mutex);
            (void)cmeta_thread_join(&worker_thread);
            return 1;
        }
    }
    int distinct_thread = ctx.worker_token != NULL &&
                          ctx.worker_token != ctx.caller_token;
    cmeta_mutex_unlock(&ctx.mutex);
    CHECK(distinct_thread);
    CHECK(cmeta_thread_join(&worker_thread) == 0);
    CHECK(worker_thread == NULL);
    cmeta_once(&once_flag, once_callback);
    CHECK(once_calls == 1);

    CHECK(cmeta_rwlock_init(&rwlock) == 0 && rwlock != NULL);
    cmeta_rwlock_rdlock(&rwlock);
    cmeta_rwlock_rdunlock(&rwlock);
    cmeta_rwlock_wrlock(&rwlock);
    cmeta_rwlock_wrunlock(&rwlock);
    cmeta_rwlock_destroy(&rwlock);
    CHECK(rwlock == NULL);
    cmeta_cond_destroy(&ctx.cond);
    cmeta_mutex_destroy(&ctx.mutex);
    CHECK(ctx.cond == NULL && ctx.mutex == NULL);
    puts("Salts POSIX thread backend: PASS (diagnostic, not TCC admission)");
    return 0;
}
