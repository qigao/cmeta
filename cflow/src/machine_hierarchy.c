#include <cflow/machine_hierarchy.h>

#include "machine_instance_internal.h"
#include "timer_event_internal.h"

#include <thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct hierarchy_route_record {
    size_t exit_offset;
    size_t exit_count;
    size_t entry_offset;
    size_t entry_count;
} hierarchy_route_record;

typedef struct hierarchy_expanded_transition {
    cflow_machine_transition transition;
    uint32_t declaration_priority;
    size_t declaration_depth;
    size_t source_index;
    size_t target_index;
} hierarchy_expanded_transition;

typedef struct cflow_machine_hierarchy_impl {
    cflow_machine flat;
    cflow_machine_hierarchy_state *states;
    size_t *parents;
    size_t *depths;
    size_t state_count;
    hierarchy_route_record *routes;
    cflow_machine_state_id *route_states;
    size_t route_state_count;
    size_t route_count;
} cflow_machine_hierarchy_impl;

typedef struct cflow_machine_hierarchy_instance_impl {
    const cflow_machine_hierarchy *hierarchy;
    cflow_machine_instance machine;
    cflow_timer_event_queue timers;
    cflow_executor *executor;
    cmeta_mutex_t gate;
    bool closed;
    bool cancelled;
} cflow_machine_hierarchy_instance_impl;

static bool checked_mul(size_t left, size_t right, size_t *out) {
    if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return false;
    *out = left * right;
    return true;
}

static int compare_hierarchy_state(const void *left, const void *right) {
    const cflow_machine_hierarchy_state *a =
        (const cflow_machine_hierarchy_state *)left;
    const cflow_machine_hierarchy_state *b =
        (const cflow_machine_hierarchy_state *)right;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int compare_expanded_transition(const void *left, const void *right) {
    const hierarchy_expanded_transition *a =
        (const hierarchy_expanded_transition *)left;
    const hierarchy_expanded_transition *b =
        (const hierarchy_expanded_transition *)right;
    if (a->transition.source != b->transition.source)
        return a->transition.source < b->transition.source ? -1 : 1;
    if (a->transition.event != b->transition.event)
        return a->transition.event < b->transition.event ? -1 : 1;
    if (a->declaration_depth != b->declaration_depth)
        return a->declaration_depth > b->declaration_depth ? -1 : 1;
    if (a->declaration_priority != b->declaration_priority)
        return a->declaration_priority < b->declaration_priority ? -1 : 1;
    if (a->transition.target != b->transition.target)
        return a->transition.target < b->transition.target ? -1 : 1;
    if (a->transition.guard != b->transition.guard)
        return a->transition.guard < b->transition.guard ? -1 : 1;
    if (a->transition.action != b->transition.action)
        return a->transition.action < b->transition.action ? -1 : 1;
    return 0;
}

static size_t find_state_index(const cflow_machine_hierarchy_impl *impl,
                               cflow_machine_state_id id) {
    size_t left = 0u;
    size_t right = impl->state_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_state_id current = impl->states[middle].id;
        if (current == id) return middle;
        if (current < id) left = middle + 1u;
        else right = middle;
    }
    return SIZE_MAX;
}

static void hierarchy_impl_destroy(cflow_machine_hierarchy_impl *impl) {
    if (impl == NULL) return;
    cflow_machine_destroy(&impl->flat);
    free(impl->route_states);
    free(impl->routes);
    free(impl->depths);
    free(impl->parents);
    free(impl->states);
    free(impl);
}

