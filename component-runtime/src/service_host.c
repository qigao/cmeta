/* Canonical Salts ServiceHost: single Plugin generation/Scope owner. */
#include <salts/service_host.h>
#include <salts/thread.h>

#include <string.h>

static salts_service_host_failure service_host_failure_none(void) {
    salts_service_host_failure failure;
    failure.source_index = SALTS_SERVICE_HOST_INDEX_NONE;
    failure.plugin_status = CMETA_PLUGIN_OK;
    failure.component_status = SALTS_COMPONENT_OK;
    return failure;
}

static bool plugin_ref_equal(cmeta_plugin_ref a, cmeta_plugin_ref b) {
    return a.slot == b.slot && a.generation == b.generation;
}

static salts_service_host_status generation_fail_plugin(
    salts_service_host_generation *generation,
    size_t source_index,
    cmeta_plugin_status status) {
    generation->failure.source_index = source_index;
    generation->failure.plugin_status = status;
    generation->state = SALTS_SERVICE_HOST_GENERATION_FAILED;
    return SALTS_SERVICE_HOST_PLUGIN_ERROR;
}

static salts_service_host_status generation_fail_component(
    salts_service_host_generation *generation,
    salts_component_status status) {
    generation->failure.component_status = status;
    generation->state = SALTS_SERVICE_HOST_GENERATION_FAILED;
    return SALTS_SERVICE_HOST_COMPONENT_ERROR;
}

static salts_service_host_status generation_fail_provider(
    salts_service_host_generation *generation,
    size_t source_index) {
    generation->failure.source_index = source_index;
    generation->state = SALTS_SERVICE_HOST_GENERATION_FAILED;
    return SALTS_SERVICE_HOST_PROVIDER_ERROR;
}

static bool plugin_lease_is_zero(cmeta_plugin_lease lease) {
    return lease.plugin.slot == 0u &&
           lease.plugin.generation == 0u &&
           lease.slot == 0u &&
           lease.generation == 0u;
}

static bool module_tail_is_clear(
    const salts_service_host_generation *generation) {
    size_t i;

    if (generation == NULL ||
        generation->module_count > generation->storage.module_capacity)
        return false;
    if (generation->storage.module_capacity == 0u)
        return generation->module_count == 0u;
    if (generation->storage.modules == NULL)
        return false;

    for (i = generation->module_count;
         i < generation->storage.module_capacity; ++i) {
        const salts_service_host_module *module =
            &generation->storage.modules[i];
        if (!plugin_lease_is_zero(module->lease) ||
            module->plugin.slot != 0u ||
            module->plugin.generation != 0u ||
            module->manifest != NULL)
            return false;
    }
    return true;
}

static bool module_records_are_clear(
    const salts_service_host_generation *generation) {
    return generation != NULL &&
           generation->module_count == 0u &&
           module_tail_is_clear(generation);
}

static cmeta_plugin_status release_modules(
    salts_service_host_generation *generation) {
    cmeta_plugin_status first = CMETA_PLUGIN_OK;
    size_t i;

    if (generation == NULL)
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    if (generation->module_count > generation->storage.module_capacity ||
        (generation->module_count != 0u &&
         generation->storage.modules == NULL))
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    /* Any entry outside the recorded prefix must be empty; otherwise a
     * smaller/corrupted module_count could hide a live lease in the tail. */
    if (!module_tail_is_clear(generation))
        return CMETA_PLUGIN_INVALID_ARGUMENT;
    if (generation->module_count == 0u &&
        !module_records_are_clear(generation))
        return CMETA_PLUGIN_INVALID_ARGUMENT;

    i = generation->module_count;
    while (i != 0u) {
        salts_service_host_module *module;
        cmeta_plugin_lease lease;
        cmeta_plugin_status status;
        --i;
        module = &generation->storage.modules[i];
        lease = module->lease;

        if (plugin_lease_is_zero(lease)) {
            /* Zero means consumed only when the accompanying ownership record
             * has also been cleared after a successful Registry release. A
             * zero/malformed token with a retained module record is ambiguous:
             * fail closed and never drop the generation's last owner record. */
            if (module->plugin.slot != 0u ||
                module->plugin.generation != 0u ||
                module->manifest != NULL) {
                if (first == CMETA_PLUGIN_OK)
                    first = CMETA_PLUGIN_INVALID_ARGUMENT;
            }
            continue;
        }

        if (!cmeta_plugin_lease_valid(lease) ||
            !plugin_ref_equal(module->plugin, lease.plugin) ||
            module->manifest == NULL) {
            if (first == CMETA_PLUGIN_OK)
                first = CMETA_PLUGIN_INVALID_ARGUMENT;
            continue;
        }

        status = cmeta_plugin_registry_release(
            generation->registry, &module->lease);
        if (first == CMETA_PLUGIN_OK && status != CMETA_PLUGIN_OK)
            first = status;
        if (status == CMETA_PLUGIN_OK) {
            module->manifest = NULL;
            module->plugin = (cmeta_plugin_ref){0u, 0u};
        }
    }

    if (first == CMETA_PLUGIN_OK)
        generation->module_count = 0u;
    return first;
}

