/* Canonical Salts Configurator: single bounded static graph/lifecycle owner. */
#include <cmeta/configurator.h>

#include <string.h>

static salts_configurator_failure salts_configurator_failure_none(void) {
    salts_configurator_failure failure;
    failure.phase = SALTS_CONFIGURATOR_PHASE_NONE;
    failure.component_index = SALTS_CONFIGURATOR_INDEX_NONE;
    failure.dependency_index = SALTS_CONFIGURATOR_INDEX_NONE;
    failure.provider_status = CMETA_OK;
    return failure;
}

static bool salts_configurator_instances_settled(
    const salts_configurator_context *context);

static void salts_configurator_context_detach_storage(
    salts_configurator_context *context) {
    if (context == NULL)
        return;
    context->deployments = NULL;
    context->deployment_count = 0u;
    context->selections = NULL;
    context->selection_count = 0u;
    context->instances = NULL;
    context->instance_capacity = 0u;
    context->dependencies = NULL;
    context->dependency_capacity = 0u;
    context->dependency_count = 0u;
    context->activation_order = NULL;
    context->activation_capacity = 0u;
    context->activation_count = 0u;
}

static salts_configurator_status salts_configurator_fail(
    salts_configurator_context *context,
    salts_configurator_status status,
    salts_configurator_phase phase,
    size_t component_index,
    size_t dependency_index,
    cmeta_status provider_status) {
    if (context != NULL) {
        context->failure.phase = phase;
        context->failure.component_index = component_index;
        context->failure.dependency_index = dependency_index;
        context->failure.provider_status = provider_status;
        context->state = SALTS_CONFIGURATOR_CONTEXT_FAILED;

        /* Init and Resolve cannot leave a live ObjectRef. Start failures reach
         * this helper only after the failing instance is released and rollback
         * is complete. Detach borrowed arrays only if that invariant holds;
         * retain their addresses if teardown did not settle, so reuse is
         * rejected rather than losing the remaining owner authority. */
        if (phase == SALTS_CONFIGURATOR_PHASE_INIT ||
            phase == SALTS_CONFIGURATOR_PHASE_RESOLVE ||
            salts_configurator_instances_settled(context))
            salts_configurator_context_detach_storage(context);
    }
    return status;
}

static bool salts_configurator_provides(
    const cmeta_component_desc *component,
    const cmeta_interface_desc *expected) {
    size_t i;
    if (!cmeta_component_desc_valid(component) ||
        !cmeta_interface_desc_valid(expected))
        return false;
    for (i = 0u; i < component->capability_count; ++i) {
        const cmeta_component_capability *row = &component->capabilities[i];
        if (row->role == CMETA_COMPONENT_PROVIDES &&
            cmeta_interface_desc_equal(row->interface_desc, expected))
            return true;
    }
    return false;
}

static size_t salts_configurator_requirement_count(
    const cmeta_component_desc *component) {
    size_t count = 0u;
    size_t i;
    for (i = 0u; i < component->capability_count; ++i)
        if (component->capabilities[i].role == CMETA_COMPONENT_REQUIRES)
            ++count;
    return count;
}

static salts_configurator_status salts_configurator_deployment_status(
    const salts_configurator_deployment *deployment) {
    const cmeta_component_desc *component;
    if (deployment == NULL || deployment->provider == NULL)
        return SALTS_CONFIGURATOR_INVALID_COMPONENT;

    if (!salts_configurator_provider_binding_valid(deployment->provider))
        return SALTS_CONFIGURATOR_INVALID_COMPONENT;

    component = deployment->provider->component;
    if (component->config == NULL) {
        if (deployment->config_data != NULL || deployment->config_value != NULL)
            return SALTS_CONFIGURATOR_CONFIG_MISMATCH;
    } else {
        if (deployment->config_data == NULL ||
            deployment->config_value == NULL ||
            !cmeta_data_desc_valid(deployment->config_data) ||
            !cmeta_data_desc_equal(component->config, deployment->config_data))
            return SALTS_CONFIGURATOR_CONFIG_MISMATCH;
    }

    return SALTS_CONFIGURATOR_OK;
}

static const salts_configurator_provider_binding *salts_configurator_provider_at(
    const salts_configurator_context *context,
    size_t index) {
    if (context == NULL || index >= context->deployment_count ||
        context->deployments[index].provider == NULL)
        return NULL;
    return context->deployments[index].provider;
}

static void salts_configurator_instance_reset(salts_configurator_instance *instance) {
    if (instance == NULL)
        return;
    instance->object = (cmeta_object_ref)CMETA_OBJECT_REF_INIT;
    instance->dependency_offset = 0u;
    instance->dependency_count = 0u;
    instance->active = false;
}

/* A public state enum, activation_count, or failure phase is not by itself
 * proof that teardown authority is gone. A genuine INIT-phase failure has
 * already detached all borrowed arrays in salts_configurator_fail(); therefore
 * its settled representation is an empty prefix, not a phase-based bypass.
 * Verify every retained instance record before allowing FAILED/STOPPED reuse. */