static cflow_machine_hierarchy_status copy_states(
    cflow_machine_hierarchy_impl *impl,
    const cflow_machine_hierarchy_definition *definition) {
    size_t bytes;
    size_t index;
    const cmeta_type_desc *state_type;

    if (definition->state_count == 0u)
        return CFLOW_MACHINE_HIERARCHY_EMPTY;
    if (definition->state_count > CFLOW_MACHINE_MAX_STATES)
        return CFLOW_MACHINE_HIERARCHY_LIMIT_EXCEEDED;
    if (!checked_mul(definition->state_count, sizeof(*impl->states), &bytes))
        return CFLOW_MACHINE_HIERARCHY_LIMIT_EXCEEDED;
    impl->states = (cflow_machine_hierarchy_state *)malloc(bytes);
    impl->parents = (size_t *)malloc(definition->state_count *
                                     sizeof(*impl->parents));
    impl->depths = (size_t *)calloc(definition->state_count,
                                    sizeof(*impl->depths));
    if (impl->states == NULL || impl->parents == NULL || impl->depths == NULL)
        return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    memcpy(impl->states, definition->states, bytes);
    impl->state_count = definition->state_count;
    qsort(impl->states, impl->state_count, sizeof(*impl->states),
          compare_hierarchy_state);

    state_type = impl->states[0].value_type;
    for (index = 0u; index < impl->state_count; ++index) {
        const cflow_machine_hierarchy_state *state = &impl->states[index];
        if (state->id == 0u) return CFLOW_MACHINE_HIERARCHY_INVALID_ID;
        if (index != 0u && state->id == impl->states[index - 1u].id)
            return CFLOW_MACHINE_HIERARCHY_DUPLICATE_ID;
        if (!cmeta_type_desc_valid(state->value_type))
            return CFLOW_MACHINE_HIERARCHY_INVALID_TYPE;
        if (!cmeta_type_equal(state_type, state->value_type))
            return CFLOW_MACHINE_HIERARCHY_TYPE_MISMATCH;
        if (state->kind < CFLOW_MACHINE_STATE_ACTIVE ||
            state->kind > CFLOW_MACHINE_STATE_ERROR)
            return CFLOW_MACHINE_HIERARCHY_INVALID_STATE_KIND;
        impl->parents[index] = state->parent == 0u
            ? SIZE_MAX : find_state_index(impl, state->parent);
        if (state->parent != 0u && impl->parents[index] == SIZE_MAX)
            return CFLOW_MACHINE_HIERARCHY_INVALID_PARENT;
    }
    return CFLOW_MACHINE_HIERARCHY_OK;
}

static cflow_machine_hierarchy_status validate_tree(
    cflow_machine_hierarchy_impl *impl) {
    unsigned char *colors;
    size_t *stack;
    bool *has_child;
    size_t root_count = 0u;
    size_t index;
    cflow_machine_hierarchy_status result = CFLOW_MACHINE_HIERARCHY_OK;

    colors = (unsigned char *)calloc(impl->state_count, sizeof(*colors));
    stack = (size_t *)malloc(impl->state_count * sizeof(*stack));
    has_child = (bool *)calloc(impl->state_count, sizeof(*has_child));
    if (colors == NULL || stack == NULL || has_child == NULL) {
        result = CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
        goto cleanup;
    }
    for (index = 0u; index < impl->state_count; ++index) {
        if (impl->parents[index] == SIZE_MAX) ++root_count;
        else has_child[impl->parents[index]] = true;
    }
    if (root_count != 1u) {
        result = CFLOW_MACHINE_HIERARCHY_INVALID_PARENT;
        goto cleanup;
    }
    for (index = 0u; index < impl->state_count; ++index) {
        size_t current = index;
        size_t count = 0u;
        if (colors[current] == 2u) continue;
        while (current != SIZE_MAX && colors[current] == 0u) {
            colors[current] = 1u;
            stack[count++] = current;
            current = impl->parents[current];
        }
        if (current != SIZE_MAX && colors[current] == 1u) {
            result = CFLOW_MACHINE_HIERARCHY_INVALID_PARENT;
            goto cleanup;
        }
        while (count != 0u) {
            const size_t node = stack[--count];
            const size_t parent = impl->parents[node];
            impl->depths[node] = parent == SIZE_MAX
                ? 0u : impl->depths[parent] + 1u;
            colors[node] = 2u;
        }
    }
    for (index = 0u; index < impl->state_count; ++index) {
        const cflow_machine_hierarchy_state *state = &impl->states[index];
        if (has_child[index]) {
            const size_t child = find_state_index(impl, state->initial_child);
            if (state->kind != CFLOW_MACHINE_STATE_ACTIVE)
                result = CFLOW_MACHINE_HIERARCHY_INVALID_STATE_KIND;
            else if (state->initial_child == 0u || child == SIZE_MAX ||
                     impl->parents[child] != index)
                result = CFLOW_MACHINE_HIERARCHY_INVALID_INITIAL_CHILD;
        } else if (state->initial_child != 0u) {
            result = CFLOW_MACHINE_HIERARCHY_INVALID_INITIAL_CHILD;
        }
        if (result != CFLOW_MACHINE_HIERARCHY_OK) goto cleanup;
    }

cleanup:
    free(has_child);
    free(stack);
    free(colors);
    return result;
}