static salts_service_host_module *find_module(
    salts_service_host_generation *generation,
    cmeta_plugin_ref ref) {
    size_t i;
    for (i = 0u; i < generation->module_count; ++i)
        if (plugin_ref_equal(generation->storage.modules[i].plugin, ref))
            return &generation->storage.modules[i];
    return NULL;
}

static salts_service_host_status preflight_dynamic_sources(
    const salts_service_host_source *sources,
    size_t source_count,
    size_t module_capacity,
    size_t *out_distinct_modules,
    size_t *out_invalid_source) {
    size_t distinct = 0u;
    size_t i;

    if (out_distinct_modules == NULL || out_invalid_source == NULL)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    *out_distinct_modules = 0u;
    *out_invalid_source = SALTS_SERVICE_HOST_INDEX_NONE;

    for (i = 0u; i < source_count; ++i) {
        size_t j;
        bool seen = false;

        if (!cmeta_plugin_ref_valid(sources[i].plugin) ||
            sources[i].export_id == NULL ||
            sources[i].export_id[0] == '\0') {
            *out_invalid_source = i;
            return SALTS_SERVICE_HOST_PROVIDER_ERROR;
        }

        for (j = 0u; j < i; ++j) {
            if (plugin_ref_equal(sources[i].plugin, sources[j].plugin)) {
                seen = true;
                break;
            }
        }

        if (!seen) {
            if (distinct == SIZE_MAX)
                return SALTS_SERVICE_HOST_CAPACITY_EXCEEDED;
            ++distinct;
        }
    }

    if (distinct > module_capacity)
        return SALTS_SERVICE_HOST_CAPACITY_EXCEEDED;

    *out_distinct_modules = distinct;
    return SALTS_SERVICE_HOST_OK;
}

static salts_service_host_status acquire_module(
    salts_service_host_generation *generation,
    size_t source_index,
    cmeta_plugin_ref ref,
    salts_service_host_module **out_module) {
    salts_service_host_module *module;
    cmeta_plugin_status status;

    module = find_module(generation, ref);
    if (module != NULL) {
        *out_module = module;
        return SALTS_SERVICE_HOST_OK;
    }

    if (generation->module_count >= generation->storage.module_capacity)
        return SALTS_SERVICE_HOST_CAPACITY_EXCEEDED;

    module = &generation->storage.modules[generation->module_count];
    memset(module, 0, sizeof(*module));
    module->plugin = ref;

    status = cmeta_plugin_registry_acquire(
        generation->registry, ref, &module->lease, &module->manifest);
    if (status != CMETA_PLUGIN_OK) {
        memset(module, 0, sizeof(*module));
        return generation_fail_plugin(generation, source_index, status);
    }

    ++generation->module_count;
    *out_module = module;
    return SALTS_SERVICE_HOST_OK;
}

/* Dynamic Provider admission has stricter native ABI requirements than a
 * local static Configurator graph. This runs while the Plugin lease is live,
 * before any create/activate callback and before publication. */
static bool service_host_dynamic_component_abi_valid(
    const cmeta_component_desc *component) {
    size_t i;
    if (!cmeta_component_desc_valid(component))
        return false;
    for (i = 0u; i < component->capability_count; ++i)
        if (!cmeta_interface_borrowed_service_abi_valid(
                component->capabilities[i].interface_desc))
            return false;
    return true;
}