static bool salts_configurator_object_ref_empty(
    const cmeta_object_ref *object) {
    return object != NULL &&
           object->size == sizeof(*object) &&
           object->object == NULL &&
           object->data == NULL &&
           object->field_provider == NULL &&
           object->operations == NULL &&
           object->operation_provider == NULL &&
           object->lifetime == CMETA_OBJECT_LIFETIME_NONE &&
           object->lifecycle == NULL;
}

static bool salts_configurator_storage_detached(
    const salts_configurator_context *context) {
    return context != NULL &&
           context->deployments == NULL &&
           context->deployment_count == 0u &&
           context->selections == NULL &&
           context->selection_count == 0u &&
           context->instances == NULL &&
           context->instance_capacity == 0u &&
           context->dependencies == NULL &&
           context->dependency_capacity == 0u &&
           context->dependency_count == 0u &&
           context->activation_order == NULL &&
           context->activation_capacity == 0u &&
           context->activation_count == 0u;
}

static bool salts_configurator_context_zero_is_clear(
    const salts_configurator_context *context) {
    return context != NULL &&
           context->state == SALTS_CONFIGURATOR_CONTEXT_ZERO &&
           salts_configurator_storage_detached(context);
}

static bool salts_configurator_instances_settled(
    const salts_configurator_context *context) {
    size_t i;

    if (context == NULL)
        return false;
    /* A genuine FAILED/STOPPED zero-deployment context has already detached
     * its borrowed arrays. Do not let deployment_count=0 hide retained
     * storage and owner records during reuse. */
    if ((context->state == SALTS_CONFIGURATOR_CONTEXT_FAILED ||
         context->state == SALTS_CONFIGURATOR_CONTEXT_STOPPED) &&
        context->deployment_count == 0u &&
        !salts_configurator_storage_detached(context))
        return false;
    if (context->deployment_count > context->instance_capacity ||
        (context->instance_capacity != 0u && context->instances == NULL))
        return false;

    /* Scan the whole backing store. deployment_count is caller-owned state
     * and truncating it must not hide a still-live tail ObjectRef. */
    for (i = 0u; i < context->instance_capacity; ++i) {
        const salts_configurator_instance *instance = &context->instances[i];
        if (instance->active ||
            !salts_configurator_object_ref_empty(&instance->object))
            return false;
    }
    return true;
}

static bool salts_configurator_order_contains(
    const salts_configurator_context *context,
    size_t prefix_count,
    size_t component_index) {
    size_t i;
    for (i = 0u; i < prefix_count; ++i)
        if (context->activation_order[i] == component_index)
            return true;
    return false;
}

static bool salts_configurator_requires(
    const cmeta_component_desc *component,
    const cmeta_interface_desc *expected) {
    size_t i;
    if (!cmeta_component_desc_valid(component) ||
        !cmeta_interface_desc_valid(expected))
        return false;
    for (i = 0u; i < component->capability_count; ++i) {
        const cmeta_component_capability *row = &component->capabilities[i];
        if (row->role == CMETA_COMPONENT_REQUIRES &&
            cmeta_interface_desc_equal(row->interface_desc, expected))
            return true;
    }
    return false;
}

static size_t salts_configurator_find_id(
    const salts_configurator_context *context,
    const char *stable_id) {
    size_t i;
    if (context == NULL || stable_id == NULL)
        return SALTS_CONFIGURATOR_INDEX_NONE;
    for (i = 0u; i < context->deployment_count; ++i)
        if (strcmp(salts_configurator_provider_at(context, i)->component->stable_id, stable_id) == 0)
            return i;
    return SALTS_CONFIGURATOR_INDEX_NONE;
}

static salts_configurator_status salts_configurator_select_provider(
    const salts_configurator_context *context,
    size_t consumer_index,
    const cmeta_interface_desc *requirement,
    size_t *out_provider) {
    const char *consumer_id;
    const salts_configurator_selection *selection = NULL;
    size_t selection_matches = 0u;
    size_t provider_candidate = SALTS_CONFIGURATOR_INDEX_NONE;
    size_t provider_matches = 0u;
    size_t i;

    if (context == NULL || consumer_index >= context->deployment_count ||
        !cmeta_interface_desc_valid(requirement) || out_provider == NULL)
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;

    consumer_id = salts_configurator_provider_at(context, consumer_index)->component->stable_id;

    for (i = 0u; i < context->selection_count; ++i) {
        const salts_configurator_selection *row = &context->selections[i];
        if (strcmp(row->consumer_component_id, consumer_id) == 0 &&
            cmeta_interface_desc_equal(row->requirement, requirement)) {
            selection = row;
            ++selection_matches;
        }
    }

    if (selection_matches > 1u)
        return SALTS_CONFIGURATOR_INVALID_SELECTION;

    if (selection_matches == 1u) {
        provider_candidate =
            salts_configurator_find_id(context, selection->provider_component_id);
        if (provider_candidate == SALTS_CONFIGURATOR_INDEX_NONE ||
            !salts_configurator_provides(
                salts_configurator_provider_at(context, provider_candidate)->component, requirement))
            return SALTS_CONFIGURATOR_INVALID_SELECTION;
        *out_provider = provider_candidate;
        return SALTS_CONFIGURATOR_OK;
    }

    for (i = 0u; i < context->deployment_count; ++i) {
        if (salts_configurator_provides(
                salts_configurator_provider_at(context, i)->component, requirement)) {
            provider_candidate = i;
            ++provider_matches;
        }
    }

    if (provider_matches == 0u)
        return SALTS_CONFIGURATOR_MISSING_PROVIDER;
    if (provider_matches != 1u)
        return SALTS_CONFIGURATOR_AMBIGUOUS_PROVIDER;

    *out_provider = provider_candidate;
    return SALTS_CONFIGURATOR_OK;
}