static size_t resolve_initial_leaf(const cflow_machine_hierarchy_impl *impl,
                                   size_t index) {
    size_t hops = 0u;
    while (index != SIZE_MAX &&
           impl->states[index].initial_child != 0u &&
           hops++ < impl->state_count) {
        index = find_state_index(impl, impl->states[index].initial_child);
    }
    return hops <= impl->state_count ? index : SIZE_MAX;
}

static bool is_descendant(const cflow_machine_hierarchy_impl *impl,
                          size_t node, size_t ancestor) {
    while (node != SIZE_MAX) {
        if (node == ancestor) return true;
        node = impl->parents[node];
    }
    return false;
}

static cflow_machine_hierarchy_status expand_transitions(
    cflow_machine_hierarchy_impl *impl,
    const cflow_machine_hierarchy_definition *definition,
    hierarchy_expanded_transition **out_rows,
    size_t *out_count) {
    hierarchy_expanded_transition *rows;
    size_t count = 0u;
    size_t leaf;
    size_t declaration;

    for (leaf = 0u; leaf < impl->state_count; ++leaf) {
        if (impl->states[leaf].initial_child != 0u) continue;
        for (declaration = 0u; declaration < definition->transition_count;
             ++declaration) {
            const size_t source = find_state_index(
                impl, definition->transitions[declaration].source);
            if (source == SIZE_MAX)
                return CFLOW_MACHINE_HIERARCHY_UNKNOWN_STATE;
            if (is_descendant(impl, leaf, source)) {
                if (count == CFLOW_MACHINE_MAX_TRANSITIONS)
                    return CFLOW_MACHINE_HIERARCHY_LIMIT_EXCEEDED;
                ++count;
            }
        }
    }
    rows = count != 0u
        ? (hierarchy_expanded_transition *)calloc(count, sizeof(*rows))
        : NULL;
    if (count != 0u && rows == NULL)
        return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    count = 0u;
    for (leaf = 0u; leaf < impl->state_count; ++leaf) {
        if (impl->states[leaf].initial_child != 0u) continue;
        for (declaration = 0u; declaration < definition->transition_count;
             ++declaration) {
            const cflow_machine_transition *decl =
                &definition->transitions[declaration];
            const size_t source = find_state_index(impl, decl->source);
            const size_t target_node = find_state_index(impl, decl->target);
            size_t target;
            if (target_node == SIZE_MAX) {
                free(rows);
                return CFLOW_MACHINE_HIERARCHY_UNKNOWN_STATE;
            }
            if (!is_descendant(impl, leaf, source)) continue;
            target = resolve_initial_leaf(impl, target_node);
            if (target == SIZE_MAX) {
                free(rows);
                return CFLOW_MACHINE_HIERARCHY_INVALID_INITIAL_CHILD;
            }
            rows[count].transition = *decl;
            rows[count].transition.source = impl->states[leaf].id;
            rows[count].transition.target = impl->states[target].id;
            rows[count].declaration_priority = decl->priority;
            rows[count].declaration_depth = impl->depths[source];
            rows[count].source_index = leaf;
            rows[count].target_index = target;
            ++count;
        }
    }
    qsort(rows, count, sizeof(*rows), compare_expanded_transition);
    for (leaf = 0u; leaf < count; ++leaf) {
        if (leaf != 0u &&
            rows[leaf].transition.source == rows[leaf - 1u].transition.source &&
            rows[leaf].transition.event == rows[leaf - 1u].transition.event &&
            rows[leaf].declaration_depth ==
                rows[leaf - 1u].declaration_depth &&
            rows[leaf].declaration_priority ==
                rows[leaf - 1u].declaration_priority) {
            free(rows);
            return CFLOW_MACHINE_HIERARCHY_AMBIGUOUS_TRANSITION;
        }
        if (leaf == 0u ||
            rows[leaf].transition.source != rows[leaf - 1u].transition.source ||
            rows[leaf].transition.event != rows[leaf - 1u].transition.event) {
            rows[leaf].transition.priority = 0u;
        } else {
            rows[leaf].transition.priority =
                rows[leaf - 1u].transition.priority + 1u;
        }
    }
    *out_rows = rows;
    *out_count = count;
    return CFLOW_MACHINE_HIERARCHY_OK;
}