static salts_service_host_status admit_dynamic_source(
    salts_service_host_generation *generation,
    size_t source_index,
    const salts_service_host_source *source,
    salts_configurator_deployment *out_deployment) {
    salts_service_host_module *module = NULL;
    const cmeta_plugin_export *entry = NULL;
    salts_component_provider *provider;
    const salts_component_provider_binding *binding;
    cmeta_plugin_status plugin_status;
    salts_service_host_status status;

    if (!cmeta_plugin_ref_valid(source->plugin) ||
        source->export_id == NULL || source->export_id[0] == '\0')
        return generation_fail_provider(generation, source_index);

    status = acquire_module(
        generation, source_index, source->plugin, &module);
    if (status != SALTS_SERVICE_HOST_OK)
        return status;

    plugin_status = cmeta_plugin_manifest_find_export(
        module->manifest, source->export_id, &entry);
    if (plugin_status != CMETA_PLUGIN_OK)
        return generation_fail_plugin(
            generation, source_index, plugin_status);

    plugin_status = cmeta_plugin_export_require_interface(
        entry,
        SALTS_COMPONENT_PROVIDER_CONTRACT_ID,
        SALTS_COMPONENT_PROVIDER_CONTRACT_VERSION,
        0u,
        salts_component_provider_interface());
    if (plugin_status != CMETA_PLUGIN_OK)
        return generation_fail_plugin(
            generation, source_index, plugin_status);

    provider =
        (salts_component_provider *)entry->value.interface.value;
    if (!salts_component_provider_valid(provider))
        return generation_fail_provider(generation, source_index);

    binding = salts_component_provider_get_binding(provider);
    if (!salts_component_provider_binding_valid(binding))
        return generation_fail_provider(generation, source_index);
    if (!service_host_dynamic_component_abi_valid(binding->component))
        return generation_fail_provider(generation, source_index);

    out_deployment->provider = binding;
    out_deployment->config_data = source->config_data;
    out_deployment->config_value = source->config_value;
    return SALTS_SERVICE_HOST_OK;
}

static void generation_clear(
    salts_service_host_generation *generation) {
    if (generation == NULL)
        return;
    generation->registry = NULL;
    generation->deployment_count = 0u;
    generation->module_count = 0u;
    /* A DRAINED generation no longer owns or borrows any graph/Provider
     * storage. Drop its stale addresses so callers may reclaim their bundles
     * as soon as cleanup returns successfully. */
    generation->storage =
        (salts_service_host_generation_storage){0};
    generation->components =
        (salts_configurator_context)SALTS_CONFIGURATOR_CONTEXT_INIT;
}

/* ZERO and DRAINED are reusable only if every owner/borrrow field has reached
 * the canonical empty representation. The state enum alone is not enough to
 * authorize overwriting a public generation record with unsettled ownership. */
static bool generation_is_clear(
    const salts_service_host_generation *generation) {
    const salts_service_host_generation_storage *storage;
    const salts_configurator_context *components;

    if (generation == NULL || generation->registry != NULL ||
        generation->deployment_count != 0u || generation->module_count != 0u ||
        generation->runtime_owner != NULL || generation->active_scopes != 0u)
        return false;

    storage = &generation->storage;
    if (storage->deployments != NULL || storage->deployment_capacity != 0u ||
        storage->instances != NULL || storage->instance_capacity != 0u ||
        storage->dependencies != NULL || storage->dependency_capacity != 0u ||
        storage->activation_order != NULL || storage->activation_capacity != 0u ||
        storage->modules != NULL || storage->module_capacity != 0u)
        return false;

    components = &generation->components;
    return components->state == SALTS_CONFIGURATOR_CONTEXT_ZERO &&
           components->deployment_count == 0u &&
           components->selection_count == 0u &&
           components->instance_capacity == 0u &&
           components->dependency_capacity == 0u &&
           components->activation_capacity == 0u &&
           components->dependency_count == 0u &&
           components->activation_count == 0u &&
           components->deployments == NULL &&
           components->selections == NULL &&
           components->instances == NULL &&
           components->dependencies == NULL &&
           components->activation_order == NULL;
}