static bool salts_configurator_dependencies_ready(
    const salts_configurator_context *context,
    size_t component_index,
    size_t prefix_count) {
    const salts_configurator_instance *instance = &context->instances[component_index];
    size_t i;

    for (i = 0u; i < instance->dependency_count; ++i) {
        const salts_configurator_dependency *dependency =
            &context->dependencies[instance->dependency_offset + i];
        if (!salts_configurator_order_contains(
                context, prefix_count, dependency->provider_index))
            return false;
    }
    return true;
}

static cmeta_status salts_configurator_preflight_provides(
    const salts_configurator_provider_binding *binding,
    const cmeta_object_ref *instance) {
    size_t i;

    for (i = 0u; i < binding->component->capability_count; ++i) {
        const cmeta_component_capability *capability =
            &binding->component->capabilities[i];
        cmeta_interface_projection projection =
            CMETA_INTERFACE_PROJECTION_INIT;

        if (capability->role != CMETA_COMPONENT_PROVIDES)
            continue;

        {
            cmeta_status status = cmeta_object_interface_project_borrowed(
                instance, binding->interfaces,
                capability->interface_desc, &projection);
            if (status != CMETA_OK)
                return status;
        }
    }

    return CMETA_OK;
}

static bool salts_configurator_activation_plan_valid(
    const salts_configurator_context *context,
    bool expect_active) {
    size_t expected_dependency_offset = 0u;
    size_t i;
    size_t j;

    if (context == NULL ||
        context->deployment_count > context->instance_capacity ||
        context->deployment_count > context->activation_capacity ||
        context->activation_count != context->deployment_count ||
        context->dependency_count > context->dependency_capacity ||
        (context->instance_capacity != 0u && context->instances == NULL) ||
        (context->activation_capacity != 0u && context->activation_order == NULL) ||
        (context->deployment_count != 0u &&
         (context->deployments == NULL || context->instances == NULL ||
          context->activation_order == NULL)) ||
        (context->dependency_capacity != 0u && context->dependencies == NULL))
        return false;

    /* Validate all bindings, instance states and dependency slices before any
     * lifecycle callback or activation-order dereference occurs. The arrays
     * are caller-owned, so a mutable count/offset cannot be trusted alone. */
    for (i = 0u; i < context->deployment_count; ++i) {
        const salts_configurator_instance *instance = &context->instances[i];
        if (salts_configurator_deployment_status(&context->deployments[i]) !=
                SALTS_CONFIGURATOR_OK ||
            instance->dependency_offset != expected_dependency_offset ||
            instance->dependency_offset > context->dependency_count ||
            instance->dependency_count >
                context->dependency_count - instance->dependency_offset)
            return false;

        if (expect_active) {
            if (!instance->active || !cmeta_object_ref_valid(&instance->object))
                return false;
        } else if (instance->active ||
                   !salts_configurator_object_ref_empty(&instance->object)) {
            return false;
        }
        expected_dependency_offset += instance->dependency_count;
    }
    if (expected_dependency_offset != context->dependency_count)
        return false;

    for (i = context->deployment_count; i < context->instance_capacity; ++i) {
        const salts_configurator_instance *instance = &context->instances[i];
        if (instance->active ||
            !salts_configurator_object_ref_empty(&instance->object) ||
            instance->dependency_offset != 0u ||
            instance->dependency_count != 0u)
            return false;
    }

    for (i = 0u; i < context->activation_count; ++i) {
        const size_t component_index = context->activation_order[i];
        if (component_index >= context->deployment_count)
            return false;
        for (j = 0u; j < i; ++j)
            if (context->activation_order[j] == component_index)
                return false;
    }

    for (i = 0u; i < context->deployment_count; ++i) {
        const salts_configurator_instance *instance = &context->instances[i];
        const salts_configurator_provider_binding *consumer =
            salts_configurator_provider_at(context, i);
        size_t dependency_index;

        for (dependency_index = 0u;
             dependency_index < instance->dependency_count;
             ++dependency_index) {
            const salts_configurator_dependency *dependency =
                &context->dependencies[instance->dependency_offset + dependency_index];
            const salts_configurator_provider_binding *provider;

            if (dependency->consumer_index != i ||
                dependency->provider_index >= context->deployment_count ||
                !cmeta_interface_desc_valid(dependency->interface_desc))
                return false;
            provider = salts_configurator_provider_at(
                context, dependency->provider_index);
            if (consumer == NULL || provider == NULL ||
                dependency->provider_component != provider->component ||
                dependency->provider_instance !=
                    &context->instances[dependency->provider_index].object ||
                dependency->provider_interfaces != provider->interfaces ||
                !salts_configurator_requires(
                    consumer->component, dependency->interface_desc) ||
                !salts_configurator_provides(
                    provider->component, dependency->interface_desc))
                return false;
        }
    }

    /* A valid permutation is not sufficient: every provider must precede its
     * consumer so start/rollback/stop all use the canonical dependency order. */
    for (i = 0u; i < context->activation_count; ++i)
        if (!salts_configurator_dependencies_ready(
                context, context->activation_order[i], i))
            return false;

    return true;
}