static size_t least_common_ancestor(
    const cflow_machine_hierarchy_impl *impl, size_t source, size_t target) {
    while (impl->depths[source] > impl->depths[target])
        source = impl->parents[source];
    while (impl->depths[target] > impl->depths[source])
        target = impl->parents[target];
    while (source != target) {
        source = impl->parents[source];
        target = impl->parents[target];
    }
    return source;
}

static cflow_machine_hierarchy_status build_routes(
    cflow_machine_hierarchy_impl *impl,
    const hierarchy_expanded_transition *rows,
    size_t count) {
    size_t total = 0u;
    size_t route_bytes = 0u;
    size_t index;
    size_t cursor = 0u;
    if (count == 0u) return CFLOW_MACHINE_HIERARCHY_OK;
    impl->routes = (hierarchy_route_record *)calloc(count,
                                                     sizeof(*impl->routes));
    if (impl->routes == NULL)
        return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    for (index = 0u; index < count; ++index) {
        const size_t lca = least_common_ancestor(
            impl, rows[index].source_index, rows[index].target_index);
        const size_t exits = impl->depths[rows[index].source_index] -
                             impl->depths[lca];
        const size_t entries = impl->depths[rows[index].target_index] -
                               impl->depths[lca];
        if (exits > SIZE_MAX - total || entries > SIZE_MAX - total - exits)
            return CFLOW_MACHINE_HIERARCHY_LIMIT_EXCEEDED;
        total += exits + entries;
    }
    if (total != 0u) {
        if (!checked_mul(total, sizeof(*impl->route_states), &route_bytes))
            return CFLOW_MACHINE_HIERARCHY_LIMIT_EXCEEDED;
        impl->route_states = (cflow_machine_state_id *)malloc(
            route_bytes);
        if (impl->route_states == NULL)
            return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    }
    for (index = 0u; index < count; ++index) {
        hierarchy_route_record *route = &impl->routes[index];
        const size_t lca = least_common_ancestor(
            impl, rows[index].source_index, rows[index].target_index);
        size_t node = rows[index].source_index;
        size_t entry_end;
        route->exit_offset = cursor;
        while (node != lca) {
            impl->route_states[cursor++] = impl->states[node].id;
            ++route->exit_count;
            node = impl->parents[node];
        }
        route->entry_offset = cursor;
        route->entry_count = impl->depths[rows[index].target_index] -
                             impl->depths[lca];
        entry_end = cursor + route->entry_count;
        node = rows[index].target_index;
        while (node != lca) {
            impl->route_states[--entry_end] = impl->states[node].id;
            node = impl->parents[node];
        }
        cursor += route->entry_count;
    }
    impl->route_state_count = total;
    impl->route_count = count;
    return CFLOW_MACHINE_HIERARCHY_OK;
}

static cflow_machine_hierarchy_status build_flat_machine(
    cflow_machine_hierarchy_impl *impl,
    const cflow_machine_hierarchy_definition *definition,
    const hierarchy_expanded_transition *rows,
    size_t row_count) {
    cflow_machine_state *states;
    cflow_machine_transition *transitions;
    cflow_machine_definition flat_definition;
    const size_t initial_node = find_state_index(impl,
                                                  definition->initial_state);
    const size_t initial_leaf = initial_node == SIZE_MAX
        ? SIZE_MAX : resolve_initial_leaf(impl, initial_node);
    size_t state_count = 0u;
    size_t index;
    cflow_machine_status status;

    if (initial_leaf == SIZE_MAX)
        return CFLOW_MACHINE_HIERARCHY_UNKNOWN_STATE;
    states = (cflow_machine_state *)calloc(impl->state_count, sizeof(*states));
    transitions = row_count != 0u
        ? (cflow_machine_transition *)calloc(row_count, sizeof(*transitions))
        : NULL;
    if (states == NULL || (row_count != 0u && transitions == NULL)) {
        free(transitions);
        free(states);
        return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    }
    for (index = 0u; index < impl->state_count; ++index) {
        if (impl->states[index].initial_child != 0u) continue;
        states[state_count++] = (cflow_machine_state){
            impl->states[index].id,
            impl->states[index].value_type,
            impl->states[index].kind};
    }
    for (index = 0u; index < row_count; ++index)
        transitions[index] = rows[index].transition;
    flat_definition = (cflow_machine_definition){
        states, state_count, impl->states[initial_leaf].id,
        definition->events, definition->event_count,
        definition->guards, definition->guard_count,
        definition->actions, definition->action_count,
        transitions, row_count};
    status = cflow_machine_build(&impl->flat, &flat_definition);
    free(transitions);
    free(states);
    if (status == CFLOW_MACHINE_ALLOCATION_FAILED)
        return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    if (status == CFLOW_MACHINE_LIMIT_EXCEEDED)
        return CFLOW_MACHINE_HIERARCHY_LIMIT_EXCEEDED;
    return status == CFLOW_MACHINE_OK
        ? CFLOW_MACHINE_HIERARCHY_OK
        : CFLOW_MACHINE_HIERARCHY_FLAT_MACHINE_REJECTED;
}