const char *salts_service_host_status_string(
    salts_service_host_status status) {
    switch (status) {
    case SALTS_SERVICE_HOST_OK: return "ok";
    case SALTS_SERVICE_HOST_INVALID_ARGUMENT: return "invalid argument";
    case SALTS_SERVICE_HOST_CAPACITY_EXCEEDED: return "capacity exceeded";
    case SALTS_SERVICE_HOST_PLUGIN_ERROR: return "plugin error";
    case SALTS_SERVICE_HOST_PROVIDER_ERROR: return "provider error";
    case SALTS_SERVICE_HOST_COMPONENT_ERROR: return "component error";
    case SALTS_SERVICE_HOST_BUSY: return "busy";
    case SALTS_SERVICE_HOST_INVALID_STATE: return "invalid state";
    case SALTS_SERVICE_HOST_RESOURCE_ERROR: return "resource error";
    }
    return "unknown ServiceHost status";
}

salts_service_host_status salts_service_host_generation_build(
    salts_service_host_generation *generation,
    uint64_t generation_id,
    cmeta_plugin_registry *registry,
    const salts_service_host_generation_storage *storage,
    const salts_configurator_deployment *static_deployments,
    size_t static_deployment_count,
    const salts_service_host_source *dynamic_sources,
    size_t dynamic_source_count,
    const salts_configurator_selection *selections,
    size_t selection_count) {
    size_t total;
    size_t distinct_modules = 0u;
    size_t invalid_source = SALTS_SERVICE_HOST_INDEX_NONE;
    size_t i;
    salts_component_status component_status;
    salts_service_host_status preflight_status;

    if (generation == NULL || generation_id == 0u ||
        storage == NULL ||
        (static_deployment_count != 0u && static_deployments == NULL) ||
        (dynamic_source_count != 0u &&
         (dynamic_sources == NULL || registry == NULL)) ||
        (selection_count != 0u && selections == NULL))
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    if (generation->state == SALTS_SERVICE_HOST_GENERATION_ZERO ||
        generation->state == SALTS_SERVICE_HOST_GENERATION_DRAINED) {
        if (!generation_is_clear(generation))
            return SALTS_SERVICE_HOST_INVALID_STATE;
    } else if (generation->state == SALTS_SERVICE_HOST_GENERATION_FAILED) {
        if (generation->runtime_owner != NULL ||
            generation->active_scopes != 0u ||
            generation->module_count != 0u ||
            !module_records_are_clear(generation) ||
            generation->components.activation_count != 0u ||
            (generation->components.state != SALTS_CONFIGURATOR_CONTEXT_ZERO &&
             generation->components.state != SALTS_CONFIGURATOR_CONTEXT_FAILED &&
             generation->components.state != SALTS_CONFIGURATOR_CONTEXT_STOPPED))
            return SALTS_SERVICE_HOST_INVALID_STATE;
    } else {
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }

    if (static_deployment_count > SIZE_MAX - dynamic_source_count)
        return SALTS_SERVICE_HOST_CAPACITY_EXCEEDED;
    total = static_deployment_count + dynamic_source_count;

    if (total > storage->deployment_capacity ||
        total > storage->instance_capacity ||
        total > storage->activation_capacity ||
        (total != 0u &&
         (storage->deployments == NULL || storage->instances == NULL ||
          storage->activation_order == NULL)) ||
        (storage->dependency_capacity != 0u && storage->dependencies == NULL) ||
        (storage->module_capacity != 0u && storage->modules == NULL))
        return SALTS_SERVICE_HOST_CAPACITY_EXCEEDED;

    preflight_status = preflight_dynamic_sources(
        dynamic_sources,
        dynamic_source_count,
        storage->module_capacity,
        &distinct_modules,
        &invalid_source);
    if (preflight_status != SALTS_SERVICE_HOST_OK) {
        if (preflight_status == SALTS_SERVICE_HOST_PROVIDER_ERROR) {
            generation->failure = service_host_failure_none();
            generation->failure.source_index = invalid_source;
            generation->state = SALTS_SERVICE_HOST_GENERATION_FAILED;
        }
        return preflight_status;
    }
    (void)distinct_modules;

    /* A candidate owns this context exclusively, never while ACTIVE.
     * Establish defined typed ZERO state before static Context admission.
     * The whole module table is caller-owned scratch storage: initialize every
     * slot before any failure path can ask release_modules() to distinguish
     * canonical empty records from unsettled Plugin ownership. */
    for (i = 0u; i < storage->module_capacity; ++i)
        memset(&storage->modules[i], 0, sizeof(storage->modules[i]));

    generation->components = (salts_configurator_context)SALTS_CONFIGURATOR_CONTEXT_INIT;
    generation->id = generation_id;
    generation->registry = registry;
    generation->storage = *storage;
    generation->deployment_count = total;
    generation->module_count = 0u;
    generation->runtime_owner = NULL;
    generation->active_scopes = 0u;
    generation->failure = service_host_failure_none();
    generation->state = SALTS_SERVICE_HOST_GENERATION_ZERO;

    /* Establish a canonical empty tail before the first lease can be
     * acquired. Later cleanup can then detect orphaned records if the public
     * module_count metadata ever disagrees with the caller-owned table. */
    for (i = 0u; i < storage->module_capacity; ++i)
        memset(&storage->modules[i], 0, sizeof(storage->modules[i]));

    for (i = 0u; i < static_deployment_count; ++i)
        storage->deployments[i] = static_deployments[i];

    for (i = 0u; i < dynamic_source_count; ++i) {
        salts_service_host_status status = admit_dynamic_source(
            generation, i, &dynamic_sources[i],
            &storage->deployments[static_deployment_count + i]);
        if (status != SALTS_SERVICE_HOST_OK) {
            cmeta_plugin_status release_status = release_modules(generation);
            if (generation->failure.plugin_status == CMETA_PLUGIN_OK &&
                release_status != CMETA_PLUGIN_OK)
                generation->failure.plugin_status = release_status;
            return status;
        }
    }

    component_status = salts_configurator_context_init(
        &generation->components,
        storage->deployments, total,
        selections, selection_count,
        storage->instances, storage->instance_capacity,
        storage->dependencies, storage->dependency_capacity,
        storage->activation_order, storage->activation_capacity);
    if (component_status == SALTS_COMPONENT_OK)
        component_status =
            salts_configurator_context_resolve(&generation->components);
    if (component_status == SALTS_COMPONENT_OK)
        component_status =
            salts_configurator_context_start(&generation->components);

    if (component_status != SALTS_COMPONENT_OK) {
        cmeta_plugin_status release_status;
        generation_fail_component(generation, component_status);
        release_status = release_modules(generation);
        if (release_status != CMETA_PLUGIN_OK)
            generation->failure.plugin_status = release_status;
        return SALTS_SERVICE_HOST_COMPONENT_ERROR;
    }

    generation->state = SALTS_SERVICE_HOST_GENERATION_BUILT;
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_generation_discard(
    salts_service_host_generation *generation) {
    cmeta_plugin_status plugin_status;

    if (generation == NULL)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (generation->state != SALTS_SERVICE_HOST_GENERATION_BUILT)
        return SALTS_SERVICE_HOST_INVALID_STATE;

    if (salts_configurator_context_stop(&generation->components) !=
        SALTS_COMPONENT_OK)
        return generation_fail_component(
            generation, SALTS_COMPONENT_INVALID_STATE);

    plugin_status = release_modules(generation);
    if (plugin_status != CMETA_PLUGIN_OK)
        return generation_fail_plugin(
            generation, SALTS_SERVICE_HOST_INDEX_NONE, plugin_status);

    generation->state = SALTS_SERVICE_HOST_GENERATION_DRAINED;
    generation_clear(generation);
    generation->state = SALTS_SERVICE_HOST_GENERATION_DRAINED;
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_generation_recover(
    salts_service_host_generation *generation) {
    cmeta_plugin_status plugin_status;
    salts_configurator_context_state component_state;

    if (generation == NULL)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (generation->state != SALTS_SERVICE_HOST_GENERATION_FAILED ||
        generation->runtime_owner != NULL ||
        generation->active_scopes != 0u ||
        (generation->module_count != 0u &&
         generation->storage.modules == NULL))
        return SALTS_SERVICE_HOST_INVALID_STATE;

    component_state = generation->components.state;
    if ((component_state != SALTS_CONFIGURATOR_CONTEXT_ZERO &&
         component_state != SALTS_CONFIGURATOR_CONTEXT_FAILED &&
         component_state != SALTS_CONFIGURATOR_CONTEXT_STOPPED) ||
        generation->components.activation_count != 0u)
        return SALTS_SERVICE_HOST_INVALID_STATE;

    /* A failure must not discard an unresolved token. Plugin release consumes
     * successful leases exactly once and leaves failed tokens unchanged.
     * release_modules preserves the module table until all remaining tokens
     * settle, so the caller can repair the ownership fault and retry. */
    plugin_status = release_modules(generation);
    if (plugin_status != CMETA_PLUGIN_OK) {
        generation->failure.plugin_status = plugin_status;
        return SALTS_SERVICE_HOST_PLUGIN_ERROR;
    }

    generation_clear(generation);
    generation->components =
        (salts_configurator_context)SALTS_CONFIGURATOR_CONTEXT_INIT;
    generation->runtime_owner = NULL;
    generation->active_scopes = 0u;
    generation->state = SALTS_SERVICE_HOST_GENERATION_DRAINED;
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_runtime_publish(
    salts_service_host_runtime *runtime,
    salts_service_host_generation *generation,
    salts_service_host_generation **out_previous) {
    salts_service_host_generation *previous;

    if (runtime == NULL || generation == NULL || out_previous == NULL ||
        !runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);

    if (generation->state != SALTS_SERVICE_HOST_GENERATION_BUILT ||
        generation->runtime_owner != NULL ||
        generation->active_scopes != 0u ||
        generation->id <= runtime->last_generation_id) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }

    if (runtime->attached_generations >=
        SALTS_SERVICE_HOST_MAX_ATTACHED_GENERATIONS) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_BUSY;
    }

    previous = runtime->current;
    if (previous != NULL &&
        previous->state != SALTS_SERVICE_HOST_GENERATION_PUBLISHED) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }

    generation->runtime_owner = runtime;
    generation->state = SALTS_SERVICE_HOST_GENERATION_PUBLISHED;
    ++runtime->attached_generations;

    runtime->current = generation;
    runtime->last_generation_id = generation->id;

    if (previous != NULL)
        previous->state = SALTS_SERVICE_HOST_GENERATION_DRAINING;

    *out_previous = previous;
    cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_runtime_close(
    salts_service_host_runtime *runtime,
    salts_service_host_generation **out_previous) {
    salts_service_host_generation *previous;

    if (runtime == NULL || out_previous == NULL || !runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);
    previous = runtime->current;
    if (previous == NULL) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }
    if (previous->state != SALTS_SERVICE_HOST_GENERATION_PUBLISHED) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }

    runtime->current = NULL;
    previous->state = SALTS_SERVICE_HOST_GENERATION_DRAINING;
    *out_previous = previous;

    cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_scope_acquire(
    salts_service_host_runtime *runtime,
    salts_service_host_scope *scope) {
    salts_service_host_generation *generation;

    if (runtime == NULL || scope == NULL || !runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (scope->live)
        return SALTS_SERVICE_HOST_INVALID_STATE;

    cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);
    generation = runtime->current;
    if (generation == NULL ||
        generation->state != SALTS_SERVICE_HOST_GENERATION_PUBLISHED) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }
    if (generation->active_scopes == SIZE_MAX ||
        runtime->active_scopes == SIZE_MAX) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_CAPACITY_EXCEEDED;
    }

    ++generation->active_scopes;
    ++runtime->active_scopes;

    scope->runtime = runtime;
    scope->generation = generation;
    scope->generation_id = generation->id;
    scope->owner_address = scope;
    scope->live = true;

    cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
    return SALTS_SERVICE_HOST_OK;
}