static void salts_configurator_rollback(
    salts_configurator_context *context,
    size_t activated_count) {
    while (activated_count != 0u) {
        size_t component_index;
        salts_configurator_instance *instance;
        const salts_configurator_provider_binding *binding;

        --activated_count;
        component_index = context->activation_order[activated_count];
        instance = &context->instances[component_index];
        binding = salts_configurator_provider_at(context, component_index);

        if (!instance->active)
            continue;

        if (binding->deactivate != NULL)
            binding->deactivate(binding->provider_context, &instance->object);
        instance->active = false;
        cmeta_object_release(&instance->object);
    }
    context->activation_count = 0u;
}

const char *salts_configurator_status_string(salts_configurator_status status) {
    switch (status) {
    case SALTS_CONFIGURATOR_OK: return "ok";
    case SALTS_CONFIGURATOR_INVALID_ARGUMENT: return "invalid argument";
    case SALTS_CONFIGURATOR_INVALID_COMPONENT: return "invalid component";
    case SALTS_CONFIGURATOR_DUPLICATE_COMPONENT_ID: return "duplicate component id";
    case SALTS_CONFIGURATOR_INVALID_SELECTION: return "invalid provider selection";
    case SALTS_CONFIGURATOR_CONFIG_MISMATCH: return "configuration mismatch";
    case SALTS_CONFIGURATOR_CAPACITY_EXCEEDED: return "capacity exceeded";
    case SALTS_CONFIGURATOR_MISSING_PROVIDER: return "missing provider";
    case SALTS_CONFIGURATOR_AMBIGUOUS_PROVIDER: return "ambiguous provider";
    case SALTS_CONFIGURATOR_DEPENDENCY_CYCLE: return "dependency cycle";
    case SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE: return "interface unavailable";
    case SALTS_CONFIGURATOR_CREATE_FAILED: return "create failed";
    case SALTS_CONFIGURATOR_ACTIVATE_FAILED: return "activate failed";
    case SALTS_CONFIGURATOR_INVALID_STATE: return "invalid state";
    }
    return "unknown component status";
}