cflow_machine_hierarchy_status cflow_machine_hierarchy_build(
    cflow_machine_hierarchy *out,
    const cflow_machine_hierarchy_definition *definition) {
    cflow_machine_hierarchy_impl *impl;
    hierarchy_expanded_transition *rows = NULL;
    size_t row_count = 0u;
    cflow_machine_hierarchy_status status;

    if (out == NULL || definition == NULL || out->impl != NULL)
        return CFLOW_MACHINE_HIERARCHY_INVALID_ARGUMENT;
    if (definition->state_count == 0u)
        return CFLOW_MACHINE_HIERARCHY_EMPTY;
    if (definition->states == NULL || definition->initial_state == 0u ||
        (definition->event_count != 0u && definition->events == NULL) ||
        (definition->guard_count != 0u && definition->guards == NULL) ||
        (definition->action_count != 0u && definition->actions == NULL) ||
        (definition->transition_count != 0u &&
         definition->transitions == NULL))
        return CFLOW_MACHINE_HIERARCHY_INVALID_ARGUMENT;
    impl = (cflow_machine_hierarchy_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return CFLOW_MACHINE_HIERARCHY_ALLOCATION_FAILED;
    status = copy_states(impl, definition);
    if (status == CFLOW_MACHINE_HIERARCHY_OK) status = validate_tree(impl);
    if (status == CFLOW_MACHINE_HIERARCHY_OK)
        status = expand_transitions(impl, definition, &rows, &row_count);
    if (status == CFLOW_MACHINE_HIERARCHY_OK)
        status = build_flat_machine(impl, definition, rows, row_count);
    if (status == CFLOW_MACHINE_HIERARCHY_OK)
        status = build_routes(impl, rows, row_count);
    free(rows);
    if (status != CFLOW_MACHINE_HIERARCHY_OK) {
        hierarchy_impl_destroy(impl);
        return status;
    }
    out->impl = impl;
    return CFLOW_MACHINE_HIERARCHY_OK;
}

void cflow_machine_hierarchy_destroy(cflow_machine_hierarchy *hierarchy) {
    if (hierarchy == NULL) return;
    hierarchy_impl_destroy((cflow_machine_hierarchy_impl *)hierarchy->impl);
    hierarchy->impl = NULL;
}

const cflow_machine *cflow_machine_hierarchy_flat_machine(
    const cflow_machine_hierarchy *hierarchy) {
    const cflow_machine_hierarchy_impl *impl = hierarchy != NULL
        ? (const cflow_machine_hierarchy_impl *)hierarchy->impl : NULL;
    return impl != NULL ? &impl->flat : NULL;
}

size_t cflow_machine_hierarchy_state_count(
    const cflow_machine_hierarchy *hierarchy) {
    const cflow_machine_hierarchy_impl *impl = hierarchy != NULL
        ? (const cflow_machine_hierarchy_impl *)hierarchy->impl : NULL;
    return impl != NULL ? impl->state_count : 0u;
}

const cflow_machine_hierarchy_state *cflow_machine_hierarchy_state_at(
    const cflow_machine_hierarchy *hierarchy, size_t index) {
    const cflow_machine_hierarchy_impl *impl = hierarchy != NULL
        ? (const cflow_machine_hierarchy_impl *)hierarchy->impl : NULL;
    return impl != NULL && index < impl->state_count
        ? &impl->states[index] : NULL;
}

bool cflow_machine_hierarchy_route_at(
    const cflow_machine_hierarchy *hierarchy,
    size_t flat_transition_index,
    cflow_machine_hierarchy_route *out_route) {
    const cflow_machine_hierarchy_impl *impl = hierarchy != NULL
        ? (const cflow_machine_hierarchy_impl *)hierarchy->impl : NULL;
    const hierarchy_route_record *route;
    if (impl == NULL || out_route == NULL ||
        flat_transition_index >= impl->route_count)
        return false;
    route = &impl->routes[flat_transition_index];
    out_route->exit_states = route->exit_count != 0u
        ? impl->route_states + route->exit_offset : NULL;
    out_route->exit_count = route->exit_count;
    out_route->entry_states = route->entry_count != 0u
        ? impl->route_states + route->entry_offset : NULL;
    out_route->entry_count = route->entry_count;
    return true;
}

static bool hierarchy_scope_active(
    const cflow_machine_hierarchy_impl *hierarchy,
    cflow_machine_state_id current_state,
    cflow_machine_state_id scope) {
    size_t current = find_state_index(hierarchy, current_state);
    const size_t expected = find_state_index(hierarchy, scope);
    if (current == SIZE_MAX || expected == SIZE_MAX) return false;
    if (hierarchy->states[current].kind != CFLOW_MACHINE_STATE_ACTIVE)
        return false;
    while (current != SIZE_MAX) {
        if (current == expected) return true;
        current = hierarchy->parents[current];
    }
    return false;
}

static void hierarchy_transition_committed(
    void *user, size_t normalized_transition_index, bool begin) {
    cflow_machine_hierarchy_instance_impl *instance =
        (cflow_machine_hierarchy_instance_impl *)user;
    cflow_machine_hierarchy_route route = {0};
    if (instance == NULL) return;
    if (begin) {
        cmeta_mutex_lock(&instance->gate);
        return;
    }
    if (normalized_transition_index != SIZE_MAX &&
        cflow_machine_hierarchy_route_at(
            instance->hierarchy, normalized_transition_index, &route)) {
        const cflow_machine *flat = cflow_machine_hierarchy_flat_machine(
            instance->hierarchy);
        const cflow_machine_transition *transition =
            cflow_machine_transition_at(flat, normalized_transition_index);
        const cflow_machine_hierarchy_impl *hierarchy =
            (const cflow_machine_hierarchy_impl *)instance->hierarchy->impl;
        (void)cflow_timer_event_queue_cancel_scopes(
            &instance->timers, route.exit_states, route.exit_count);
        if (transition != NULL) {
            const size_t target = find_state_index(
                hierarchy, transition->target);
            if (target != SIZE_MAX &&
                hierarchy->states[target].kind != CFLOW_MACHINE_STATE_ACTIVE)
                instance->closed = true;
        }
    } else {
        instance->closed = true;
    }
    if (instance->closed)
        (void)cflow_timer_event_queue_close(&instance->timers);
    cmeta_mutex_unlock(&instance->gate);
}

static void hierarchy_instance_impl_destroy(
    cflow_machine_hierarchy_instance_impl *impl) {
    if (impl == NULL) return;
    if (impl->executor != NULL)
        (void)cflow_executor_wait_idle(impl->executor);
    cflow_timer_event_queue_destroy(&impl->timers);
    cflow_machine_instance_destroy(&impl->machine);
    if (impl->gate != NULL) cmeta_mutex_destroy(&impl->gate);
    free(impl);
}

cflow_machine_hierarchy_instance_init_result
cflow_machine_hierarchy_instance_init(
    cflow_machine_hierarchy_instance *instance,
    const cflow_machine_hierarchy_instance_config *config) {
    cflow_machine_hierarchy_instance_init_result result = {
        CFLOW_MACHINE_HIERARCHY_INSTANCE_INVALID_ARGUMENT,
        CFLOW_MACHINE_INSTANCE_OK,
        CFLOW_TIMER_EVENT_OK
    };
    cflow_machine_hierarchy_instance_impl *impl;
    const cflow_machine_hierarchy_impl *hierarchy;
    cflow_machine_instance_config machine_config;
    cflow_timer_event_queue_config timer_config;

    if (instance == NULL || config == NULL || instance->impl != NULL ||
        config->hierarchy == NULL || config->hierarchy->impl == NULL ||
        config->clock == NULL || config->timer_capacity == 0u)
        return result;
    hierarchy = (const cflow_machine_hierarchy_impl *)config->hierarchy->impl;
    impl = (cflow_machine_hierarchy_instance_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) {
        result.status = CFLOW_MACHINE_HIERARCHY_INSTANCE_ALLOCATION_FAILED;
        return result;
    }
    impl->hierarchy = config->hierarchy;
    impl->executor = config->executor;
    cmeta_mutex_init(&impl->gate);
    if (impl->gate == NULL) {
        hierarchy_instance_impl_destroy(impl);
        result.status = CFLOW_MACHINE_HIERARCHY_INSTANCE_ALLOCATION_FAILED;
        return result;
    }
    machine_config = (cflow_machine_instance_config){
        &hierarchy->flat,
        config->initial_state,
        config->output_type,
        config->guards,
        config->guard_count,
        config->actions,
        config->action_count,
        config->mailbox_capacity,
        config->executor
    };
    result.machine_status = cflow_machine_instance_init_internal(
        &impl->machine, &machine_config,
        hierarchy_transition_committed, impl, NULL, NULL);
    if (result.machine_status != CFLOW_MACHINE_INSTANCE_OK) {
        result.status = CFLOW_MACHINE_HIERARCHY_INSTANCE_MACHINE_REJECTED;
        hierarchy_instance_impl_destroy(impl);
        return result;
    }
    timer_config = (cflow_timer_event_queue_config){
        config->clock, &impl->machine, config->timer_capacity
    };
    result.timer_status = cflow_timer_event_queue_init(
        &impl->timers, &timer_config);
    if (result.timer_status != CFLOW_TIMER_EVENT_OK) {
        result.status = CFLOW_MACHINE_HIERARCHY_INSTANCE_TIMER_REJECTED;
        hierarchy_instance_impl_destroy(impl);
        return result;
    }
    {
        const size_t initial = find_state_index(
            hierarchy,
            cflow_machine_instance_current_state(&impl->machine));
        if (initial != SIZE_MAX &&
            hierarchy->states[initial].kind != CFLOW_MACHINE_STATE_ACTIVE) {
            impl->closed = true;
            (void)cflow_timer_event_queue_close(&impl->timers);
        }
    }
    instance->impl = impl;
    result.status = CFLOW_MACHINE_HIERARCHY_INSTANCE_OK;
    return result;
}

cflow_mailbox_status cflow_machine_hierarchy_instance_try_send(
    cflow_machine_hierarchy_instance *instance,
    const cflow_event_view *event) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    return impl != NULL
        ? cflow_machine_instance_try_send(&impl->machine, event)
        : CFLOW_MAILBOX_INVALID_ARGUMENT;
}