salts_service_host_status salts_service_host_scope_release(
    salts_service_host_scope *scope) {
    salts_service_host_runtime *runtime;
    salts_service_host_generation *generation;

    if (scope == NULL || !scope->live)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (scope->owner_address != scope)
        return SALTS_SERVICE_HOST_INVALID_STATE;
    if (scope->runtime == NULL || scope->generation == NULL)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    runtime = scope->runtime;
    generation = scope->generation;

    if (!runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_STATE;

    cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);
    if (generation->runtime_owner != runtime ||
        generation->id != scope->generation_id ||
        generation->active_scopes == 0u ||
        runtime->active_scopes == 0u) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }

    --generation->active_scopes;
    --runtime->active_scopes;
    cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);

    *scope = (salts_service_host_scope)SALTS_SERVICE_HOST_SCOPE_INIT;
    return SALTS_SERVICE_HOST_OK;
}

uint64_t salts_service_host_scope_generation_id(
    const salts_service_host_scope *scope) {
    return scope != NULL && scope->live && scope->owner_address == scope
        ? scope->generation_id : UINT64_C(0);
}

salts_service_host_status salts_service_host_scope_find_service(
    const salts_service_host_scope *scope,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service) {
    salts_component_status status;

    if (scope == NULL || !scope->live)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (scope->owner_address != scope)
        return SALTS_SERVICE_HOST_INVALID_STATE;
    if (scope->generation == NULL)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    status = salts_configurator_context_find_service(
        &scope->generation->components, interface_desc, out_service);
    return status == SALTS_COMPONENT_OK
        ? SALTS_SERVICE_HOST_OK
        : SALTS_SERVICE_HOST_COMPONENT_ERROR;
}