salts_configurator_status salts_configurator_context_init(
    salts_configurator_context *context,
    const salts_configurator_deployment *deployments,
    size_t deployment_count,
    const salts_configurator_selection *selections,
    size_t selection_count,
    salts_configurator_instance *instances,
    size_t instance_capacity,
    salts_configurator_dependency *dependencies,
    size_t dependency_capacity,
    size_t *activation_order,
    size_t activation_capacity) {
    size_t i;
    size_t j;

    if (context == NULL)
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;

    /* The caller must explicitly initialize the context before first use.
     * Never inspect an indeterminate automatic object. During legal reuse,
     * only ZERO, fully rolled-back FAILED or STOPPED may be reset; otherwise
     * resetting would discard live ObjectRef/graph teardown authority. */
    if (context->state != SALTS_CONFIGURATOR_CONTEXT_ZERO &&
        context->state != SALTS_CONFIGURATOR_CONTEXT_STOPPED &&
        context->state != SALTS_CONFIGURATOR_CONTEXT_FAILED)
        return SALTS_CONFIGURATOR_INVALID_STATE;
    /* activation_count is the resolved plan length while READY/RESOLVED/
     * ACTIVE, and is cleared only after rollback or successful stop. A
     * reusable state carrying entries is inconsistent and may still own
     * ObjectRefs; do not overwrite its teardown authority. */
    if (context->activation_count != 0u)
        return SALTS_CONFIGURATOR_INVALID_STATE;
    /* ZERO is valid only as the canonical empty initial representation. Merely
     * forging the enum must not erase retained arrays or ObjectRef ownership. */
    if (context->state == SALTS_CONFIGURATOR_CONTEXT_ZERO &&
        !salts_configurator_context_zero_is_clear(context))
        return SALTS_CONFIGURATOR_INVALID_STATE;
    if ((context->state == SALTS_CONFIGURATOR_CONTEXT_STOPPED ||
         context->state == SALTS_CONFIGURATOR_CONTEXT_FAILED) &&
        !salts_configurator_instances_settled(context))
        return SALTS_CONFIGURATOR_INVALID_STATE;

    *context = (salts_configurator_context)SALTS_CONFIGURATOR_CONTEXT_INIT;
    context->failure = salts_configurator_failure_none();

    if ((deployment_count != 0u &&
         (deployments == NULL || instances == NULL || activation_order == NULL)) ||
        (instance_capacity != 0u && instances == NULL) ||
        (activation_capacity != 0u && activation_order == NULL) ||
        (selection_count != 0u && selections == NULL) ||
        (dependency_capacity != 0u && dependencies == NULL))
        return salts_configurator_fail(
            context, SALTS_CONFIGURATOR_INVALID_ARGUMENT,
            SALTS_CONFIGURATOR_PHASE_INIT, SALTS_CONFIGURATOR_INDEX_NONE,
            SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);

    if (deployment_count > instance_capacity ||
        deployment_count > activation_capacity)
        return salts_configurator_fail(
            context, SALTS_CONFIGURATOR_CAPACITY_EXCEEDED,
            SALTS_CONFIGURATOR_PHASE_INIT, SALTS_CONFIGURATOR_INDEX_NONE,
            SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);

    context->deployments = deployments;
    context->deployment_count = deployment_count;
    context->selections = selections;
    context->selection_count = selection_count;
    context->instances = instances;
    context->instance_capacity = instance_capacity;
    context->dependencies = dependencies;
    context->dependency_capacity = dependency_capacity;
    context->activation_order = activation_order;
    context->activation_capacity = activation_capacity;

    for (i = 0u; i < deployment_count; ++i) {
        salts_configurator_status status =
            salts_configurator_deployment_status(&deployments[i]);
        if (status != SALTS_CONFIGURATOR_OK)
            return salts_configurator_fail(
                context, status, SALTS_CONFIGURATOR_PHASE_INIT, i,
                SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);

        for (j = 0u; j < i; ++j) {
            if (strcmp(
                    deployments[i].provider->component->stable_id,
                    deployments[j].provider->component->stable_id) == 0)
                return salts_configurator_fail(
                    context, SALTS_CONFIGURATOR_DUPLICATE_COMPONENT_ID,
                    SALTS_CONFIGURATOR_PHASE_INIT, i,
                    SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);
        }

    }

    for (i = 0u; i < selection_count; ++i) {
        const salts_configurator_selection *selection = &selections[i];
        size_t consumer_index;
        size_t provider_index;

        if (selection->consumer_component_id == NULL ||
            selection->consumer_component_id[0] == '\0' ||
            selection->provider_component_id == NULL ||
            selection->provider_component_id[0] == '\0' ||
            !cmeta_interface_desc_valid(selection->requirement))
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_INVALID_SELECTION,
                SALTS_CONFIGURATOR_PHASE_INIT, SALTS_CONFIGURATOR_INDEX_NONE,
                i, CMETA_OK);

        consumer_index =
            salts_configurator_find_id(context, selection->consumer_component_id);
        provider_index =
            salts_configurator_find_id(context, selection->provider_component_id);

        if (consumer_index == SALTS_CONFIGURATOR_INDEX_NONE ||
            provider_index == SALTS_CONFIGURATOR_INDEX_NONE ||
            !salts_configurator_requires(
                deployments[consumer_index].provider->component, selection->requirement) ||
            !salts_configurator_provides(
                deployments[provider_index].provider->component, selection->requirement))
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_INVALID_SELECTION,
                SALTS_CONFIGURATOR_PHASE_INIT,
                consumer_index, i, CMETA_OK);

        for (j = 0u; j < i; ++j) {
            if (strcmp(
                    selections[j].consumer_component_id,
                    selection->consumer_component_id) == 0 &&
                cmeta_interface_desc_equal(
                    selections[j].requirement, selection->requirement))
                return salts_configurator_fail(
                    context, SALTS_CONFIGURATOR_INVALID_SELECTION,
                    SALTS_CONFIGURATOR_PHASE_INIT,
                    consumer_index, i, CMETA_OK);
        }
    }

    /* The entire caller-provided capacity is Configurator scratch storage.
     * Initialize unused slots too, so a truncated public deployment_count
     * cannot later conceal an ObjectRef in the tail during reuse. */
    for (i = 0u; i < instance_capacity; ++i)
        salts_configurator_instance_reset(&instances[i]);

    context->state = SALTS_CONFIGURATOR_CONTEXT_READY;
    return SALTS_CONFIGURATOR_OK;
}

