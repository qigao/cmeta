#include "service_host_publication_fixture.h"
#include "tinytest.h"
#include <thread.h>

enum { PUBLICATION_READERS = 4, PUBLICATION_RACE_ROUNDS = 32 };
static const uint64_t PUBLICATION_WAIT_NS = UINT64_C(5000000000);

typedef struct publication_gate {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    size_t entered;
    size_t advanced;
    unsigned phase;
} publication_gate;

typedef struct publication_reader {
    publication_gate *gate;
    salts_service_host_runtime *runtime;
    bool race_publish;
    salts_service_host_status acquire_old;
    salts_service_host_status release_old;
    salts_service_host_status acquire_new;
    salts_service_host_status release_new;
    salts_service_host_status acquire_closed;
    uint64_t old_id;
    uint64_t new_id;
    int old_value;
    int new_value;
} publication_reader;

typedef struct publication_drainer {
    publication_gate *gate;
    salts_service_host_runtime *runtime;
    salts_service_host_generation *generation;
    salts_service_host_status status;
} publication_drainer;

static void gate_arrive(publication_gate *gate, size_t *count, unsigned phase) {
    cmeta_mutex_lock(&gate->mutex);
    ++*count;
    cmeta_cond_broadcast(&gate->changed);
    while (gate->phase < phase)
        cmeta_cond_wait(&gate->changed, &gate->mutex);
    cmeta_mutex_unlock(&gate->mutex);
}

static int gate_wait(publication_gate *gate, size_t *count) {
    int status = 0;
    cmeta_mutex_lock(&gate->mutex);
    while (*count != PUBLICATION_READERS && status == 0)
        status = cmeta_cond_timedwait(&gate->changed, &gate->mutex,
                                      PUBLICATION_WAIT_NS);
    cmeta_mutex_unlock(&gate->mutex);
    return status;
}

static void gate_advance(publication_gate *gate, unsigned phase) {
    cmeta_mutex_lock(&gate->mutex);
    gate->phase = phase;
    cmeta_cond_broadcast(&gate->changed);
    cmeta_mutex_unlock(&gate->mutex);
}

/* Workers record results; TinyTest assertions run only after synchronization on
 * the runner thread. A live scope is never copied or concurrently released. */
static void publication_worker(void *arg) {
    publication_reader *reader = arg;
    salts_service_host_scope old_scope = {0};
    salts_service_host_scope new_scope = {0};
    salts_service_host_scope closed_scope = {0};

    if (reader->race_publish)
        gate_arrive(reader->gate, &reader->gate->entered, 1u);
    reader->acquire_old = salts_service_host_scope_acquire(
        reader->runtime, &old_scope);
    if (!reader->race_publish)
        gate_arrive(reader->gate, &reader->gate->entered, 1u);
    if (reader->acquire_old == SALTS_SERVICE_HOST_OK) {
        reader->old_id = salts_service_host_scope_generation_id(&old_scope);
        reader->old_value = scope_value(&old_scope);
    }
    reader->acquire_new = salts_service_host_scope_acquire(
        reader->runtime, &new_scope);
    if (reader->acquire_new == SALTS_SERVICE_HOST_OK)
        reader->new_id = salts_service_host_scope_generation_id(&new_scope);
    if (old_scope.live)
        reader->release_old = salts_service_host_scope_release(&old_scope);
    gate_arrive(reader->gate, &reader->gate->advanced, 2u);
    if (reader->acquire_new == SALTS_SERVICE_HOST_OK)
        reader->new_value = scope_value(&new_scope);
    reader->acquire_closed = salts_service_host_scope_acquire(
        reader->runtime, &closed_scope);
    if (closed_scope.live)
        (void)salts_service_host_scope_release(&closed_scope);
    if (new_scope.live)
        reader->release_new = salts_service_host_scope_release(&new_scope);
}

static void drain_worker(void *arg) {
    publication_drainer *drainer = arg;
    gate_arrive(drainer->gate, &drainer->gate->entered, 1u);
    drainer->status = salts_service_host_generation_drain(
        drainer->runtime, drainer->generation);
}