salts_service_host_status salts_service_host_scope_find_service_from(
    const salts_service_host_scope *scope,
    const char *provider_component_id,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service) {
    salts_component_status status;

    if (scope == NULL || !scope->live)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;
    if (scope->owner_address != scope)
        return SALTS_SERVICE_HOST_INVALID_STATE;
    if (scope->generation == NULL)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    status = salts_configurator_context_find_service_from(
        &scope->generation->components,
        provider_component_id,
        interface_desc,
        out_service);
    return status == SALTS_COMPONENT_OK
        ? SALTS_SERVICE_HOST_OK
        : SALTS_SERVICE_HOST_COMPONENT_ERROR;
}

salts_service_host_status salts_service_host_generation_drain(
    salts_service_host_runtime *runtime,
    salts_service_host_generation *generation) {
    salts_component_status component_status = SALTS_COMPONENT_OK;
    cmeta_plugin_status plugin_status;

    if (runtime == NULL || generation == NULL || !runtime->initialized)
        return SALTS_SERVICE_HOST_INVALID_ARGUMENT;

    cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);

    if (generation->runtime_owner != runtime ||
        runtime->attached_generations == 0u ||
        (generation->state != SALTS_SERVICE_HOST_GENERATION_DRAINING &&
         !(generation->state == SALTS_SERVICE_HOST_GENERATION_FAILED &&
           generation->components.state == SALTS_CONFIGURATOR_CONTEXT_STOPPED))) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_INVALID_STATE;
    }

    if (generation->active_scopes != 0u) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_BUSY;
    }

    generation->state = SALTS_SERVICE_HOST_GENERATION_STOPPING;
    cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);

    if (generation->components.state == SALTS_CONFIGURATOR_CONTEXT_ACTIVE)
        component_status =
            salts_configurator_context_stop(&generation->components);

    if (component_status != SALTS_COMPONENT_OK) {
        generation->failure.component_status = component_status;
        cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);
        generation->state = SALTS_SERVICE_HOST_GENERATION_FAILED;
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_COMPONENT_ERROR;
    }

    plugin_status = release_modules(generation);
    if (plugin_status != CMETA_PLUGIN_OK) {
        generation->failure.plugin_status = plugin_status;
        cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);
        generation->state = SALTS_SERVICE_HOST_GENERATION_FAILED;
        cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);
        return SALTS_SERVICE_HOST_PLUGIN_ERROR;
    }

    cmeta_mutex_lock((cmeta_mutex_t *)&runtime->lock);
    generation_clear(generation);
    generation->state = SALTS_SERVICE_HOST_GENERATION_DRAINED;
    generation->runtime_owner = NULL;
    --runtime->attached_generations;
    cmeta_mutex_unlock((cmeta_mutex_t *)&runtime->lock);

    return SALTS_SERVICE_HOST_OK;
}

const salts_service_host_failure *salts_service_host_generation_failure(
    const salts_service_host_generation *generation) {
    return generation == NULL ? NULL : &generation->failure;
}
