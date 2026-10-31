#include "tinytest.h"
#include <salts/service_host.h>
#include <salts/thread.h>

/* This executable links the production lifecycle TU with this single-thread
 * platform adapter, without the graph runtime or a real platform mutex object.
 * No production test hooks or symbol replacement flags are required. */
static bool fail_mutex;
static int mutex_token;
static size_t init_calls;
static size_t destroy_calls;
static int lock_depth;
static bool adapter_error;

void cmeta_mutex_init(cmeta_mutex_t *mutex) {
    ++init_calls;
    *mutex = fail_mutex ? NULL : &mutex_token;
}

void cmeta_mutex_destroy(cmeta_mutex_t *mutex) {
    if (*mutex != &mutex_token || lock_depth != 0) adapter_error = true;
    ++destroy_calls;
    *mutex = NULL;
}

void cmeta_mutex_lock(cmeta_mutex_t *mutex) {
    if (*mutex != &mutex_token || lock_depth != 0) adapter_error = true;
    ++lock_depth;
}

void cmeta_mutex_unlock(cmeta_mutex_t *mutex) {
    if (*mutex != &mutex_token || lock_depth != 1) adapter_error = true;
    --lock_depth;
}

suite("ServiceHost platform resource admission") {
    static salts_service_host_runtime runtime;

    before_each() {
        runtime = (salts_service_host_runtime){0};
        fail_mutex = true;
        init_calls = destroy_calls = 0u;
        lock_depth = 0;
        adapter_error = false;
    }
    after_each() {
        if (runtime.initialized) {
            runtime.current = NULL;
            runtime.active_scopes = runtime.attached_generations = 0u;
            check_equal(salts_service_host_runtime_destroy(&runtime),
                        SALTS_SERVICE_HOST_OK);
        }
        check_equal(lock_depth, 0);
        check_false(adapter_error);
    }

    it("rejects mutex creation failure and permits a clean retry") {
        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_RESOURCE_ERROR);
        check_false(runtime.initialized);
        check_null(runtime.lock);
        check_null(runtime.current);
        check_equal(runtime.last_generation_id, UINT64_C(0));
        check_equal(runtime.active_scopes, (size_t)0u);
        check_equal(runtime.attached_generations, (size_t)0u);
        check_equal(salts_service_host_runtime_destroy(&runtime),
                    SALTS_SERVICE_HOST_INVALID_ARGUMENT);
        check_equal(destroy_calls, (size_t)0u);

        fail_mutex = false;
        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_OK);
        check_true(runtime.initialized);
        check_equal(init_calls, (size_t)2u);
        check_equal(salts_service_host_runtime_destroy(&runtime),
                    SALTS_SERVICE_HOST_OK);
        check_equal(destroy_calls, (size_t)1u);
        check_false(runtime.initialized);
        check_null(runtime.lock);
    }

    it("does not replace an initialized lock or initialize NULL") {
        check_equal(salts_service_host_runtime_init(NULL),
                    SALTS_SERVICE_HOST_INVALID_ARGUMENT);
        check_equal(init_calls, (size_t)0u);
        fail_mutex = false;
        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_OK);
        fail_mutex = true;
        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_INVALID_ARGUMENT);
        check_true(runtime.initialized);
        check_true(runtime.lock == &mutex_token);
        check_equal(init_calls, (size_t)1u);
    }

    it("retains the lock until current, scope and generation owners are gone") {
        salts_service_host_generation generation = {0};
        fail_mutex = false;
        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_OK);
        runtime.current = &generation;
        check_equal(salts_service_host_runtime_destroy(&runtime),
                    SALTS_SERVICE_HOST_BUSY);
        runtime.current = NULL;
        runtime.active_scopes = 1u;
        check_equal(salts_service_host_runtime_destroy(&runtime),
                    SALTS_SERVICE_HOST_BUSY);
        runtime.active_scopes = 0u;
        runtime.attached_generations = 1u;
        check_equal(salts_service_host_runtime_destroy(&runtime),
                    SALTS_SERVICE_HOST_BUSY);
        check_equal(destroy_calls, (size_t)0u);
        check_equal(lock_depth, 0);
        check_true(runtime.initialized);
    }
}