bool cflow_machine_hierarchy_instance_as_resumable(
    cflow_machine_hierarchy_instance *instance,
    cflow_resumable *out) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    return impl != NULL &&
           cflow_machine_instance_as_resumable(&impl->machine, out);
}

bool cflow_machine_hierarchy_instance_as_publisher(
    cflow_machine_hierarchy_instance *instance,
    cflow_publisher *out) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    return impl != NULL && cflow_machine_instance_as_publisher(&impl->machine, out);
}

static bool hierarchy_instance_scope_active_locked(
    cflow_machine_hierarchy_instance_impl *impl,
    cflow_machine_state_id scope) {
    const cflow_machine_hierarchy_impl *hierarchy =
        (const cflow_machine_hierarchy_impl *)impl->hierarchy->impl;
    const cflow_machine_state_id current =
        cflow_machine_instance_current_state(&impl->machine);
    return hierarchy_scope_active(hierarchy, current, scope);
}

cflow_timer_event_schedule_result
cflow_machine_hierarchy_instance_try_schedule_at(
    cflow_machine_hierarchy_instance *instance,
    cflow_machine_state_id scope,
    cflow_deadline deadline,
    const cflow_event_view *event) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    cflow_timer_event_schedule_result result = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u
    };
    if (impl == NULL || scope == 0u) return result;
    cmeta_mutex_lock(&impl->gate);
    if (impl->closed) {
        result.status = CFLOW_TIMER_EVENT_CLOSED;
    } else if (hierarchy_instance_scope_active_locked(impl, scope)) {
        result = cflow_timer_event_queue_try_schedule_scoped_at(
            &impl->timers, deadline, event, scope);
    }
    cmeta_mutex_unlock(&impl->gate);
    return result;
}