static bool salts_configurator_ready_context_valid(
    const salts_configurator_context *context) {
    size_t i;
    size_t j;

    if (context == NULL ||
        context->deployment_count > context->instance_capacity ||
        context->deployment_count > context->activation_capacity ||
        context->dependency_count != 0u ||
        context->activation_count != 0u ||
        (context->instance_capacity != 0u && context->instances == NULL) ||
        (context->activation_capacity != 0u && context->activation_order == NULL) ||
        (context->deployment_count != 0u &&
         (context->deployments == NULL || context->instances == NULL ||
          context->activation_order == NULL)) ||
        (context->selection_count != 0u && context->selections == NULL) ||
        (context->dependency_capacity != 0u && context->dependencies == NULL))
        return false;

    /* READY is pre-resolution: no instance, dependency slice, or activation
     * plan may already own lifecycle state. Check bounds and backing storage
     * before the resolver dereferences caller-owned arrays. */
    for (i = 0u; i < context->deployment_count; ++i) {
        const salts_configurator_instance *instance = &context->instances[i];
        const salts_configurator_deployment *deployment = &context->deployments[i];

        if (salts_configurator_deployment_status(deployment) != SALTS_CONFIGURATOR_OK ||
            instance->active ||
            !salts_configurator_object_ref_empty(&instance->object) ||
            instance->dependency_offset != 0u ||
            instance->dependency_count != 0u)
            return false;

        for (j = 0u; j < i; ++j)
            if (strcmp(
                    deployment->provider->component->stable_id,
                    context->deployments[j].provider->component->stable_id) == 0)
                return false;
    }

    for (i = context->deployment_count; i < context->instance_capacity; ++i) {
        const salts_configurator_instance *instance = &context->instances[i];
        if (instance->active ||
            !salts_configurator_object_ref_empty(&instance->object) ||
            instance->dependency_offset != 0u ||
            instance->dependency_count != 0u)
            return false;
    }

    for (i = 0u; i < context->selection_count; ++i) {
        const salts_configurator_selection *selection = &context->selections[i];
        size_t consumer_index;
        size_t provider_index;

        if (selection->consumer_component_id == NULL ||
            selection->consumer_component_id[0] == '\0' ||
            selection->provider_component_id == NULL ||
            selection->provider_component_id[0] == '\0' ||
            !cmeta_interface_desc_valid(selection->requirement))
            return false;

        consumer_index = salts_configurator_find_id(
            context, selection->consumer_component_id);
        provider_index = salts_configurator_find_id(
            context, selection->provider_component_id);
        if (consumer_index == SALTS_CONFIGURATOR_INDEX_NONE ||
            provider_index == SALTS_CONFIGURATOR_INDEX_NONE ||
            !salts_configurator_requires(
                context->deployments[consumer_index].provider->component,
                selection->requirement) ||
            !salts_configurator_provides(
                context->deployments[provider_index].provider->component,
                selection->requirement))
            return false;

        for (j = 0u; j < i; ++j)
            if (strcmp(
                    context->selections[j].consumer_component_id,
                    selection->consumer_component_id) == 0 &&
                cmeta_interface_desc_equal(
                    context->selections[j].requirement,
                    selection->requirement))
                return false;
    }

    return true;
}

salts_configurator_status salts_configurator_context_resolve(
    salts_configurator_context *context) {
    size_t required_total = 0u;
    size_t dependency_count = 0u;
    size_t i;

    if (context == NULL)
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;
    if (context->state != SALTS_CONFIGURATOR_CONTEXT_READY ||
        !salts_configurator_ready_context_valid(context))
        return SALTS_CONFIGURATOR_INVALID_STATE;

    context->failure = salts_configurator_failure_none();
    context->dependency_count = 0u;
    context->activation_count = 0u;

    for (i = 0u; i < context->deployment_count; ++i) {
        size_t count =
            salts_configurator_requirement_count(salts_configurator_provider_at(context, i)->component);
        if (required_total > SIZE_MAX - count)
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_CAPACITY_EXCEEDED,
                SALTS_CONFIGURATOR_PHASE_RESOLVE, i,
                SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);
        required_total += count;
    }

    if (required_total > context->dependency_capacity ||
        (required_total != 0u && context->dependencies == NULL))
        return salts_configurator_fail(
            context, SALTS_CONFIGURATOR_CAPACITY_EXCEEDED,
            SALTS_CONFIGURATOR_PHASE_RESOLVE, SALTS_CONFIGURATOR_INDEX_NONE,
            SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);

    for (i = 0u; i < context->deployment_count; ++i) {
        const cmeta_component_desc *consumer = salts_configurator_provider_at(context, i)->component;
        salts_configurator_instance *instance = &context->instances[i];
        size_t capability_index;

        instance->dependency_offset = dependency_count;
        instance->dependency_count = 0u;

        for (capability_index = 0u;
             capability_index < consumer->capability_count;
             ++capability_index) {
            const cmeta_component_capability *requirement =
                &consumer->capabilities[capability_index];
            size_t candidate = SALTS_CONFIGURATOR_INDEX_NONE;
            salts_configurator_status selection_status;

            if (requirement->role != CMETA_COMPONENT_REQUIRES)
                continue;

            selection_status = salts_configurator_select_provider(
                context, i, requirement->interface_desc, &candidate);
            if (selection_status != SALTS_CONFIGURATOR_OK)
                return salts_configurator_fail(
                    context, selection_status,
                    SALTS_CONFIGURATOR_PHASE_RESOLVE, i, dependency_count,
                    CMETA_OK);

            context->dependencies[dependency_count].consumer_index = i;
            context->dependencies[dependency_count].provider_index = candidate;
            context->dependencies[dependency_count].interface_desc =
                requirement->interface_desc;
            context->dependencies[dependency_count].provider_component =
                salts_configurator_provider_at(context, candidate)->component;
            context->dependencies[dependency_count].provider_instance =
                &context->instances[candidate].object;
            context->dependencies[dependency_count].provider_interfaces =
                salts_configurator_provider_at(context, candidate)->interfaces;
            ++dependency_count;
            ++instance->dependency_count;
        }
    }

    context->dependency_count = dependency_count;

    for (i = 0u; i < context->deployment_count; ++i) {
        size_t candidate;
        size_t selected = SALTS_CONFIGURATOR_INDEX_NONE;

        for (candidate = 0u;
             candidate < context->deployment_count;
             ++candidate) {
            if (salts_configurator_order_contains(context, i, candidate))
                continue;
            if (salts_configurator_dependencies_ready(context, candidate, i)) {
                selected = candidate;
                break;
            }
        }

        if (selected == SALTS_CONFIGURATOR_INDEX_NONE)
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_DEPENDENCY_CYCLE,
                SALTS_CONFIGURATOR_PHASE_RESOLVE, SALTS_CONFIGURATOR_INDEX_NONE,
                SALTS_CONFIGURATOR_INDEX_NONE, CMETA_OK);

        context->activation_order[i] = selected;
    }

    context->activation_count = context->deployment_count;
    context->state = SALTS_CONFIGURATOR_CONTEXT_RESOLVED;
    return SALTS_CONFIGURATOR_OK;
}