suite("ServiceHost concurrent admission and drain") {
    static cmeta_plugin_registry registry;
    static cmeta_plugin_ref ref;
    static bool plugin_started;
    static publication_generation_fixture generations[2];
    static salts_service_host_runtime runtime;
    static publication_gate gate;
    static publication_reader readers[PUBLICATION_READERS];
    static publication_drainer drainers[PUBLICATION_READERS];
    static cmeta_thread_t threads[PUBLICATION_READERS];

    before_each() {
        registry = (cmeta_plugin_registry){0};
        plugin_started = false;
        runtime = (salts_service_host_runtime){0};
        gate = (publication_gate){0};
        for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
            threads[i] = NULL;
            readers[i] = (publication_reader){0};
        }
        for (size_t i = 0; i < 2u; ++i)
            generations[i] = (publication_generation_fixture){0};
        cmeta_mutex_init(&gate.mutex);
        cmeta_cond_init(&gate.changed);
        check_not_null(gate.mutex);
        check_not_null(gate.changed);
        const cmeta_plugin_registry_config config = {1};
        check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_load(
            &registry, COMPONENT_PROVIDER_PLUGIN_PATH, &ref), CMETA_PLUGIN_OK);
        check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
        plugin_started = true;
        check_equal(salts_service_host_runtime_init(&runtime),
                    SALTS_SERVICE_HOST_OK);
    }

    after_each() {
        cmeta_plugin_status stop_status = CMETA_PLUGIN_OK;
        cmeta_plugin_status quiescent_status = CMETA_PLUGIN_OK;
        cmeta_plugin_status destroy_status = CMETA_PLUGIN_OK;
        bool quiescent = true;
        /* Fatal runner checks must still wake/join workers before releasing the
         * borrowed generation bundles, DSO leases, or synchronization objects. */
        if (gate.mutex != NULL && gate.changed != NULL) gate_advance(&gate, 2u);
        for (size_t i = 0; i < PUBLICATION_READERS; ++i)
            if (threads[i] != NULL)
                check_equal_warn(cmeta_thread_join(&threads[i]), 0);
        if (runtime.initialized) {
            salts_service_host_generation *previous = NULL;
            if (runtime.current != NULL)
                check_equal_warn(salts_service_host_runtime_close(
                    &runtime, &previous), SALTS_SERVICE_HOST_OK);
            for (size_t i = 0; i < 2u; ++i) {
                salts_service_host_generation *g = &generations[i].generation;
                if (g->state == SALTS_SERVICE_HOST_GENERATION_BUILT)
                    check_equal_warn(salts_service_host_generation_discard(g),
                                     SALTS_SERVICE_HOST_OK);
                if (g->runtime_owner == &runtime)
                    check_equal_warn(salts_service_host_generation_drain(&runtime, g),
                                     SALTS_SERVICE_HOST_OK);
            }
            check_equal_warn(salts_service_host_runtime_destroy(&runtime),
                             SALTS_SERVICE_HOST_OK);
        }
        if (registry.impl != NULL) {
            if (plugin_started) {
                stop_status = cmeta_plugin_registry_request_stop(&registry, ref);
                if (stop_status == CMETA_PLUGIN_OK) {
                    /* The registry's STOPPING state is not yet QUIESCENT.
                     * Poll after the last reader, generation and DSO lease
                     * has drained; destroy() otherwise returns BUSY and
                     * intentionally retains the registry allocation. */
                    quiescent = false;
                    quiescent_status = cmeta_plugin_registry_poll_quiescent(
                        &registry, ref, &quiescent);
                }
            }
            destroy_status = cmeta_plugin_registry_destroy(&registry);
        }
        cmeta_cond_destroy(&gate.changed);
        cmeta_mutex_destroy(&gate.mutex);
        /* Cleanup failure is not a warning: it is an ownership-contract
         * violation and must fail even an otherwise successful concurrency
         * test. Do not suppress LeakSanitizer. */
        check_equal(stop_status, CMETA_PLUGIN_OK);
        check_equal(quiescent_status, CMETA_PLUGIN_OK);
        check_true(quiescent);
        check_equal(destroy_status, CMETA_PLUGIN_OK);
        check_null(registry.impl);
    }

    it("pins the old DSO generation across publication and the new one across close") {
        salts_service_host_generation *previous = NULL;
        cmeta_plugin_lifecycle_info info;
        salts_service_host_scope closed_scope = {0};
        for (size_t i = 0; i < 2u; ++i)
            check_equal(build_generation(&generations[i], i + 1u, &registry, ref),
                        SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &generations[0].generation, &previous), SALTS_SERVICE_HOST_OK);
        for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
            readers[i].gate = &gate;
            readers[i].runtime = &runtime;
            check_equal(cmeta_thread_create(&threads[i], publication_worker, &readers[i]), 0);
        }
        check_equal(gate_wait(&gate, &gate.entered), 0);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &generations[1].generation, &previous), SALTS_SERVICE_HOST_OK);
        check_true(previous == &generations[0].generation);
        check_equal(salts_service_host_generation_drain(&runtime, previous),
                    SALTS_SERVICE_HOST_BUSY);
        check_equal(cmeta_plugin_registry_get_lifecycle(&registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)2u);
        gate_advance(&gate, 1u);
        check_equal(gate_wait(&gate, &gate.advanced), 0);
        check_equal(salts_service_host_generation_drain(&runtime, previous),
                    SALTS_SERVICE_HOST_OK);
        check_null(previous->storage.deployments);
        check_null(previous->storage.instances);
        check_null(previous->storage.modules);
        check_null(previous->components.deployments);
        check_equal(previous->components.state,
                    SALTS_CONFIGURATOR_CONTEXT_ZERO);
        check_equal(cmeta_plugin_registry_get_lifecycle(&registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)1u);
        check_equal(salts_service_host_runtime_close(&runtime, &previous),
                    SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_scope_acquire(&runtime, &closed_scope),
                    SALTS_SERVICE_HOST_INVALID_STATE);
        check_false(closed_scope.live);
        check_equal(salts_service_host_generation_drain(&runtime, previous),
                    SALTS_SERVICE_HOST_BUSY);
        gate_advance(&gate, 2u);
        for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), 0);
            check_equal(readers[i].acquire_old, SALTS_SERVICE_HOST_OK);
            check_equal(readers[i].release_old, SALTS_SERVICE_HOST_OK);
            check_equal(readers[i].old_id, UINT64_C(1));
            check_equal(readers[i].old_value, COMPONENT_PROVIDER_VALUE);
            check_equal(readers[i].acquire_new, SALTS_SERVICE_HOST_OK);
            check_equal(readers[i].release_new, SALTS_SERVICE_HOST_OK);
            check_equal(readers[i].new_id, UINT64_C(2));
            check_equal(readers[i].new_value, COMPONENT_PROVIDER_VALUE);
            check_equal(readers[i].acquire_closed, SALTS_SERVICE_HOST_INVALID_STATE);
        }
        check_equal(salts_service_host_generation_drain(&runtime, previous),
                    SALTS_SERVICE_HOST_OK);
        check_null(previous->storage.deployments);
        check_null(previous->storage.instances);
        check_null(previous->storage.modules);
        check_null(previous->components.deployments);
        check_equal(previous->components.state,
                    SALTS_CONFIGURATOR_CONTEXT_ZERO);
        check_equal(cmeta_plugin_registry_get_lifecycle(&registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
        check_equal(runtime.active_scopes, (size_t)0u);
        check_equal(runtime.attached_generations, (size_t)0u);
    }

    it("admits one complete generation when multiple readers race publication") {
        for (size_t round = 0; round < PUBLICATION_RACE_ROUNDS; ++round) {
            salts_service_host_generation *previous = NULL;
            const uint64_t old_id = (uint64_t)round * 2u + 1u;
            gate.entered = gate.advanced = 0u;
            gate.phase = 0u;
            for (size_t i = 0; i < 2u; ++i)
                check_equal(build_generation(&generations[i], old_id + i, &registry, ref),
                            SALTS_SERVICE_HOST_OK);
            check_equal(salts_service_host_runtime_publish(
                &runtime, &generations[0].generation, &previous), SALTS_SERVICE_HOST_OK);
            for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
                readers[i] = (publication_reader){0};
                readers[i].gate = &gate;
                readers[i].runtime = &runtime;
                readers[i].race_publish = true;
                check_equal(cmeta_thread_create(&threads[i], publication_worker, &readers[i]), 0);
            }
            check_equal(gate_wait(&gate, &gate.entered), 0);
            gate_advance(&gate, 1u);
            check_equal(salts_service_host_runtime_publish(
                &runtime, &generations[1].generation, &previous), SALTS_SERVICE_HOST_OK);
            check_equal(gate_wait(&gate, &gate.advanced), 0);
            check_equal(salts_service_host_runtime_close(&runtime, &previous),
                        SALTS_SERVICE_HOST_OK);
            gate_advance(&gate, 2u);
            for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
                check_equal(cmeta_thread_join(&threads[i]), 0);
                check_equal(readers[i].acquire_old, SALTS_SERVICE_HOST_OK);
                check_true(readers[i].old_id == old_id || readers[i].old_id == old_id + 1u);
                check_equal(readers[i].old_value, COMPONENT_PROVIDER_VALUE);
                check_equal(readers[i].acquire_new, SALTS_SERVICE_HOST_OK);
                check_true(readers[i].new_id == old_id || readers[i].new_id == old_id + 1u);
                check_equal(readers[i].new_value, COMPONENT_PROVIDER_VALUE);
                check_equal(readers[i].release_old, SALTS_SERVICE_HOST_OK);
                check_equal(readers[i].release_new, SALTS_SERVICE_HOST_OK);
                check_equal(readers[i].acquire_closed, SALTS_SERVICE_HOST_INVALID_STATE);
            }
            for (size_t i = 0; i < 2u; ++i)
                check_equal(salts_service_host_generation_drain(
                    &runtime, &generations[i].generation), SALTS_SERVICE_HOST_OK);
            check_equal(runtime.active_scopes, (size_t)0u);
            check_equal(runtime.attached_generations, (size_t)0u);
        }
    }

    it("allows only one concurrent drainer to release the generation lease") {
        salts_service_host_generation *previous = NULL;
        cmeta_plugin_lifecycle_info info;
        size_t successful_drains = 0u;
        check_equal(build_generation(&generations[0], UINT64_C(1), &registry, ref),
                    SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_publish(
            &runtime, &generations[0].generation, &previous), SALTS_SERVICE_HOST_OK);
        check_equal(salts_service_host_runtime_close(&runtime, &previous),
                    SALTS_SERVICE_HOST_OK);
        for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
            drainers[i] = (publication_drainer){&gate, &runtime, previous,
                                               SALTS_SERVICE_HOST_BUSY};
            check_equal(cmeta_thread_create(&threads[i], drain_worker, &drainers[i]), 0);
        }
        check_equal(gate_wait(&gate, &gate.entered), 0);
        gate_advance(&gate, 1u);
        for (size_t i = 0; i < PUBLICATION_READERS; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), 0);
            if (drainers[i].status == SALTS_SERVICE_HOST_OK)
                ++successful_drains;
            else
                check_equal(drainers[i].status, SALTS_SERVICE_HOST_INVALID_STATE);
        }
        check_equal(successful_drains, (size_t)1u);
        check_equal(cmeta_plugin_registry_get_lifecycle(&registry, ref, &info), CMETA_PLUGIN_OK);
        check_equal(info.active_leases, (size_t)0u);
        check_equal(runtime.attached_generations, (size_t)0u);
        check_equal(generations[0].generation.state, SALTS_SERVICE_HOST_GENERATION_DRAINED);
        check_null(generations[0].generation.runtime_owner);
        check_null(generations[0].generation.registry);
    }
}