cflow_timer_event_schedule_result
cflow_machine_hierarchy_instance_try_schedule_after(
    cflow_machine_hierarchy_instance *instance,
    cflow_machine_state_id scope,
    cflow_duration delay,
    const cflow_event_view *event) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    cflow_timer_event_schedule_result result = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u
    };
    if (impl == NULL || scope == 0u) return result;
    cmeta_mutex_lock(&impl->gate);
    if (impl->closed) {
        result.status = CFLOW_TIMER_EVENT_CLOSED;
    } else if (hierarchy_instance_scope_active_locked(impl, scope)) {
        result = cflow_timer_event_queue_try_schedule_scoped_after(
            &impl->timers, delay, event, scope);
    }
    cmeta_mutex_unlock(&impl->gate);
    return result;
}

cflow_timer_event_status cflow_machine_hierarchy_instance_cancel_timer(
    cflow_machine_hierarchy_instance *instance,
    cflow_timer_event_id timer_id) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    cflow_timer_event_status result;
    if (impl == NULL) return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->gate);
    result = impl->closed
        ? CFLOW_TIMER_EVENT_CLOSED
        : cflow_timer_event_queue_cancel(&impl->timers, timer_id);
    cmeta_mutex_unlock(&impl->gate);
    return result;
}

cflow_timer_event_fire_result
cflow_machine_hierarchy_instance_run_one_ready(
    cflow_machine_hierarchy_instance *instance) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    cflow_timer_event_fire_result result = {
        CFLOW_TIMER_EVENT_FIRE_INVALID_ARGUMENT, 0u,
        CFLOW_MAILBOX_INVALID_ARGUMENT
    };
    if (impl == NULL) return result;
    cmeta_mutex_lock(&impl->gate);
    if (impl->closed) {
        result.status = CFLOW_TIMER_EVENT_FIRE_CLOSED;
    } else {
        result = cflow_timer_event_queue_run_one_ready(&impl->timers);
    }
    cmeta_mutex_unlock(&impl->gate);
    return result;
}