salts_configurator_status salts_configurator_context_start(
    salts_configurator_context *context) {
    size_t order_index;

    if (context == NULL)
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;
    if (context->state != SALTS_CONFIGURATOR_CONTEXT_RESOLVED ||
        !salts_configurator_activation_plan_valid(context, false))
        return SALTS_CONFIGURATOR_INVALID_STATE;

    context->failure = salts_configurator_failure_none();

    for (order_index = 0u;
         order_index < context->activation_count;
         ++order_index) {
        const size_t component_index = context->activation_order[order_index];
        const salts_configurator_provider_binding *binding =
            salts_configurator_provider_at(context, component_index);
        salts_configurator_instance *instance =
            &context->instances[component_index];
        const salts_configurator_dependency *dependencies =
            instance->dependency_count == 0u
                ? NULL
                : &context->dependencies[instance->dependency_offset];
        size_t dependency_index;
        cmeta_status provider_status;

        for (dependency_index = 0u;
             dependency_index < instance->dependency_count;
             ++dependency_index) {
            cmeta_interface_projection projection =
                CMETA_INTERFACE_PROJECTION_INIT;
            const salts_configurator_dependency *dependency =
                &dependencies[dependency_index];

            provider_status = cmeta_object_interface_project_borrowed(
                dependency->provider_instance,
                dependency->provider_interfaces,
                dependency->interface_desc,
                &projection);
            if (provider_status != CMETA_OK) {
                salts_configurator_rollback(context, order_index);
                return salts_configurator_fail(
                    context, SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE,
                    SALTS_CONFIGURATOR_PHASE_DEPENDENCY, component_index,
                    instance->dependency_offset + dependency_index,
                    provider_status);
            }
        }

        instance->object = (cmeta_object_ref)CMETA_OBJECT_REF_INIT;
        provider_status = binding->create(
            binding->provider_context,
            context->deployments[component_index].config_data,
            context->deployments[component_index].config_value,
            dependencies,
            instance->dependency_count,
            &instance->object);

        if (provider_status != CMETA_OK) {
            if (cmeta_object_ref_valid(&instance->object))
                cmeta_object_release(&instance->object);
            salts_configurator_rollback(context, order_index);
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_CREATE_FAILED,
                SALTS_CONFIGURATOR_PHASE_CREATE, component_index,
                SALTS_CONFIGURATOR_INDEX_NONE, provider_status);
        }

        if (!cmeta_object_ref_valid(&instance->object)) {
            salts_configurator_rollback(context, order_index);
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_CREATE_FAILED,
                SALTS_CONFIGURATOR_PHASE_CREATE, component_index,
                SALTS_CONFIGURATOR_INDEX_NONE, CMETA_CALLBACK_ERROR);
        }

        if (binding->activate != NULL) {
            provider_status =
                binding->activate(binding->provider_context, &instance->object);
            if (provider_status != CMETA_OK) {
                cmeta_object_release(&instance->object);
                salts_configurator_rollback(context, order_index);
                return salts_configurator_fail(
                    context, SALTS_CONFIGURATOR_ACTIVATE_FAILED,
                    SALTS_CONFIGURATOR_PHASE_ACTIVATE, component_index,
                    SALTS_CONFIGURATOR_INDEX_NONE, provider_status);
            }
        }

        provider_status =
            salts_configurator_preflight_provides(binding, &instance->object);
        if (provider_status != CMETA_OK) {
            if (binding->deactivate != NULL)
                binding->deactivate(
                    binding->provider_context, &instance->object);
            cmeta_object_release(&instance->object);
            salts_configurator_rollback(context, order_index);
            return salts_configurator_fail(
                context, SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE,
                SALTS_CONFIGURATOR_PHASE_PROVIDE, component_index,
                SALTS_CONFIGURATOR_INDEX_NONE, provider_status);
        }

        instance->active = true;
    }

    context->state = SALTS_CONFIGURATOR_CONTEXT_ACTIVE;
    return SALTS_CONFIGURATOR_OK;
}