void cflow_machine_hierarchy_instance_close(
    cflow_machine_hierarchy_instance *instance) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    bool first_close;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->gate);
    first_close = !impl->closed;
    impl->closed = true;
    cmeta_mutex_unlock(&impl->gate);
    if (!first_close) return;
    (void)cflow_timer_event_queue_close(&impl->timers);
    cflow_machine_instance_close(&impl->machine);
}

void cflow_machine_hierarchy_instance_cancel(
    cflow_machine_hierarchy_instance *instance) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    bool first_cancel;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->gate);
    first_cancel = !impl->cancelled;
    impl->closed = true;
    impl->cancelled = true;
    cmeta_mutex_unlock(&impl->gate);
    if (!first_cancel) return;
    (void)cflow_timer_event_queue_close(&impl->timers);
    cflow_machine_instance_cancel(&impl->machine);
}

bool cflow_machine_hierarchy_instance_copy_state(
    const cflow_machine_hierarchy_instance *instance,
    const cmeta_type_desc **out_type,
    void *out_value,
    size_t out_value_capacity) {
    const cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (const cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    return impl != NULL && cflow_machine_instance_copy_state(
        &impl->machine, out_type, out_value, out_value_capacity);
}

cflow_machine_state_id cflow_machine_hierarchy_instance_current_state(
    const cflow_machine_hierarchy_instance *instance) {
    const cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (const cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    return impl != NULL
        ? cflow_machine_instance_current_state(&impl->machine) : 0u;
}

bool cflow_machine_hierarchy_instance_get_stats(
    const cflow_machine_hierarchy_instance *instance,
    cflow_machine_hierarchy_instance_stats *out) {
    cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    bool valid;
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->gate);
    valid = cflow_machine_instance_get_stats(&impl->machine, &out->machine) &&
            cflow_timer_event_queue_get_stats(&impl->timers, &out->timers);
    cmeta_mutex_unlock(&impl->gate);
    return valid;
}

const char *cflow_machine_hierarchy_instance_error(
    const cflow_machine_hierarchy_instance *instance) {
    const cflow_machine_hierarchy_instance_impl *impl = instance != NULL
        ? (const cflow_machine_hierarchy_instance_impl *)instance->impl : NULL;
    return impl != NULL ? cflow_machine_instance_error(&impl->machine) : NULL;
}

void cflow_machine_hierarchy_instance_destroy(
    cflow_machine_hierarchy_instance *instance) {
    cflow_machine_hierarchy_instance_impl *impl;
    if (instance == NULL || instance->impl == NULL) return;
    impl = (cflow_machine_hierarchy_instance_impl *)instance->impl;
    cflow_machine_hierarchy_instance_close(instance);
    hierarchy_instance_impl_destroy(impl);
    instance->impl = NULL;
}