salts_configurator_status salts_configurator_context_stop(
    salts_configurator_context *context) {
    size_t order_index;

    if (context == NULL)
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;
    if (context->state != SALTS_CONFIGURATOR_CONTEXT_ACTIVE ||
        !salts_configurator_activation_plan_valid(context, true))
        return SALTS_CONFIGURATOR_INVALID_STATE;

    order_index = context->activation_count;
    while (order_index != 0u) {
        size_t component_index;
        salts_configurator_instance *instance;
        const salts_configurator_provider_binding *binding;

        --order_index;
        component_index = context->activation_order[order_index];
        instance = &context->instances[component_index];
        binding = salts_configurator_provider_at(context, component_index);

        if (!instance->active)
            continue;

        if (binding->deactivate != NULL)
            binding->deactivate(binding->provider_context, &instance->object);
        instance->active = false;
        cmeta_object_release(&instance->object);
    }

    /* activation_count describes the resolved activation plan. Once all
     * instances have been deactivated and released, STOPPED has no remaining
     * activation entries and is safe to reuse or hand to ServiceHost cleanup.
     * Do not retain borrowed array addresses beyond this boundary. */
    context->activation_count = 0u;
    if (!salts_configurator_instances_settled(context)) {
        context->state = SALTS_CONFIGURATOR_CONTEXT_FAILED;
        return SALTS_CONFIGURATOR_INVALID_STATE;
    }
    salts_configurator_context_detach_storage(context);
    context->state = SALTS_CONFIGURATOR_CONTEXT_STOPPED;
    return SALTS_CONFIGURATOR_OK;
}

salts_configurator_status salts_configurator_context_find_service_from(
    const salts_configurator_context *context,
    const char *provider_component_id,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service) {
    salts_configurator_service candidate_service;
    cmeta_interface_projection projection =
        CMETA_INTERFACE_PROJECTION_INIT;
    size_t candidate;
    cmeta_status status;

    if (context == NULL || out_service == NULL ||
        provider_component_id == NULL || provider_component_id[0] == '\0' ||
        !cmeta_interface_desc_valid(interface_desc))
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;
    if (context->state != SALTS_CONFIGURATOR_CONTEXT_ACTIVE)
        return SALTS_CONFIGURATOR_INVALID_STATE;

    candidate = salts_configurator_find_id(context, provider_component_id);
    if (candidate == SALTS_CONFIGURATOR_INDEX_NONE)
        return SALTS_CONFIGURATOR_MISSING_PROVIDER;
    if (!salts_configurator_provides(
            salts_configurator_provider_at(context, candidate)->component, interface_desc))
        return SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE;
    if (!context->instances[candidate].active)
        return SALTS_CONFIGURATOR_INVALID_STATE;

    status = cmeta_object_interface_project_borrowed(
        &context->instances[candidate].object,
        salts_configurator_provider_at(context, candidate)->interfaces,
        interface_desc,
        &projection);
    if (status != CMETA_OK)
        return SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE;

    candidate_service.component = salts_configurator_provider_at(context, candidate)->component;
    candidate_service.object = &context->instances[candidate].object;
    candidate_service.interfaces = salts_configurator_provider_at(context, candidate)->interfaces;
    *out_service = candidate_service;
    return SALTS_CONFIGURATOR_OK;
}

salts_configurator_status salts_configurator_context_find_service(
    const salts_configurator_context *context,
    const cmeta_interface_desc *interface_desc,
    salts_configurator_service *out_service) {
    salts_configurator_service candidate_service;
    size_t candidate = SALTS_CONFIGURATOR_INDEX_NONE;
    size_t candidate_count = 0u;
    size_t i;

    if (context == NULL || out_service == NULL ||
        !cmeta_interface_desc_valid(interface_desc))
        return SALTS_CONFIGURATOR_INVALID_ARGUMENT;
    if (context->state != SALTS_CONFIGURATOR_CONTEXT_ACTIVE)
        return SALTS_CONFIGURATOR_INVALID_STATE;

    for (i = 0u; i < context->deployment_count; ++i) {
        if (salts_configurator_provides(
                salts_configurator_provider_at(context, i)->component, interface_desc)) {
            candidate = i;
            ++candidate_count;
        }
    }

    if (candidate_count == 0u)
        return SALTS_CONFIGURATOR_MISSING_PROVIDER;
    if (candidate_count != 1u)
        return SALTS_CONFIGURATOR_AMBIGUOUS_PROVIDER;

    if (!context->instances[candidate].active)
        return SALTS_CONFIGURATOR_INVALID_STATE;

    {
        cmeta_interface_projection projection =
            CMETA_INTERFACE_PROJECTION_INIT;
        cmeta_status status = cmeta_object_interface_project_borrowed(
            &context->instances[candidate].object,
            salts_configurator_provider_at(context, candidate)->interfaces,
            interface_desc,
            &projection);
        if (status != CMETA_OK)
            return SALTS_CONFIGURATOR_INTERFACE_UNAVAILABLE;
    }

    candidate_service.component = salts_configurator_provider_at(context, candidate)->component;
    candidate_service.object = &context->instances[candidate].object;
    candidate_service.interfaces = salts_configurator_provider_at(context, candidate)->interfaces;
    *out_service = candidate_service;
    return SALTS_CONFIGURATOR_OK;
}

const salts_configurator_failure *salts_configurator_context_failure(
    const salts_configurator_context *context) {
    return context == NULL ? NULL : &context->failure;
}
