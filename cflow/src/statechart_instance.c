#include "statechart_instance_internal.h"

#include "executor_internal.h"
#include "event_internal.h"
#include "statechart_internal.h"
#include "value_storage.h"

#include <thread.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct statechart_configuration_buffer {
    unsigned char *bits;
    size_t *states;
    size_t state_count;
} statechart_configuration_buffer;

typedef struct statechart_internal_event_slot {
    size_t type_index;
    uint64_t origin_token;
} statechart_internal_event_slot;

typedef enum statechart_terminal_outcome {
    STATECHART_TERMINAL_NONE = 0,
    STATECHART_TERMINAL_CLEAN_DONE,
    STATECHART_TERMINAL_CLOSE,
    STATECHART_TERMINAL_CANCEL,
    STATECHART_TERMINAL_ERROR
} statechart_terminal_outcome;

typedef struct cflow_statechart_instance_impl {
    const cflow_statechart *statechart;
    const cflow_statechart_impl *ir;
    cflow_executor *executor;
    cflow_mailbox external_mailbox;
    bool external_mailbox_initialized;
    uint64_t *external_origin_tokens;
    size_t external_source_head;
    size_t external_source_tail;
    size_t external_event_capacity;
    cflow_mailbox adapter_internal_mailbox;
    bool adapter_internal_mailbox_initialized;
    cflow_timer_event_queue timers;
    bool timers_initialized;
    cflow_statechart_guard_binding *guards;
    size_t guard_count;
    cflow_statechart_executable_binding *executables;
    size_t executable_count;
    statechart_configuration_buffer configurations[2];
    unsigned char *history_bits[2];
    size_t *history_counts[2];
    size_t *history_slots;
    size_t history_count;
    unsigned char *extended_states[2];
    bool extended_state_live[2];
    size_t published;
    size_t bitset_bytes;
    size_t *entry_stack;
    size_t *path_stack;
    size_t *exit_order;
    cflow_machine_state_id *timer_exit_scopes;
    size_t *entry_order;
    size_t *pseudo_transition_order;
    size_t *pseudo_transition_by_state;
    cflow_statechart_transition_id *selected_transition_ids;
    size_t *selected_transition_indices;
    size_t *selected_sources;
    size_t *selected_leaf_orders;
    unsigned char *selected_exit_sets;
    unsigned char *candidate_exit_set;
    unsigned char *candidate_seen;
    size_t selected_count;
    cflow_statechart_selection_trigger selection_trigger;
    cflow_event_view selection_event;
    unsigned char *selection_event_payload;
    uint64_t instance_token;
    uint64_t selection_generation;
    uint64_t selected_configuration_version;
    bool selection_in_progress;
    bool selection_valid;
    bool selection_consumed;
    size_t *request_transition_indices;
    unsigned char *request_exit_sets;
    size_t request_transition_count;
    cflow_statechart_selection_trigger request_trigger;
    cflow_event_view request_event;
    unsigned char *request_event_payload;
    unsigned char *exit_union;
    unsigned char *entry_bits;
    unsigned char *action_configuration_bits;
    unsigned char *action_scratch;
    bool action_scratch_live;
    statechart_internal_event_slot *staged_event_slots;
    unsigned char *staged_event_payloads;
    size_t staged_event_count;
    cflow_statechart_effect_ticket *staged_effects;
    size_t staged_effect_count;
    size_t effect_capacity;
    statechart_internal_event_slot *internal_event_slots;
    unsigned char *internal_event_payloads;
    size_t internal_event_head;
    size_t internal_event_count;
    size_t internal_event_capacity;
    size_t event_payload_stride;
    size_t adapter_internal_pending;
    uint64_t adapter_internal_accepted;
    unsigned char *driver_event_payload;
    size_t driver_event_capacity;
    size_t *completion_rows;
    size_t completion_head;
    size_t completion_count;
    size_t completion_capacity;
    unsigned char *completion_bits[2];
    unsigned char *completion_work;
    size_t *staged_completion_rows;
    size_t staged_completion_count;
    size_t microstep_limit;
    size_t macrostep_microsteps;
    bool initial_configuration_pending;
    bool macrostep_active;
    bool macrostep_has_external;
    bool external_in_flight;
    uint64_t external_in_flight_origin_token;
    bool external_in_flight_dropped;
    cflow_statechart_external_settlement pending_external_settlement;
    bool pending_external_settlement_ready;
    bool external_settlement_delivery_active;
    size_t cancelled_external_settlement_head;
    size_t cancelled_external_settlement_count;
    bool driver_scheduled;
    bool driver_repost;
    bool driver_after_microstep;
    bool root_completion_draining;
    bool skip_to_external;
    bool microstep_pending;
    size_t task_reservations;
    cflow_statechart_instance_status microstep_result;
    uint64_t microstep_accepted;
    uint64_t microstep_completed;
    uint64_t microstep_failed;
    uint64_t microstep_cancelled;
    uint64_t microstep_finalized;
    char *error_storage;
    /* External transfer/accounting order is this lock, then mailbox lock.
       The private mailbox is never armed with a waker. */
    cmeta_mutex_t lock;
    cmeta_cond_t tasks_changed;
    const char *error;
    bool error_owned;
    uint64_t configuration_version;
    uint64_t macrosteps;
    uint64_t microsteps;
    uint64_t actions;
    uint64_t external_accepted;
    uint64_t external_completed;
    uint64_t external_failed;
    uint64_t external_cancelled;
    size_t external_pending;
    cflow_statechart_instance_status last_status;
    statechart_terminal_outcome terminal_outcome;
    bool closed;
    bool cancelled;
    bool exit_requested;
    bool done;
    bool adapter_attached;
    cflow_statechart_instance_hooks hooks;
    void *hook_user;
    cflow_waker downstream_waiter;
    cflow_waker terminal_waiter;
    cflow_statechart_instance_test_hooks test_hooks;
} cflow_statechart_instance_impl;

static atomic_uint_fast64_t statechart_instance_token_source =
    UINT64_C(1);

static cflow_admission_status schedule_statechart_driver(
    cflow_statechart_instance_impl *impl);
static cflow_admission_status schedule_statechart_driver_reserved(
    cflow_statechart_instance_impl *impl);
static void latch_terminal_failure(cflow_statechart_instance_impl *impl,
                                   cflow_statechart_instance_status status,
                                   const char *message);
static void discard_staged_effects(cflow_statechart_instance_impl *impl);
static void commit_internal_events(cflow_statechart_instance_impl *impl);
static cflow_statechart_host_result run_host_transaction_runtime_hook(
    cflow_statechart_instance_impl *impl,
    cflow_statechart_host_phase phase,
    const cflow_statechart_observed_event *trigger);

static bool acquire_instance_token(uint64_t *out) {
    uint_fast64_t current;
    if (out == NULL) return false;
    current = atomic_load_explicit(
        &statechart_instance_token_source, memory_order_relaxed);
    while (current != UINT_FAST64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &statechart_instance_token_source, &current, current + 1u,
                memory_order_relaxed, memory_order_relaxed)) {
            *out = (uint64_t)current;
            return true;
        }
    }
    return false;
}

static bool checked_add(size_t left, size_t right, size_t *out) {
    if (out == NULL || left > SIZE_MAX - right) return false;
    *out = left + right;
    return true;
}

static bool checked_multiply(size_t left, size_t right, size_t *out) {
    if (out == NULL || (right != 0u && left > SIZE_MAX / right)) return false;
    *out = left * right;
    return true;
}

static bool checked_accumulate(size_t value, size_t *total) {
    return checked_add(*total, value, total);
}

static bool instance_hooks_shape_valid(
    const cflow_statechart_instance_hooks *hooks) {
    const size_t v4_size = offsetof(
        cflow_statechart_instance_hooks, on_external_settlement);
    if (hooks == NULL) return true;
    if (hooks->abi_version == CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4)
        return hooks->struct_size >= v4_size &&
            hooks->on_host_transaction != NULL;
    if (hooks->abi_version == CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V5)
        return hooks->struct_size >= sizeof(*hooks) &&
            (hooks->on_host_transaction != NULL ||
             hooks->on_external_settlement != NULL);
    return false;
}

static void copy_instance_hooks(
    cflow_statechart_instance_hooks *destination,
    const cflow_statechart_instance_hooks *source) {
    const size_t v4_size = offsetof(
        cflow_statechart_instance_hooks, on_external_settlement);
    memset(destination, 0, sizeof(*destination));
    if (source == NULL) return;
    memcpy(destination, source,
           source->abi_version == CFLOW_STATECHART_INSTANCE_HOOKS_ABI_V4
               ? v4_size : sizeof(*destination));
}

static bool bitset_bytes_for(size_t bit_count, size_t *out) {
    size_t rounded;
    return checked_add(bit_count, 7u, &rounded) && ((*out = rounded / 8u), true);
}

static bool align_size(size_t size, size_t alignment, size_t *out) {
    const size_t padding = alignment - 1u;
    return out != NULL && size != 0u && alignment != 0u &&
        (alignment & padding) == 0u && size <= SIZE_MAX - padding &&
        ((*out = (size + padding) & ~padding), true);
}

static cflow_statechart_instance_status measure_event_storage(
    const cflow_statechart_impl *ir, size_t *out_stride) {
    size_t max_size = 0u, max_align = 1u, index;
    if (ir == NULL || out_stride == NULL)
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    for (index = 0u; index < ir->event_count; ++index) {
        const cmeta_type_desc *type = ir->events[index].payload_type;
        if (!cmeta_type_desc_valid(type) || type->size == 0u ||
            type->align == 0u ||
            (type->align & (type->align - 1u)) != 0u ||
            type->align > _Alignof(cmeta_capture_storage) ||
            !cflow_value_storage_type_supported(type))
            return CFLOW_STATECHART_INSTANCE_UNSUPPORTED_TYPE;
        if (type->size > max_size) max_size = type->size;
        if (type->align > max_align) max_align = type->align;
    }
    if (ir->event_count == 0u) {
        *out_stride = 0u;
        return CFLOW_STATECHART_INSTANCE_OK;
    }
    return align_size(max_size, max_align, out_stride)
        ? CFLOW_STATECHART_INSTANCE_OK
        : CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
}

static bool bit_test(const unsigned char *bits, size_t index) {
    return (bits[index / 8u] & (unsigned char)(1u << (index % 8u))) != 0u;
}

static void bit_set(unsigned char *bits, size_t index) {
    bits[index / 8u] |= (unsigned char)(1u << (index % 8u));
}

static void bit_clear(unsigned char *bits, size_t index) {
    bits[index / 8u] &= (unsigned char)~(1u << (index % 8u));
}

static bool pseudo_kind(cflow_statechart_state_kind kind) {
    return kind == CFLOW_STATECHART_INITIAL ||
           kind == CFLOW_STATECHART_HISTORY_SHALLOW ||
           kind == CFLOW_STATECHART_HISTORY_DEEP;
}

static bool leaf_kind(cflow_statechart_state_kind kind) {
    return kind == CFLOW_STATECHART_ATOMIC || kind == CFLOW_STATECHART_FINAL;
}

static cflow_statechart_instance_status normalize_internal_event_capacity(
    size_t requested, size_t *out) {
    const size_t effective = requested != 0u
        ? requested
        : (size_t)CFLOW_STATECHART_DEFAULT_INTERNAL_EVENT_CAPACITY;
    if (out == NULL) return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    if (effective == 0u) return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    *out = effective;
    return CFLOW_STATECHART_INSTANCE_OK;
}

static cflow_statechart_instance_status calculate_storage_requirements(
    const cflow_statechart_impl *ir, size_t external_event_capacity,
    size_t internal_event_capacity, size_t completion_capacity,
    cflow_statechart_storage_requirements *out) {
    cflow_statechart_storage_requirements requirements = {0};
    size_t bitset_bytes, transition_bitset_bytes, state_index_bytes;
    size_t selection_exit_bytes, selection_id_bytes;
    size_t history_count = 0u;
    size_t history_table_bytes, one_binding_kind, timer_scope_bytes, index;
    size_t event_stride, event_slot_bytes, event_payload_bytes;
    cflow_statechart_instance_status event_status;
    event_status = measure_event_storage(ir, &event_stride);
    if (event_status != CFLOW_STATECHART_INSTANCE_OK) return event_status;
    if (ir == NULL || out == NULL ||
        !bitset_bytes_for(ir->state_count, &bitset_bytes) ||
        !bitset_bytes_for(ir->transition_count, &transition_bitset_bytes) ||
        !checked_multiply(ir->state_count, sizeof(size_t),
                          &state_index_bytes) ||
        !checked_multiply(ir->state_count,
                          sizeof(cflow_statechart_transition_id),
                          &selection_id_bytes) ||
        !checked_multiply(ir->state_count,
                          sizeof(cflow_machine_state_id),
                          &timer_scope_bytes) ||
        !checked_multiply(ir->state_count, bitset_bytes,
                          &selection_exit_bytes))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    for (index = 0u; index < ir->state_count; ++index) {
        const cflow_statechart_state_kind kind = ir->states[index].kind;
        if (kind == CFLOW_STATECHART_HISTORY_SHALLOW ||
            kind == CFLOW_STATECHART_HISTORY_DEEP)
            ++history_count;
    }

    requirements.external_event_capacity = external_event_capacity;
    requirements.internal_event_capacity = internal_event_capacity;
    requirements.completion_capacity = completion_capacity;
    requirements.control_bytes = sizeof(cflow_statechart_instance_impl);
    if (!checked_multiply(ir->guard_count,
                          sizeof(cflow_statechart_guard_binding),
                          &requirements.binding_bytes) ||
        !checked_multiply(ir->executable_count,
                          sizeof(cflow_statechart_executable_binding),
                          &one_binding_kind) ||
        !checked_accumulate(one_binding_kind, &requirements.binding_bytes) ||
        !checked_add(bitset_bytes, state_index_bytes,
                     &requirements.configuration_bytes) ||
        !checked_multiply(requirements.configuration_bytes, 2u,
                          &requirements.configuration_bytes) ||
        !checked_multiply(history_count, bitset_bytes,
                          &history_table_bytes) ||
        !checked_multiply(history_table_bytes, 2u,
                          &requirements.history_bitset_bytes) ||
        !checked_multiply(history_count, sizeof(size_t),
                          &requirements.history_count_bytes) ||
        !checked_multiply(requirements.history_count_bytes, 2u,
                          &requirements.history_count_bytes) ||
        !checked_multiply(ir->state_type->size, 2u,
                          &requirements.extended_state_bytes) ||
        !checked_multiply(state_index_bytes, 11u,
                          &requirements.index_work_bytes) ||
        !checked_accumulate(timer_scope_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(selection_id_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(selection_exit_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(selection_exit_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(bitset_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(bitset_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(bitset_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(bitset_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate(transition_bitset_bytes,
                            &requirements.index_work_bytes) ||
        !checked_accumulate((size_t)CFLOW_STATECHART_ERROR_CAPACITY,
                            &requirements.index_work_bytes))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;

    requirements.action_scratch_bytes = ir->state_type->size;
    if (ir->event_count != 0u) {
        if (!checked_multiply(internal_event_capacity,
                              sizeof(statechart_internal_event_slot),
                              &event_slot_bytes) ||
            !checked_multiply(internal_event_capacity, event_stride,
                              &event_payload_bytes) ||
            !checked_multiply(event_slot_bytes, 2u,
                              &requirements.internal_event_bytes) ||
            !checked_multiply(event_payload_bytes, 2u,
                              &one_binding_kind) ||
            !checked_accumulate(one_binding_kind,
                                &requirements.internal_event_bytes) ||
            !checked_multiply(event_stride, 3u, &one_binding_kind) ||
            !checked_accumulate(one_binding_kind,
                                &requirements.internal_event_bytes))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
        if (!cflow_mailbox_storage_requirements_internal(
                ir->events, ir->event_count, external_event_capacity,
                &requirements.external_event_bytes))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    }
    if (!checked_multiply(completion_capacity, sizeof(size_t),
                          &requirements.completion_bytes) ||
        !checked_multiply(requirements.completion_bytes, 2u,
                          &requirements.completion_bytes) ||
        !checked_multiply(bitset_bytes, 3u, &one_binding_kind) ||
        !checked_accumulate(one_binding_kind,
                            &requirements.completion_bytes))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;

    requirements.total_bytes = requirements.control_bytes;
    if (!checked_accumulate(requirements.binding_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.configuration_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.history_bitset_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.history_count_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.extended_state_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.index_work_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.action_scratch_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.internal_event_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.external_event_bytes,
                            &requirements.total_bytes) ||
        !checked_accumulate(requirements.completion_bytes,
                            &requirements.total_bytes))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    *out = requirements;
    return CFLOW_STATECHART_INSTANCE_OK;
}

static cflow_statechart_instance_status storage_requirements_for_ir(
    const cflow_statechart_impl *ir, size_t external_event_capacity,
    size_t requested_internal_event_capacity, size_t completion_capacity,
    cflow_statechart_storage_requirements *out) {
    cflow_statechart_storage_requirements requirements;
    size_t effective_internal_event_capacity;
    cflow_statechart_instance_status status =
        normalize_internal_event_capacity(
            requested_internal_event_capacity,
            &effective_internal_event_capacity);
    if (status != CFLOW_STATECHART_INSTANCE_OK) return status;
    status = calculate_storage_requirements(
        ir, external_event_capacity, effective_internal_event_capacity,
        completion_capacity, &requirements);
    if (status != CFLOW_STATECHART_INSTANCE_OK) return status;
    if (requirements.total_bytes >
        (size_t)CFLOW_STATECHART_MAX_INSTANCE_BYTES)
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    *out = requirements;
    return CFLOW_STATECHART_INSTANCE_OK;
}

cflow_statechart_instance_status
cflow_statechart_instance_storage_requirements_internal(
    const cflow_statechart *statechart,
    size_t external_event_capacity,
    size_t internal_event_capacity,
    size_t completion_capacity,
    cflow_statechart_storage_requirements *out) {
    const cflow_statechart_impl *ir = cflow_statechart_internal_get(statechart);
    if (ir == NULL || out == NULL)
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    if (external_event_capacity == 0u || completion_capacity == 0u)
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    return storage_requirements_for_ir(
        ir, external_event_capacity, internal_event_capacity,
        completion_capacity, out);
}

static size_t find_state_index(const cflow_statechart_impl *ir,
                               cflow_machine_state_id id) {
    size_t left = 0u;
    size_t right = ir->state_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        const cflow_machine_state_id current = ir->states[middle].id;
        if (current == id) return middle;
        if (current < id) left = middle + 1u;
        else right = middle;
    }
    return SIZE_MAX;
}

static int compare_guard_binding(const void *left, const void *right) {
    const cflow_statechart_guard_binding *a =
        (const cflow_statechart_guard_binding *)left;
    const cflow_statechart_guard_binding *b =
        (const cflow_statechart_guard_binding *)right;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int compare_executable_binding(const void *left, const void *right) {
    const cflow_statechart_executable_binding *a =
        (const cflow_statechart_executable_binding *)left;
    const cflow_statechart_executable_binding *b =
        (const cflow_statechart_executable_binding *)right;
    return a->id < b->id ? -1 : a->id > b->id;
}

static void reset_state_value(cflow_statechart_instance_impl *impl,
                              unsigned char *storage, bool *live) {
    if (impl == NULL || storage == NULL || live == NULL || !*live) return;
    cflow_value_destroy(impl->ir->state_type, storage);
    *live = false;
}

static bool copy_state_value(cflow_statechart_instance_impl *impl,
                             unsigned char *destination,
                             bool *destination_live,
                             const unsigned char *source) {
    reset_state_value(impl, destination, destination_live);
    if (!cflow_value_construct(
            impl->ir->state_type, destination, source))
        return false;
    *destination_live = true;
    return true;
}

static void reset_transaction_state(cflow_statechart_instance_impl *impl,
                                    size_t staged) {
    reset_state_value(
        impl, impl->extended_states[staged],
        &impl->extended_state_live[staged]);
    reset_state_value(
        impl, impl->action_scratch, &impl->action_scratch_live);
}

static void instance_impl_free(cflow_statechart_instance_impl *impl) {
    size_t index;
    if (impl == NULL) return;
    for (index = 0u; index < impl->staged_effect_count; ++index)
        impl->staged_effects[index].discard(
            impl->staged_effects[index].user);
    impl->staged_effect_count = 0u;
    reset_state_value(
        impl, impl->action_scratch, &impl->action_scratch_live);
    for (index = 0u; index < 2u; ++index)
        reset_state_value(
            impl, impl->extended_states[index],
            &impl->extended_state_live[index]);
    if (impl->timers_initialized)
        cflow_timer_event_queue_destroy(&impl->timers);
    if (impl->external_mailbox_initialized)
        cflow_mailbox_destroy(&impl->external_mailbox);
    if (impl->adapter_internal_mailbox_initialized)
        cflow_mailbox_destroy(&impl->adapter_internal_mailbox);
    if (impl->tasks_changed != NULL)
        cmeta_cond_destroy(&impl->tasks_changed);
    if (impl->lock != NULL) cmeta_mutex_destroy(&impl->lock);
    if (impl->error_owned) free((void *)impl->error);
    free(impl->internal_event_payloads);
    free(impl->external_origin_tokens);
    free(impl->internal_event_slots);
    free(impl->staged_event_payloads);
    free(impl->staged_event_slots);
    free(impl->staged_effects);
    free(impl->driver_event_payload);
    free(impl->completion_rows);
    free(impl->completion_work);
    free(impl->staged_completion_rows);
    free(impl->action_scratch);
    free(impl->action_configuration_bits);
    free(impl->entry_bits);
    free(impl->exit_union);
    free(impl->selection_event_payload);
    free(impl->request_event_payload);
    free(impl->request_exit_sets);
    free(impl->request_transition_indices);
    free(impl->error_storage);
    free(impl->candidate_seen);
    free(impl->candidate_exit_set);
    free(impl->selected_exit_sets);
    free(impl->selected_leaf_orders);
    free(impl->selected_sources);
    free(impl->selected_transition_indices);
    free(impl->selected_transition_ids);
    free(impl->path_stack);
    free(impl->entry_stack);
    free(impl->pseudo_transition_order);
    free(impl->pseudo_transition_by_state);
    free(impl->entry_order);
    free(impl->exit_order);
    free(impl->timer_exit_scopes);
    for (index = 0u; index < 2u; ++index) {
        free(impl->completion_bits[index]);
        free(impl->extended_states[index]);
        free(impl->history_counts[index]);
        free(impl->history_bits[index]);
        free(impl->configurations[index].states);
        free(impl->configurations[index].bits);
    }
    free(impl->history_slots);
    free(impl->executables);
    free(impl->guards);
    free(impl);
}

static bool reserve_instance_task_locked(
    cflow_statechart_instance_impl *impl) {
    if (impl->task_reservations == SIZE_MAX) return false;
    ++impl->task_reservations;
    return true;
}

static void release_instance_task_locked(
    cflow_statechart_instance_impl *impl) {
    if (impl->task_reservations != 0u) --impl->task_reservations;
    if (impl->task_reservations == 0u)
        cmeta_cond_broadcast(&impl->tasks_changed);
}

static cflow_statechart_instance_status wait_instance_tasks(
    cflow_statechart_instance_impl *impl) {
    if (cflow_executor_is_current_internal(impl->executor))
        return CFLOW_STATECHART_INSTANCE_WOULD_BLOCK;
    cmeta_mutex_lock(&impl->lock);
    while (impl->task_reservations != 0u)
        cmeta_cond_wait(&impl->tasks_changed, &impl->lock);
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_STATECHART_INSTANCE_OK;
}

static bool binding_rows_shape_valid(
    const cflow_statechart_impl *ir,
    const cflow_statechart_instance_config *config) {
    return config->guard_count == ir->guard_count &&
        config->executable_count == ir->executable_count &&
        (config->guard_count == 0u || config->guards != NULL) &&
        (config->executable_count == 0u || config->executables != NULL);
}

static cflow_statechart_instance_status copy_bindings(
    cflow_statechart_instance_impl *impl,
    const cflow_statechart_instance_config *config) {
    size_t index, bytes;
    if (!binding_rows_shape_valid(impl->ir, config))
        return CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH;

    if (config->guard_count != 0u) {
        if (!checked_multiply(config->guard_count, sizeof(*impl->guards),
                              &bytes))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
        impl->guards = (cflow_statechart_guard_binding *)malloc(bytes);
        if (impl->guards == NULL)
            return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
        memcpy(impl->guards, config->guards, bytes);
        if (config->guard_count > 1u)
            qsort(impl->guards, config->guard_count, sizeof(*impl->guards),
                  compare_guard_binding);
        for (index = 0u; index < config->guard_count; ++index) {
            if (impl->guards[index].id != impl->ir->guards[index].id ||
                (impl->guards[index].fn == NULL) ==
                    (impl->guards[index].contextual_fn == NULL))
                return CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH;
        }
    }

    if (config->executable_count != 0u) {
        if (!checked_multiply(config->executable_count,
                              sizeof(*impl->executables), &bytes))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
        impl->executables =
            (cflow_statechart_executable_binding *)malloc(bytes);
        if (impl->executables == NULL)
            return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
        memcpy(impl->executables, config->executables, bytes);
        if (config->executable_count > 1u)
            qsort(impl->executables, config->executable_count,
                  sizeof(*impl->executables), compare_executable_binding);
        for (index = 0u; index < config->executable_count; ++index) {
            if (impl->executables[index].id !=
                    impl->ir->executables[index].id ||
                (impl->executables[index].fn == NULL) ==
                    (impl->executables[index].contextual_fn == NULL))
                return CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH;
        }
    }
    impl->guard_count = config->guard_count;
    impl->executable_count = config->executable_count;
    return CFLOW_STATECHART_INSTANCE_OK;
}

static cflow_statechart_configuration_status validate_dense_configuration(
    const cflow_statechart_impl *ir,
    const unsigned char *bits,
    const size_t *states,
    size_t state_count) {
    size_t position, leaf_count = 0u;
    if (ir == NULL || bits == NULL || states == NULL || state_count == 0u ||
        !bit_test(bits, ir->root))
        return CFLOW_STATECHART_CONFIGURATION_INVALID_ARGUMENT;

    for (position = 0u; position < state_count; ++position) {
        const size_t state = states[position];
        const size_t parent = state < ir->state_count
            ? ir->parents[state] : SIZE_MAX;
        if (state >= ir->state_count)
            return CFLOW_STATECHART_CONFIGURATION_UNKNOWN_STATE;
        if (pseudo_kind(ir->states[state].kind))
            return CFLOW_STATECHART_CONFIGURATION_PSEUDO_STATE;
        if (!bit_test(bits, state))
            return CFLOW_STATECHART_CONFIGURATION_INVALID_ARGUMENT;
        if (position != 0u &&
            ir->states[states[position - 1u]].document_order >=
                ir->states[state].document_order)
            return states[position - 1u] == state
                ? CFLOW_STATECHART_CONFIGURATION_DUPLICATE_STATE
                : CFLOW_STATECHART_CONFIGURATION_INVALID_ORDER;
        if (parent != SIZE_MAX && !bit_test(bits, parent))
            return CFLOW_STATECHART_CONFIGURATION_MISSING_ANCESTOR;
        if (leaf_kind(ir->states[state].kind)) ++leaf_count;
    }

    for (position = 0u; position < state_count; ++position) {
        const size_t state = states[position];
        const cflow_statechart_state_kind kind = ir->states[state].kind;
        size_t child, active_real_children = 0u, real_children = 0u;
        if (kind != CFLOW_STATECHART_COMPOUND &&
            kind != CFLOW_STATECHART_PARALLEL)
            continue;
        for (child = ir->child_offsets[state];
             child < ir->child_offsets[state + 1u]; ++child) {
            const size_t child_state = ir->children[child];
            if (pseudo_kind(ir->states[child_state].kind)) continue;
            ++real_children;
            if (bit_test(bits, child_state)) ++active_real_children;
        }
        if ((kind == CFLOW_STATECHART_COMPOUND &&
             active_real_children != 1u) ||
            (kind == CFLOW_STATECHART_PARALLEL &&
             active_real_children != real_children))
            return CFLOW_STATECHART_CONFIGURATION_WRONG_CHILD;
    }
    return leaf_count != 0u
        ? CFLOW_STATECHART_CONFIGURATION_OK
        : CFLOW_STATECHART_CONFIGURATION_MISSING_LEAF;
}

cflow_statechart_configuration_status
cflow_statechart_configuration_validate_internal(
    const cflow_statechart *statechart,
    const cflow_machine_state_id *states,
    size_t state_count,
    unsigned char *scratch,
    size_t scratch_capacity) {
    const cflow_statechart_impl *ir = cflow_statechart_internal_get(statechart);
    size_t required, index;
    if (ir == NULL || states == NULL || scratch == NULL || state_count == 0u ||
        !bitset_bytes_for(ir->state_count, &required) ||
        scratch_capacity < required)
        return CFLOW_STATECHART_CONFIGURATION_INVALID_ARGUMENT;
    memset(scratch, 0, (ir->state_count + 7u) / 8u);
    for (index = 0u; index < state_count; ++index) {
        const size_t state = find_state_index(ir, states[index]);
        if (state == SIZE_MAX)
            return CFLOW_STATECHART_CONFIGURATION_UNKNOWN_STATE;
        if (pseudo_kind(ir->states[state].kind))
            return CFLOW_STATECHART_CONFIGURATION_PSEUDO_STATE;
        if (bit_test(scratch, state))
            return CFLOW_STATECHART_CONFIGURATION_DUPLICATE_STATE;
        if (index != 0u &&
            ir->states[find_state_index(ir, states[index - 1u])].document_order >=
                ir->states[state].document_order)
            return CFLOW_STATECHART_CONFIGURATION_INVALID_ORDER;
        bit_set(scratch, state);
    }
    if (!bit_test(scratch, ir->root))
        return CFLOW_STATECHART_CONFIGURATION_MISSING_ANCESTOR;
    for (index = 0u; index < state_count; ++index) {
        const size_t state = find_state_index(ir, states[index]);
        const size_t parent = ir->parents[state];
        if (parent != SIZE_MAX && !bit_test(scratch, parent))
            return CFLOW_STATECHART_CONFIGURATION_MISSING_ANCESTOR;
    }
    {
        size_t leaf_count = 0u;
        for (index = 0u; index < state_count; ++index) {
            const size_t state = find_state_index(ir, states[index]);
            const cflow_statechart_state_kind kind = ir->states[state].kind;
            size_t child, active_real_children = 0u, real_children = 0u;
            if (leaf_kind(kind)) ++leaf_count;
            if (kind != CFLOW_STATECHART_COMPOUND &&
                kind != CFLOW_STATECHART_PARALLEL)
                continue;
            for (child = ir->child_offsets[state];
                 child < ir->child_offsets[state + 1u]; ++child) {
                const size_t child_state = ir->children[child];
                if (pseudo_kind(ir->states[child_state].kind)) continue;
                ++real_children;
                if (bit_test(scratch, child_state)) ++active_real_children;
            }
            if ((kind == CFLOW_STATECHART_COMPOUND &&
                 active_real_children != 1u) ||
                (kind == CFLOW_STATECHART_PARALLEL &&
                 active_real_children != real_children))
                return CFLOW_STATECHART_CONFIGURATION_WRONG_CHILD;
        }
        return leaf_count != 0u
            ? CFLOW_STATECHART_CONFIGURATION_OK
            : CFLOW_STATECHART_CONFIGURATION_MISSING_LEAF;
    }
}

static cflow_statechart_instance_status allocate_storage(
    cflow_statechart_instance_impl *impl,
    const cflow_statechart_storage_requirements *requirements) {
    size_t state_bytes, transition_id_bytes, timer_scope_bytes;
    size_t selected_exit_bytes;
    size_t transition_bitset_bytes, history_bytes, history_count_bytes;
    size_t index, history_slot = 0u;
    if (requirements == NULL ||
        !bitset_bytes_for(impl->ir->state_count, &impl->bitset_bytes) ||
        !checked_multiply(impl->ir->state_count, sizeof(size_t),
                          &state_bytes) ||
        !checked_multiply(impl->ir->state_count,
                          sizeof(cflow_statechart_transition_id),
                          &transition_id_bytes) ||
        !checked_multiply(impl->ir->state_count,
                          sizeof(cflow_machine_state_id),
                          &timer_scope_bytes) ||
        !checked_multiply(impl->ir->state_count, impl->bitset_bytes,
                          &selected_exit_bytes) ||
        !bitset_bytes_for(impl->ir->transition_count,
                          &transition_bitset_bytes))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    for (index = 0u; index < impl->ir->state_count; ++index) {
        const cflow_statechart_state_kind kind = impl->ir->states[index].kind;
        if (kind == CFLOW_STATECHART_HISTORY_SHALLOW ||
            kind == CFLOW_STATECHART_HISTORY_DEEP)
            ++impl->history_count;
    }
    history_bytes = requirements->history_bitset_bytes / 2u;
    history_count_bytes = requirements->history_count_bytes / 2u;

    impl->history_slots = (size_t *)malloc(state_bytes);
    impl->entry_stack = (size_t *)malloc(state_bytes);
    impl->path_stack = (size_t *)malloc(state_bytes);
    impl->exit_order = (size_t *)malloc(state_bytes);
    impl->timer_exit_scopes =
        (cflow_machine_state_id *)malloc(timer_scope_bytes);
    impl->entry_order = (size_t *)malloc(state_bytes);
    impl->pseudo_transition_order = (size_t *)malloc(state_bytes);
    impl->pseudo_transition_by_state = (size_t *)malloc(state_bytes);
    impl->selected_transition_ids =
        (cflow_statechart_transition_id *)malloc(transition_id_bytes);
    impl->selected_transition_indices = (size_t *)malloc(state_bytes);
    impl->selected_sources = (size_t *)malloc(state_bytes);
    impl->selected_leaf_orders = (size_t *)malloc(state_bytes);
    impl->selected_exit_sets =
        (unsigned char *)calloc(1u, selected_exit_bytes);
    impl->candidate_exit_set =
        (unsigned char *)calloc(1u, impl->bitset_bytes);
    impl->request_transition_indices = (size_t *)malloc(state_bytes);
    impl->request_exit_sets =
        (unsigned char *)calloc(1u, selected_exit_bytes);
    impl->exit_union = (unsigned char *)calloc(1u, impl->bitset_bytes);
    impl->entry_bits = (unsigned char *)calloc(1u, impl->bitset_bytes);
    impl->action_configuration_bits =
        (unsigned char *)calloc(1u, impl->bitset_bytes);
    impl->action_scratch = (unsigned char *)malloc(
        requirements->action_scratch_bytes);
    impl->completion_rows = (size_t *)malloc(
        requirements->completion_capacity * sizeof(size_t));
    impl->staged_completion_rows = (size_t *)malloc(
        requirements->completion_capacity * sizeof(size_t));
    impl->completion_work = (unsigned char *)calloc(1u, impl->bitset_bytes);
    impl->candidate_seen =
        transition_bitset_bytes != 0u
            ? (unsigned char *)calloc(1u, transition_bitset_bytes)
            : NULL;
    impl->error_storage =
        (char *)malloc((size_t)CFLOW_STATECHART_ERROR_CAPACITY);
    if (impl->effect_capacity != 0u)
        impl->staged_effects = (cflow_statechart_effect_ticket *)malloc(
            impl->effect_capacity * sizeof(*impl->staged_effects));
    if (impl->ir->event_count != 0u) {
        size_t slot_bytes, payload_bytes;
        if (measure_event_storage(impl->ir, &impl->event_payload_stride) !=
                CFLOW_STATECHART_INSTANCE_OK ||
            !checked_multiply(impl->internal_event_capacity,
                              sizeof(*impl->staged_event_slots),
                              &slot_bytes) ||
            !checked_multiply(impl->internal_event_capacity,
                              impl->event_payload_stride, &payload_bytes))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
        impl->staged_event_slots =
            (statechart_internal_event_slot *)malloc(slot_bytes);
        impl->internal_event_slots =
            (statechart_internal_event_slot *)malloc(slot_bytes);
        impl->staged_event_payloads =
            (unsigned char *)malloc(payload_bytes);
        impl->internal_event_payloads =
            (unsigned char *)malloc(payload_bytes);
        impl->request_event_payload =
            (unsigned char *)malloc(impl->event_payload_stride);
        impl->selection_event_payload =
            (unsigned char *)malloc(impl->event_payload_stride);
        impl->driver_event_payload =
            (unsigned char *)malloc(impl->event_payload_stride);
        impl->driver_event_capacity = impl->event_payload_stride;
    }
    for (index = 0u; index < 2u; ++index) {
        impl->completion_bits[index] =
            (unsigned char *)calloc(1u, impl->bitset_bytes);
        impl->configurations[index].bits =
            (unsigned char *)calloc(1u, impl->bitset_bytes);
        impl->configurations[index].states = (size_t *)malloc(state_bytes);
        impl->extended_states[index] =
            (unsigned char *)malloc(impl->ir->state_type->size);
        if (impl->history_count != 0u) {
            impl->history_bits[index] =
                (unsigned char *)calloc(1u, history_bytes);
            impl->history_counts[index] =
                (size_t *)calloc(1u, history_count_bytes);
        }
    }
    if (impl->history_slots == NULL || impl->entry_stack == NULL ||
        impl->path_stack == NULL || impl->exit_order == NULL ||
        impl->timer_exit_scopes == NULL ||
        impl->entry_order == NULL || impl->pseudo_transition_order == NULL ||
        impl->pseudo_transition_by_state == NULL ||
        impl->selected_transition_ids == NULL ||
        impl->selected_transition_indices == NULL ||
        impl->selected_sources == NULL || impl->selected_leaf_orders == NULL ||
        impl->selected_exit_sets == NULL || impl->candidate_exit_set == NULL ||
        impl->request_transition_indices == NULL ||
        impl->request_exit_sets == NULL || impl->exit_union == NULL ||
        impl->entry_bits == NULL ||
        impl->action_configuration_bits == NULL ||
        impl->action_scratch == NULL ||
        impl->completion_rows == NULL ||
        impl->staged_completion_rows == NULL ||
        impl->completion_work == NULL ||
        (transition_bitset_bytes != 0u && impl->candidate_seen == NULL) ||
        impl->error_storage == NULL ||
        (impl->effect_capacity != 0u && impl->staged_effects == NULL) ||
        (impl->ir->event_count != 0u &&
         (impl->staged_event_slots == NULL ||
          impl->internal_event_slots == NULL ||
          impl->staged_event_payloads == NULL ||
          impl->internal_event_payloads == NULL ||
          impl->selection_event_payload == NULL ||
          impl->request_event_payload == NULL ||
          impl->driver_event_payload == NULL)) ||
        impl->completion_bits[0] == NULL ||
        impl->completion_bits[1] == NULL ||
        impl->configurations[0].bits == NULL ||
        impl->configurations[1].bits == NULL ||
        impl->configurations[0].states == NULL ||
        impl->configurations[1].states == NULL ||
        impl->extended_states[0] == NULL ||
        impl->extended_states[1] == NULL ||
        (impl->history_count != 0u &&
         (impl->history_bits[0] == NULL || impl->history_bits[1] == NULL ||
          impl->history_counts[0] == NULL ||
          impl->history_counts[1] == NULL)))
        return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
    for (index = 0u; index < impl->ir->state_count; ++index) {
        const cflow_statechart_state_kind kind = impl->ir->states[index].kind;
        impl->history_slots[index] = SIZE_MAX;
        if (kind == CFLOW_STATECHART_HISTORY_SHALLOW ||
            kind == CFLOW_STATECHART_HISTORY_DEEP)
            impl->history_slots[index] = history_slot++;
    }
    return CFLOW_STATECHART_INSTANCE_OK;
}

static bool activate_state(cflow_statechart_instance_impl *impl,
                           statechart_configuration_buffer *configuration,
                           size_t state, size_t *stack_count) {
    if (state >= impl->ir->state_count ||
        pseudo_kind(impl->ir->states[state].kind))
        return false;
    if (bit_test(configuration->bits, state)) return true;
    if (*stack_count >= impl->ir->state_count) return false;
    bit_set(configuration->bits, state);
    impl->entry_stack[(*stack_count)++] = state;
    return true;
}

static bool activate_target_path(
    cflow_statechart_instance_impl *impl,
    statechart_configuration_buffer *configuration,
    size_t compound,
    size_t target,
    size_t *stack_count) {
    size_t path_count = 0u, current = target;
    while (current != compound) {
        if (current == SIZE_MAX || path_count >= impl->ir->state_count ||
            pseudo_kind(impl->ir->states[current].kind))
            return false;
        impl->path_stack[path_count++] = current;
        current = impl->ir->parents[current];
    }
    if (path_count == 0u) return false;
    while (path_count != 0u) {
        if (!activate_state(impl, configuration,
                            impl->path_stack[--path_count], stack_count))
            return false;
    }
    return true;
}

static cflow_statechart_instance_status build_initial_configuration(
    cflow_statechart_instance_impl *impl, size_t staged) {
    statechart_configuration_buffer *configuration =
        &impl->configurations[staged];
    size_t stack_count = 0u, index;
    cflow_statechart_configuration_status configuration_status;
    memset(configuration->bits, 0, impl->bitset_bytes);
    configuration->state_count = 0u;
    for (index = 0u; index < impl->ir->state_count; ++index)
        impl->pseudo_transition_by_state[index] = SIZE_MAX;
    if (!activate_state(impl, configuration, impl->ir->root, &stack_count))
        return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;

    while (stack_count != 0u) {
        const size_t state = impl->entry_stack[--stack_count];
        const cflow_statechart_state_kind kind = impl->ir->states[state].kind;
        size_t child;
        if (kind == CFLOW_STATECHART_COMPOUND) {
            size_t active_child_count = 0u;
            size_t initial = SIZE_MAX;
            for (child = impl->ir->child_offsets[state];
                 child < impl->ir->child_offsets[state + 1u]; ++child) {
                const size_t child_state = impl->ir->children[child];
                if (impl->ir->states[child_state].kind ==
                    CFLOW_STATECHART_INITIAL)
                    initial = child_state;
                else if (!pseudo_kind(impl->ir->states[child_state].kind) &&
                         bit_test(configuration->bits, child_state))
                    ++active_child_count;
            }
            if (active_child_count > 1u || initial == SIZE_MAX)
                return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
            if (active_child_count == 0u) {
                const size_t transition =
                    impl->ir->default_transition_indices[initial];
                size_t target_cursor;
                if (transition == SIZE_MAX ||
                    impl->pseudo_transition_by_state[state] != SIZE_MAX)
                    return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
                for (target_cursor =
                         impl->ir->transition_target_offsets[transition];
                     target_cursor <
                         impl->ir->transition_target_offsets[transition + 1u];
                     ++target_cursor) {
                    const size_t target =
                        impl->ir->transition_target_indices[target_cursor];
                    const cflow_statechart_state_kind target_kind =
                        impl->ir->states[target].kind;
                    if (target_kind == CFLOW_STATECHART_HISTORY_SHALLOW ||
                        target_kind == CFLOW_STATECHART_HISTORY_DEEP) {
                        const size_t history_transition =
                            impl->ir->default_transition_indices[target];
                        size_t history_cursor;
                        if (history_transition == SIZE_MAX)
                            return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
                        for (history_cursor =
                                 impl->ir->transition_target_offsets[
                                     history_transition];
                             history_cursor <
                                 impl->ir->transition_target_offsets[
                                     history_transition + 1u];
                             ++history_cursor) {
                            if (!activate_target_path(
                                    impl, configuration, state,
                                    impl->ir->transition_target_indices[
                                        history_cursor],
                                    &stack_count))
                                return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
                        }
                    } else if (!activate_target_path(
                                   impl, configuration, state, target,
                                   &stack_count)) {
                        return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
                    }
                }
                impl->pseudo_transition_by_state[state] = transition;
            }
        } else if (kind == CFLOW_STATECHART_PARALLEL) {
            for (child = impl->ir->child_offsets[state];
                 child < impl->ir->child_offsets[state + 1u]; ++child) {
                const size_t child_state = impl->ir->children[child];
                if (!pseudo_kind(impl->ir->states[child_state].kind) &&
                    !activate_state(impl, configuration, child_state,
                                    &stack_count))
                    return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
            }
        }
    }

    for (index = 0u; index < impl->ir->state_count; ++index) {
        const size_t state = impl->ir->document_order_indices[index];
        if (bit_test(configuration->bits, state))
            configuration->states[configuration->state_count++] = state;
    }
    configuration_status = validate_dense_configuration(
        impl->ir, configuration->bits, configuration->states,
        configuration->state_count);
    return configuration_status == CFLOW_STATECHART_CONFIGURATION_OK
        ? CFLOW_STATECHART_INSTANCE_OK
        : CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
}

static const cflow_event_type *find_event_type(
    const cflow_statechart_impl *ir, cflow_event_id id) {
    size_t left = 0u, right = ir->event_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (ir->events[middle].id == id) return &ir->events[middle];
        if (ir->events[middle].id < id) left = middle + 1u;
        else right = middle;
    }
    return NULL;
}

static cflow_mailbox_status statechart_timer_contract(
    void *user, const cflow_event_view *event,
    const cmeta_type_desc **out_canonical_type) {
    const cflow_statechart_instance_impl *impl =
        (const cflow_statechart_instance_impl *)user;
    const cflow_event_type *type;
    if (out_canonical_type != NULL) *out_canonical_type = NULL;
    if (impl == NULL || event == NULL || event->id == 0u ||
        event->payload_type == NULL || event->payload == NULL ||
        out_canonical_type == NULL)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    type = find_event_type(impl->ir, event->id);
    if (type == NULL) return CFLOW_MAILBOX_INVALID_ARGUMENT;
    if (!cmeta_type_equal(type->payload_type, event->payload_type))
        return CFLOW_MAILBOX_TYPE_MISMATCH;
    *out_canonical_type = type->payload_type;
    return CFLOW_MAILBOX_OK;
}

static cflow_mailbox_status statechart_timer_send(
    void *user, const cflow_event_view *event) {
    cflow_statechart_instance handle = {user};
    return cflow_statechart_instance_try_send(&handle, event);
}

static const cflow_statechart_guard *find_guard_declaration(
    const cflow_statechart_instance_impl *impl,
    cflow_statechart_guard_id id) {
    size_t left = 0u, right = impl->ir->guard_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (impl->ir->guards[middle].id == id)
            return &impl->ir->guards[middle];
        if (impl->ir->guards[middle].id < id) left = middle + 1u;
        else right = middle;
    }
    return NULL;
}

static const cflow_statechart_guard_binding *find_statechart_guard_binding(
    const cflow_statechart_instance_impl *impl,
    cflow_statechart_guard_id id) {
    size_t left = 0u, right = impl->guard_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (impl->guards[middle].id == id) return &impl->guards[middle];
        if (impl->guards[middle].id < id) left = middle + 1u;
        else right = middle;
    }
    return NULL;
}

static void copy_error_storage_locked(cflow_statechart_instance_impl *impl,
                                      const char *message) {
    const char *source = message != NULL && message[0] != '\0'
        ? message : "Statechart guard failed";
    size_t length;
    if (source == impl->error_storage) return;
    length = strlen(source);
    if (length >= (size_t)CFLOW_STATECHART_ERROR_CAPACITY)
        length = (size_t)CFLOW_STATECHART_ERROR_CAPACITY - 1u;
    memcpy(impl->error_storage, source, length);
    impl->error_storage[length] = '\0';
}

static void publish_first_error_locked(cflow_statechart_instance_impl *impl,
                                       const char *message) {
    if (impl->error != NULL) return;
    copy_error_storage_locked(impl, message);
    impl->error = impl->error_storage;
}

static void counter_increment(uint64_t *counter) {
    if (*counter != UINT64_MAX) ++*counter;
}

static uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

uint64_t cflow_statechart_external_identity_sum_internal(
    uint64_t completed, uint64_t failed, uint64_t cancelled,
    uint64_t pending, uint64_t in_flight) {
    return saturating_add_u64(
        saturating_add_u64(
            saturating_add_u64(
                saturating_add_u64(completed, failed), cancelled),
            pending),
        in_flight);
}

static void settle_external_locked(cflow_statechart_instance_impl *impl,
                                   cflow_statechart_instance_status status) {
    cflow_statechart_external_settlement_kind kind;
    if (!impl->external_in_flight) return;
    if (status == CFLOW_STATECHART_INSTANCE_OK)
        kind = impl->external_in_flight_dropped
            ? CFLOW_STATECHART_EXTERNAL_SETTLED_DROPPED
            : CFLOW_STATECHART_EXTERNAL_SETTLED_COMPLETED;
    else if (status == CFLOW_STATECHART_INSTANCE_TASK_CANCELLED)
        kind = CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED;
    else
        kind = CFLOW_STATECHART_EXTERNAL_SETTLED_FAILED;
    if (impl->external_in_flight_origin_token != UINT64_C(0) &&
        impl->hooks.on_external_settlement != NULL) {
        impl->pending_external_settlement =
            (cflow_statechart_external_settlement){
                .origin_token = impl->external_in_flight_origin_token,
                .configuration_version = impl->configuration_version,
                .kind = kind,
                .status = status,
                .error = kind == CFLOW_STATECHART_EXTERNAL_SETTLED_FAILED
                    ? impl->error : NULL};
        impl->pending_external_settlement_ready = true;
    }
    impl->external_in_flight = false;
    impl->external_in_flight_origin_token = UINT64_C(0);
    impl->external_in_flight_dropped = false;
    if (status == CFLOW_STATECHART_INSTANCE_OK)
        counter_increment(&impl->external_completed);
    else if (status == CFLOW_STATECHART_INSTANCE_TASK_CANCELLED)
        counter_increment(&impl->external_cancelled);
    else
        counter_increment(&impl->external_failed);
}

static void cancel_pending_external_locked(
    cflow_statechart_instance_impl *impl) {
    const uint64_t pending = impl->external_pending > (size_t)UINT64_MAX
        ? UINT64_MAX : (uint64_t)impl->external_pending;
    if (impl->external_pending != 0u &&
        impl->hooks.on_external_settlement != NULL) {
        impl->cancelled_external_settlement_head =
            impl->external_source_head;
        impl->cancelled_external_settlement_count =
            impl->external_pending;
    }
    impl->external_pending = 0u;
    impl->external_source_head = impl->external_source_tail;
    if (UINT64_MAX - impl->external_cancelled < pending)
        impl->external_cancelled = UINT64_MAX;
    else
        impl->external_cancelled += pending;
}

static void invoke_detached_waker(cflow_waker waker) {
    if (waker.wake != NULL) waker.wake(waker.user);
}

static void deliver_external_settlements(
    cflow_statechart_instance_impl *impl) {
    cflow_statechart_external_settlement_fn callback;
    cflow_statechart_external_settlement settlement;
    void *user;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    if (impl->external_settlement_delivery_active ||
        impl->hooks.on_external_settlement == NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    impl->external_settlement_delivery_active = true;
    for (;;) {
        if (impl->pending_external_settlement_ready) {
            settlement = impl->pending_external_settlement;
            impl->pending_external_settlement_ready = false;
        } else if (impl->external_in_flight) {
            impl->external_settlement_delivery_active = false;
            cmeta_mutex_unlock(&impl->lock);
            return;
        } else if (impl->cancelled_external_settlement_count != 0u) {
            const size_t index =
                impl->cancelled_external_settlement_head;
            const uint64_t origin_token =
                impl->external_origin_tokens[index];
            impl->external_origin_tokens[index] = UINT64_C(0);
            impl->cancelled_external_settlement_head =
                (index + 1u) % impl->external_event_capacity;
            --impl->cancelled_external_settlement_count;
            if (origin_token == UINT64_C(0)) continue;
            settlement = (cflow_statechart_external_settlement){
                .origin_token = origin_token,
                .configuration_version = impl->configuration_version,
                .kind = CFLOW_STATECHART_EXTERNAL_SETTLED_CANCELLED,
                .status = CFLOW_STATECHART_INSTANCE_TASK_CANCELLED,
                .error = NULL};
        } else {
            impl->external_settlement_delivery_active = false;
            cmeta_mutex_unlock(&impl->lock);
            return;
        }
        callback = impl->hooks.on_external_settlement;
        user = impl->hook_user;
        cmeta_mutex_unlock(&impl->lock);
        callback(user, &settlement);
        cmeta_mutex_lock(&impl->lock);
    }
}

static cflow_waker take_downstream_waiter_locked(
    cflow_statechart_instance_impl *impl) {
    cflow_waker waker = impl->downstream_waiter;
    impl->downstream_waiter = (cflow_waker){0};
    return waker;
}

static cflow_waker take_terminal_waiter_locked(
    cflow_statechart_instance_impl *impl) {
    cflow_waker waker = impl->terminal_waiter;
    impl->terminal_waiter = (cflow_waker){0};
    return waker;
}

static void finish_terminal_side_effects(
    cflow_statechart_instance_impl *impl, cflow_waker waker) {
    bool terminal = false;
    bool settled = false;
    cflow_waker downstream_waker = {0};
    cflow_waker terminal_waker = {0};
    if (impl != NULL) {
        cmeta_mutex_lock(&impl->lock);
        terminal = impl->terminal_outcome != STATECHART_TERMINAL_NONE;
        settled = impl->done || impl->error != NULL;
        if (settled) {
            downstream_waker = take_downstream_waiter_locked(impl);
            terminal_waker = take_terminal_waiter_locked(impl);
        }
        cmeta_mutex_unlock(&impl->lock);
        if (terminal && impl->timers_initialized)
            (void)cflow_timer_event_queue_close(&impl->timers);
    }
    deliver_external_settlements(impl);
    invoke_detached_waker(waker);
    invoke_detached_waker(downstream_waker);
    invoke_detached_waker(terminal_waker);
}

static void cancel_external_admission_locked(
    cflow_statechart_instance_impl *impl, cflow_waker *out_waker) {
    cflow_waker adapter_waker = {0};
    if (out_waker != NULL) *out_waker = (cflow_waker){0};
    if (impl->external_mailbox_initialized)
        (void)cflow_mailbox_cancel_detach_internal(
            &impl->external_mailbox, out_waker);
    if (impl->adapter_internal_mailbox_initialized)
        (void)cflow_mailbox_cancel_detach_internal(
            &impl->adapter_internal_mailbox, &adapter_waker);
    impl->adapter_internal_pending = 0u;
    cancel_pending_external_locked(impl);
}

static void clear_semantic_queues_locked(
    cflow_statechart_instance_impl *impl) {
    impl->internal_event_head = 0u;
    impl->internal_event_count = 0u;
    impl->staged_event_count = 0u;
    impl->completion_head = 0u;
    impl->completion_count = 0u;
    impl->staged_completion_count = 0u;
    impl->root_completion_draining = false;
}

static void claim_external_for_failure_locked(
    cflow_statechart_instance_impl *impl) {
    cflow_event_id event_id = 0u;
    const cmeta_type_desc *event_type = NULL;
    if (impl->external_in_flight ||
        !impl->external_mailbox_initialized)
        return;
    if (cflow_mailbox_try_receive(
            &impl->external_mailbox, &event_id, &event_type,
            impl->driver_event_payload,
            impl->driver_event_capacity) == CFLOW_MAILBOX_OK) {
        if (impl->external_pending != 0u) --impl->external_pending;
        if (impl->external_origin_tokens != NULL) {
            impl->external_in_flight_origin_token =
                impl->external_origin_tokens[impl->external_source_head];
            impl->external_origin_tokens[impl->external_source_head] =
                UINT64_C(0);
            impl->external_source_head =
                (impl->external_source_head + 1u) %
                impl->external_event_capacity;
        }
        impl->external_in_flight = true;
    }
}

static void finish_macrostep_locked(
    cflow_statechart_instance_impl *impl,
    cflow_statechart_instance_status settlement) {
    settle_external_locked(impl, settlement);
    if (impl->macrostep_active) counter_increment(&impl->macrosteps);
    impl->macrostep_active = false;
    impl->macrostep_has_external = false;
    impl->macrostep_microsteps = 0u;
    impl->done = true;
}

static bool win_terminal_locked(
    cflow_statechart_instance_impl *impl,
    statechart_terminal_outcome outcome,
    cflow_statechart_instance_status status,
    const char *message,
    bool settle_now,
    cflow_statechart_instance_status settlement,
    cflow_waker *out_waker) {
    if (impl->terminal_outcome != STATECHART_TERMINAL_NONE)
        return false;
    impl->terminal_outcome = outcome;
    impl->closed = true;
    impl->cancelled = outcome == STATECHART_TERMINAL_CANCEL;
    if (outcome == STATECHART_TERMINAL_ERROR) {
        publish_first_error_locked(impl, message);
        impl->last_status = status;
    }
    clear_semantic_queues_locked(impl);
    cancel_external_admission_locked(impl, out_waker);
    if (impl->timers_initialized)
        (void)cflow_timer_event_queue_close_begin_internal(&impl->timers);
    impl->driver_repost = false;
    if (settle_now)
        finish_macrostep_locked(impl, settlement);
    return true;
}

static void latch_terminal_failure(cflow_statechart_instance_impl *impl,
                                   cflow_statechart_instance_status status,
                                   const char *message) {
    cflow_waker waker = {0};
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome == STATECHART_TERMINAL_NONE) {
        claim_external_for_failure_locked(impl);
        (void)win_terminal_locked(
            impl, STATECHART_TERMINAL_ERROR, status, message,
            true, status, &waker);
    }
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
}

static bool source_is_proper_descendant(const cflow_statechart_impl *ir,
                                        size_t descendant,
                                        size_t ancestor) {
    size_t current = descendant;
    if (descendant == ancestor) return false;
    while (current != SIZE_MAX) {
        current = ir->parents[current];
        if (current == ancestor) return true;
    }
    return false;
}

static bool trigger_valid(const cflow_statechart_instance_impl *impl,
                          const cflow_statechart_selection_trigger *trigger) {
    const cflow_event_type *event_type;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_EVENTLESS)
        return trigger->event == NULL && trigger->completion == 0u;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_COMPLETION) {
        const size_t completed = find_state_index(
            impl->ir, trigger->completion);
        return trigger->event == NULL && trigger->completion != 0u &&
            completed != SIZE_MAX &&
            cflow_statechart_internal_state_can_complete(
                impl->ir, completed);
    }
    if (trigger->kind != CFLOW_STATECHART_TRIGGER_EVENT ||
        trigger->event == NULL || trigger->completion != 0u ||
        trigger->event->id == 0u || trigger->event->payload_type == NULL ||
        trigger->event->payload == NULL)
        return false;
    event_type = find_event_type(impl->ir, trigger->event->id);
    return event_type != NULL &&
        cmeta_type_equal(event_type->payload_type,
                         trigger->event->payload_type);
}

static size_t find_event_index(const cflow_statechart_impl *ir,
                               cflow_event_id id);

static bool trigger_matches(const cflow_statechart_transition *transition,
                            const cflow_statechart_selection_trigger *trigger) {
    if (transition->trigger != trigger->kind) return false;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_EVENT)
        return transition->event == trigger->event->id;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_COMPLETION)
        return transition->completion == trigger->completion;
    return true;
}

typedef struct statechart_configuration_query {
    cflow_statechart_instance_impl *impl;
    const unsigned char *bits;
    size_t *staged_event_count;
    size_t staged_event_capacity;
    cflow_statechart_instance_status raise_status;
    const char *raise_error;
} statechart_configuration_query;

static bool configuration_query_is_active(
    void *user, cflow_machine_state_id state) {
    const statechart_configuration_query *query =
        (const statechart_configuration_query *)user;
    size_t state_index;
    if (query == NULL || query->impl == NULL || query->bits == NULL)
        return false;
    state_index = find_state_index(query->impl->ir, state);
    return state_index != SIZE_MAX &&
           !pseudo_kind(query->impl->ir->states[state_index].kind) &&
           bit_test(query->bits, state_index);
}

static cflow_statechart_instance_status stage_internal_event_for_instance(
    cflow_statechart_instance_impl *impl, const cflow_event_view *event,
    uint64_t origin_token, size_t *staged_event_count,
    size_t staged_event_capacity, const char **out_error) {
    size_t type_index;
    const cmeta_type_desc *type;
    if (out_error != NULL) *out_error = NULL;
    if (impl == NULL || event == NULL || staged_event_count == NULL ||
        out_error == NULL ||
        event->payload_type == NULL || event->payload == NULL) {
        if (out_error != NULL)
            *out_error = "Statechart internal event is invalid";
        return CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_INVALID;
    }
    type_index = find_event_index(impl->ir, event->id);
    if (type_index == SIZE_MAX) {
        *out_error = "Statechart internal event is unknown";
        return CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_INVALID;
    }
    type = impl->ir->events[type_index].payload_type;
    if (!cmeta_type_equal(type, event->payload_type)) {
        *out_error = "Statechart internal event type mismatch";
        return CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_TYPE_MISMATCH;
    }
    if (*staged_event_count >= staged_event_capacity) {
        *out_error = "Statechart internal event queue is full";
        return CFLOW_STATECHART_INSTANCE_INTERNAL_QUEUE_FULL;
    }
    impl->staged_event_slots[*staged_event_count].type_index = type_index;
    impl->staged_event_slots[*staged_event_count].origin_token =
        origin_token;
    memcpy(impl->staged_event_payloads +
               *staged_event_count * impl->event_payload_stride,
           event->payload, type->size);
    ++*staged_event_count;
    return CFLOW_STATECHART_INSTANCE_OK;
}

bool cflow_statechart_guard_context_raise_internal(
    const cflow_statechart_guard_context *context,
    const cflow_event_view *event, uint64_t origin_token,
    const char **out_error) {
    statechart_configuration_query *query = context != NULL
        ? (statechart_configuration_query *)context->configuration_user
        : NULL;
    cflow_statechart_instance_status status;
    if (out_error != NULL) *out_error = NULL;
    if (context == NULL || query == NULL ||
        query->impl == NULL ||
        context->is_active != configuration_query_is_active ||
        context->state != query->impl->extended_states[query->impl->published] ||
        query->bits != query->impl->configurations[query->impl->published].bits ||
        !query->impl->selection_in_progress) {
        if (out_error != NULL)
            *out_error = "Statechart guard context is invalid";
        return false;
    }
    if (query->raise_status != CFLOW_STATECHART_INSTANCE_OK) {
        if (out_error != NULL) *out_error = query->raise_error;
        return false;
    }
    if (out_error == NULL) {
        query->raise_status =
            CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_INVALID;
        query->raise_error = "Statechart internal event is invalid";
        return false;
    }
    status = stage_internal_event_for_instance(
        query->impl, event, origin_token, query->staged_event_count,
        query->staged_event_capacity, out_error);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        query->raise_status = status;
        query->raise_error = *out_error;
        return false;
    }
    return true;
}

static cflow_statechart_instance_status guard_enabled(
    cflow_statechart_instance_impl *impl,
    const cflow_statechart_transition *transition,
    const cflow_statechart_selection_trigger *trigger,
    size_t *staged_event_count, size_t staged_event_capacity,
    bool *out_enabled) {
    const cflow_statechart_guard *declaration;
    const cflow_statechart_guard_binding *binding;
    const char *error = NULL;
    bool enabled = false;
    bool succeeded;
    cflow_statechart_instance_status raise_status =
        CFLOW_STATECHART_INSTANCE_OK;
    if (transition->guard == 0u) {
        *out_enabled = true;
        return CFLOW_STATECHART_INSTANCE_OK;
    }
    declaration = find_guard_declaration(impl, transition->guard);
    binding = find_statechart_guard_binding(impl, transition->guard);
    if (declaration == NULL || binding == NULL) {
        latch_terminal_failure(
            impl, CFLOW_STATECHART_INSTANCE_GUARD_FAILED,
            "Statechart guard binding is missing");
        return CFLOW_STATECHART_INSTANCE_GUARD_FAILED;
    }
    if (binding->contextual_fn != NULL) {
        statechart_configuration_query query = {
            .impl = impl,
            .bits = impl->configurations[impl->published].bits,
            .staged_event_count = staged_event_count,
            .staged_event_capacity = staged_event_capacity,
            .raise_status = CFLOW_STATECHART_INSTANCE_OK};
        const cflow_statechart_guard_context context = {
            impl->extended_states[impl->published],
            trigger->kind == CFLOW_STATECHART_TRIGGER_EVENT
                ? trigger->event : NULL,
            configuration_query_is_active,
            (void *)&query};
        succeeded = binding->contextual_fn(
            binding->user, &context, &enabled, &error);
        raise_status = query.raise_status;
        if (raise_status != CFLOW_STATECHART_INSTANCE_OK) {
            succeeded = false;
            error = query.raise_error;
        }
    } else {
        succeeded = binding->fn(
            binding->user, impl->extended_states[impl->published],
            trigger->kind == CFLOW_STATECHART_TRIGGER_EVENT
                ? trigger->event : NULL,
            &enabled, &error);
    }
    if (!succeeded) {
        latch_terminal_failure(
            impl, CFLOW_STATECHART_INSTANCE_GUARD_FAILED,
            (declaration->effects & CMETA_EFFECT_MAY_FAIL) != 0u
                ? error : "Statechart guard contract violation");
        return CFLOW_STATECHART_INSTANCE_GUARD_FAILED;
    }
    *out_enabled = enabled;
    return CFLOW_STATECHART_INSTANCE_OK;
}

static void compute_exit_set(cflow_statechart_instance_impl *impl,
                             size_t transition_index) {
    const statechart_configuration_buffer *configuration =
        &impl->configurations[impl->published];
    const size_t domain = impl->ir->transition_domains[transition_index];
    size_t position;
    memset(impl->candidate_exit_set, 0, impl->bitset_bytes);
    if (impl->ir->transition_target_offsets[transition_index] ==
        impl->ir->transition_target_offsets[transition_index + 1u])
        return;
    for (position = 0u; position < configuration->state_count; ++position) {
        const size_t state = configuration->states[position];
        if (domain == SIZE_MAX ||
            source_is_proper_descendant(impl->ir, state, domain))
            bit_set(impl->candidate_exit_set, state);
    }
}

static bool exit_sets_intersect(const unsigned char *left,
                                const unsigned char *right,
                                size_t bytes) {
    size_t index;
    for (index = 0u; index < bytes; ++index)
        if ((left[index] & right[index]) != 0u) return true;
    return false;
}

static void remove_selected(cflow_statechart_instance_impl *impl,
                            size_t position) {
    const size_t tail = impl->selected_count - position - 1u;
    if (tail != 0u) {
        memmove(&impl->selected_transition_ids[position],
                &impl->selected_transition_ids[position + 1u],
                tail * sizeof(*impl->selected_transition_ids));
        memmove(&impl->selected_transition_indices[position],
                &impl->selected_transition_indices[position + 1u],
                tail * sizeof(*impl->selected_transition_indices));
        memmove(&impl->selected_sources[position],
                &impl->selected_sources[position + 1u],
                tail * sizeof(*impl->selected_sources));
        memmove(&impl->selected_leaf_orders[position],
                &impl->selected_leaf_orders[position + 1u],
                tail * sizeof(*impl->selected_leaf_orders));
        memmove(impl->selected_exit_sets + position * impl->bitset_bytes,
                impl->selected_exit_sets +
                    (position + 1u) * impl->bitset_bytes,
                tail * impl->bitset_bytes);
    }
    --impl->selected_count;
}

static void filter_candidate(cflow_statechart_instance_impl *impl,
                             size_t transition_index,
                             size_t source,
                             size_t leaf_order) {
    size_t position;
    bool accepted = true;
    for (position = 0u; position < impl->selected_count; ++position) {
        const unsigned char *selected_exit = impl->selected_exit_sets +
            position * impl->bitset_bytes;
        if (!exit_sets_intersect(impl->candidate_exit_set, selected_exit,
                                 impl->bitset_bytes))
            continue;
        if (!source_is_proper_descendant(
                impl->ir, source, impl->selected_sources[position])) {
            accepted = false;
            break;
        }
    }
    if (!accepted) return;
    position = 0u;
    while (position < impl->selected_count) {
        const unsigned char *selected_exit = impl->selected_exit_sets +
            position * impl->bitset_bytes;
        if (exit_sets_intersect(impl->candidate_exit_set, selected_exit,
                                impl->bitset_bytes))
            remove_selected(impl, position);
        else
            ++position;
    }
    position = impl->selected_count++;
    impl->selected_transition_ids[position] =
        impl->ir->transitions[transition_index].id;
    impl->selected_transition_indices[position] = transition_index;
    impl->selected_sources[position] = source;
    impl->selected_leaf_orders[position] = leaf_order;
    memcpy(impl->selected_exit_sets + position * impl->bitset_bytes,
           impl->candidate_exit_set, impl->bitset_bytes);
}

cflow_statechart_instance_status cflow_statechart_instance_select_internal(
    cflow_statechart_instance *instance,
    const cflow_statechart_selection_trigger *trigger,
    cflow_statechart_selection_snapshot *out) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    const statechart_configuration_buffer *configuration;
    size_t position, leaf_order = 0u;
    size_t selection_staged_event_count = 0u;
    size_t selection_staged_event_capacity;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (impl == NULL || trigger == NULL || out == NULL ||
        !trigger_valid(impl, trigger))
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome == STATECHART_TERMINAL_ERROR) {
        const cflow_statechart_instance_status status = impl->last_status;
        cmeta_mutex_unlock(&impl->lock);
        return status;
    }
    if (impl->closed || impl->done || impl->cancelled) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
    }
    if (impl->microstep_pending || impl->selection_in_progress) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_WOULD_BLOCK;
    }
    if (impl->selection_generation == UINT64_MAX) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    }
    impl->selection_in_progress = true;
    impl->selection_valid = false;
    impl->selection_consumed = false;
    impl->selected_count = 0u;
    impl->selected_configuration_version = impl->configuration_version;
    impl->staged_event_count = 0u;
    selection_staged_event_capacity =
        impl->internal_event_count <= impl->internal_event_capacity
            ? impl->internal_event_capacity - impl->internal_event_count
            : 0u;
    impl->selection_trigger = *trigger;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_EVENT) {
        const size_t type_index = find_event_index(
            impl->ir, trigger->event->id);
        const cmeta_type_desc *type = impl->ir->events[type_index].payload_type;
        memcpy(impl->selection_event_payload, trigger->event->payload,
               type->size);
        impl->selection_event = *trigger->event;
        impl->selection_event.payload_type = type;
        impl->selection_event.payload = impl->selection_event_payload;
        impl->selection_trigger.event = &impl->selection_event;
    }
    cmeta_mutex_unlock(&impl->lock);
    impl->selected_count = 0u;
    if (impl->ir->transition_count != 0u)
        memset(impl->candidate_seen, 0,
               (impl->ir->transition_count + 7u) / 8u);
    configuration = &impl->configurations[impl->published];
    for (position = 0u; position < configuration->state_count; ++position) {
        size_t source, span;
        const size_t leaf = configuration->states[position];
        bool candidate_found = false;
        if (!leaf_kind(impl->ir->states[leaf].kind)) continue;
        source = leaf;
        while (source != SIZE_MAX && !candidate_found) {
            for (span = impl->ir->transition_offsets[source];
                 span < impl->ir->transition_offsets[source + 1u]; ++span) {
                const size_t transition_index =
                    impl->ir->transition_indices[span];
                const cflow_statechart_transition *transition =
                    &impl->ir->transitions[transition_index];
                cflow_statechart_instance_status status;
                bool enabled = false;
                if (!trigger_matches(transition, &impl->selection_trigger))
                    continue;
                status = guard_enabled(
                    impl, transition, &impl->selection_trigger,
                    &selection_staged_event_count,
                    selection_staged_event_capacity, &enabled);
                if (status != CFLOW_STATECHART_INSTANCE_OK) {
                    cmeta_mutex_lock(&impl->lock);
                    impl->selected_count = 0u;
                    impl->selection_in_progress = false;
                    cmeta_mutex_unlock(&impl->lock);
                    return status;
                }
                if (!enabled) continue;
                candidate_found = true;
                if (!bit_test(impl->candidate_seen, transition_index)) {
                    bit_set(impl->candidate_seen, transition_index);
                    compute_exit_set(impl, transition_index);
                    filter_candidate(impl, transition_index, source,
                                     leaf_order);
                }
                break;
            }
            source = impl->ir->parents[source];
        }
        ++leaf_order;
    }
    cmeta_mutex_lock(&impl->lock);
    if (impl->closed || impl->done || impl->cancelled) {
        impl->selected_count = 0u;
        impl->selection_in_progress = false;
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
    }
    ++impl->selection_generation;
    impl->staged_event_count = selection_staged_event_count;
    commit_internal_events(impl);
    impl->selection_valid = true;
    impl->selection_in_progress = false;
    *out = (cflow_statechart_selection_snapshot){
        impl->selected_transition_ids,
        impl->selected_count,
        impl->selected_exit_sets,
        impl->bitset_bytes,
        impl->instance_token,
        impl->selection_generation,
        impl->selected_configuration_version,
        impl->selection_trigger.kind,
        impl->selection_trigger.kind == CFLOW_STATECHART_TRIGGER_EVENT
            ? impl->selection_event.id : 0u,
        impl->selection_trigger.kind == CFLOW_STATECHART_TRIGGER_COMPLETION
            ? impl->selection_trigger.completion : 0u};
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_STATECHART_INSTANCE_OK;
}

bool cflow_statechart_selection_exits_internal(
    const cflow_statechart_instance *instance,
    const cflow_statechart_selection_snapshot *selection,
    size_t transition_position,
    cflow_machine_state_id state) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    size_t state_index;
    bool exits = false;
    if (impl == NULL || selection == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    if (!impl->selection_valid ||
        selection->instance_token != impl->instance_token ||
        selection->generation != impl->selection_generation ||
        selection->configuration_version != impl->configuration_version ||
        selection->transition_ids != impl->selected_transition_ids ||
        selection->exit_sets != impl->selected_exit_sets ||
        selection->exit_set_stride != impl->bitset_bytes ||
        selection->transition_count != impl->selected_count ||
        transition_position >= selection->transition_count) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    state_index = find_state_index(impl->ir, state);
    if (state_index != SIZE_MAX)
        exits = bit_test(
            selection->exit_sets +
                transition_position * selection->exit_set_stride,
            state_index);
    cmeta_mutex_unlock(&impl->lock);
    return exits;
}

static const cflow_statechart_executable *find_executable_declaration(
    const cflow_statechart_instance_impl *impl,
    cflow_statechart_executable_id id) {
    size_t left = 0u, right = impl->ir->executable_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (impl->ir->executables[middle].id == id)
            return &impl->ir->executables[middle];
        if (impl->ir->executables[middle].id < id) left = middle + 1u;
        else right = middle;
    }
    return NULL;
}

static const cflow_statechart_executable_binding *find_executable_binding(
    const cflow_statechart_instance_impl *impl,
    cflow_statechart_executable_id id) {
    size_t left = 0u, right = impl->executable_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (impl->executables[middle].id == id)
            return &impl->executables[middle];
        if (impl->executables[middle].id < id) left = middle + 1u;
        else right = middle;
    }
    return NULL;
}

static size_t find_event_index(const cflow_statechart_impl *ir,
                               cflow_event_id id) {
    size_t left = 0u, right = ir->event_count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2u;
        if (ir->events[middle].id == id) return middle;
        if (ir->events[middle].id < id) left = middle + 1u;
        else right = middle;
    }
    return SIZE_MAX;
}

typedef struct statechart_action_context {
    cflow_statechart_instance_impl *impl;
    const cflow_event_view *event;
    unsigned char *current_state;
    unsigned char *next_state;
    bool *current_state_live;
    bool *next_state_live;
    unsigned char *configuration_bits;
    size_t invoked;
    cflow_statechart_instance_status raise_status;
    const char *raise_error;
    cflow_statechart_instance_status effect_status;
    const char *effect_error;
} statechart_action_context;

struct cflow_statechart_host_context {
    cflow_statechart_instance_impl *impl;
    statechart_action_context staging;
    const cflow_statechart_observed_event *trigger;
    cflow_statechart_host_phase phase;
    size_t staged;
    bool state_edited;
};

static bool action_configuration_is_active(
    void *user, cflow_machine_state_id state) {
    const statechart_action_context *context =
        (const statechart_action_context *)user;
    const cflow_statechart_instance_impl *impl =
        context != NULL ? context->impl : NULL;
    size_t state_index;
    if (impl == NULL || context->configuration_bits == NULL) return false;
    state_index = find_state_index(impl->ir, state);
    return state_index != SIZE_MAX &&
           !pseudo_kind(impl->ir->states[state_index].kind) &&
           bit_test(context->configuration_bits, state_index);
}

static bool stage_internal_event_tagged(
    void *user, const cflow_event_view *event, uint64_t origin_token,
    const char **out_error) {
    statechart_action_context *context =
        (statechart_action_context *)user;
    cflow_statechart_instance_impl *impl =
        context != NULL ? context->impl : NULL;
    cflow_statechart_instance_status status;
    if (context != NULL &&
        context->raise_status != CFLOW_STATECHART_INSTANCE_OK) {
        if (out_error != NULL) *out_error = context->raise_error;
        return false;
    }
    if (out_error == NULL) {
        if (context != NULL) {
            context->raise_status =
                CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_INVALID;
            context->raise_error = "Statechart internal event is invalid";
        }
        return false;
    }
    status = stage_internal_event_for_instance(
        impl, event, origin_token, &impl->staged_event_count,
        impl->internal_event_count <= impl->internal_event_capacity
            ? impl->internal_event_capacity - impl->internal_event_count
            : 0u,
        out_error);
    if (status == CFLOW_STATECHART_INSTANCE_OK) return true;
    if (context != NULL) {
        context->raise_status = status;
        context->raise_error = out_error != NULL ? *out_error : NULL;
    }
    return false;
}

static bool stage_internal_event(void *user, const cflow_event_view *event,
                                 const char **out_error) {
    return stage_internal_event_tagged(user, event, UINT64_C(0), out_error);
}

static bool stage_external_effect(
    void *user, const cflow_statechart_effect_ticket *ticket,
    const char **out_error) {
    statechart_action_context *context =
        (statechart_action_context *)user;
    cflow_statechart_instance_impl *impl =
        context != NULL ? context->impl : NULL;
    if (context != NULL &&
        context->effect_status != CFLOW_STATECHART_INSTANCE_OK) {
        if (out_error != NULL) *out_error = context->effect_error;
        return false;
    }
    if (out_error != NULL) *out_error = NULL;
    if (impl == NULL || ticket == NULL || out_error == NULL ||
        ticket->commit == NULL || ticket->discard == NULL) {
        if (context != NULL) {
            context->effect_status = CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
            context->effect_error = "Statechart effect ticket is invalid";
        }
        if (out_error != NULL)
            *out_error = "Statechart effect ticket is invalid";
        return false;
    }
    if (impl->staged_effect_count >= impl->effect_capacity) {
        context->effect_status =
            CFLOW_STATECHART_INSTANCE_EFFECT_JOURNAL_FULL;
        context->effect_error = "Statechart effect journal is full";
        *out_error = context->effect_error;
        return false;
    }
    impl->staged_effects[impl->staged_effect_count++] = *ticket;
    return true;
}

static cflow_statechart_instance_status invoke_executable(
    statechart_action_context *context,
    cflow_statechart_executable_id executable_id,
    cflow_statechart_action_phase phase,
    cflow_machine_state_id owner,
    const char **out_error) {
    cflow_statechart_instance_impl *impl = context->impl;
    const cflow_statechart_executable *declaration =
        find_executable_declaration(impl, executable_id);
    const cflow_statechart_executable_binding *binding =
        find_executable_binding(impl, executable_id);
    const char *callback_error = NULL;
    bool succeeded;
    unsigned char *previous;
    bool *previous_live;
    if (out_error != NULL) *out_error = NULL;
    if (declaration == NULL || binding == NULL ||
        !cflow_executor_is_current_internal(impl->executor)) {
        if (out_error != NULL)
            *out_error = "Statechart executable context is invalid";
        return CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
    }
    context->raise_status = CFLOW_STATECHART_INSTANCE_OK;
    context->raise_error = NULL;
    context->effect_status = CFLOW_STATECHART_INSTANCE_OK;
    context->effect_error = NULL;
    reset_state_value(
        impl, context->next_state, context->next_state_live);
    if (binding->contextual_fn != NULL) {
        const cflow_statechart_executable_context executable_context = {
            .phase = phase,
            .owner = owner,
            .state = context->current_state,
            .event = context->event,
            .out_state = context->next_state,
            .raise_internal = stage_internal_event,
            .raise_user = context,
            .stage_effect = stage_external_effect,
            .effect_user = context,
            .is_active = action_configuration_is_active,
            .configuration_user = context,
            .raise_internal_tagged = stage_internal_event_tagged};
        succeeded = binding->contextual_fn(
            binding->user, &executable_context, &callback_error);
    } else {
        succeeded = binding->fn(
            binding->user, phase, owner, context->current_state, context->event,
            context->next_state, stage_internal_event, context, &callback_error);
    }
    if (succeeded) *context->next_state_live = true;
    ++context->invoked;
    if (context->raise_status != CFLOW_STATECHART_INSTANCE_OK) {
        if (out_error != NULL) *out_error = context->raise_error;
        return context->raise_status;
    }
    if (context->effect_status != CFLOW_STATECHART_INSTANCE_OK) {
        if (out_error != NULL) *out_error = context->effect_error;
        return context->effect_status;
    }
    if (!succeeded) {
        if (out_error != NULL) {
            *out_error = (declaration->effects & CMETA_EFFECT_MAY_FAIL) != 0u
                ? (callback_error != NULL && callback_error[0] != '\0'
                       ? callback_error : "Statechart action failed")
                : "Statechart executable contract violation";
        }
        return CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
    }
    previous = context->current_state;
    previous_live = context->current_state_live;
    context->current_state = context->next_state;
    context->current_state_live = context->next_state_live;
    context->next_state = previous;
    context->next_state_live = previous_live;
    return CFLOW_STATECHART_INSTANCE_OK;
}

static bool finalize_transaction_state(
    statechart_action_context *context, size_t staged) {
    cflow_statechart_instance_impl *impl = context->impl;
    if (context->current_state_live == NULL ||
        !*context->current_state_live)
        return false;
    if (context->current_state != impl->extended_states[staged]) {
        reset_state_value(
            impl, impl->extended_states[staged],
            &impl->extended_state_live[staged]);
        if (!cflow_value_move_construct(
                impl->ir->state_type, impl->extended_states[staged],
                context->current_state))
            return false;
        impl->extended_state_live[staged] = true;
        *context->current_state_live = false;
    }
    reset_state_value(
        impl, impl->action_scratch, &impl->action_scratch_live);
    return true;
}

static bool exit_before(const cflow_statechart_impl *ir,
                        size_t left, size_t right) {
    if (source_is_proper_descendant(ir, left, right)) return true;
    if (source_is_proper_descendant(ir, right, left)) return false;
    return ir->states[left].document_order >
        ir->states[right].document_order;
}

static bool entry_before(const cflow_statechart_impl *ir,
                         size_t left, size_t right) {
    if (source_is_proper_descendant(ir, right, left)) return true;
    if (source_is_proper_descendant(ir, left, right)) return false;
    return ir->states[left].document_order <
        ir->states[right].document_order;
}

static void stable_order_states(const cflow_statechart_impl *ir,
                                size_t *states, size_t count,
                                bool (*before)(const cflow_statechart_impl *,
                                               size_t, size_t)) {
    size_t index;
    for (index = 1u; index < count; ++index) {
        const size_t value = states[index];
        size_t position = index;
        while (position != 0u && before(ir, value, states[position - 1u])) {
            states[position] = states[position - 1u];
            --position;
        }
        states[position] = value;
    }
}

static bool copy_staging_buffers(cflow_statechart_instance_impl *impl,
                                 size_t staged) {
    const size_t published = impl->published;
    const size_t history_bytes = impl->history_count * impl->bitset_bytes;
    const size_t history_count_bytes =
        impl->history_count * sizeof(*impl->history_counts[staged]);
    memcpy(impl->configurations[staged].bits,
           impl->configurations[published].bits, impl->bitset_bytes);
    memcpy(impl->configurations[staged].states,
           impl->configurations[published].states,
           impl->configurations[published].state_count * sizeof(size_t));
    impl->configurations[staged].state_count =
        impl->configurations[published].state_count;
    if (!copy_state_value(
            impl, impl->extended_states[staged],
            &impl->extended_state_live[staged],
            impl->extended_states[published]))
        return false;
    if (history_bytes != 0u) {
        memcpy(impl->history_bits[staged], impl->history_bits[published],
               history_bytes);
        memcpy(impl->history_counts[staged],
               impl->history_counts[published], history_count_bytes);
    }
    memcpy(impl->completion_bits[staged],
           impl->completion_bits[published], impl->bitset_bytes);
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    return true;
}

cflow_statechart_host_phase cflow_statechart_host_context_phase(
    const cflow_statechart_host_context *context) {
    return context != NULL ? context->phase
                           : (cflow_statechart_host_phase)0;
}

const cflow_statechart_observed_event *cflow_statechart_host_context_trigger(
    const cflow_statechart_host_context *context) {
    return context != NULL ? context->trigger : NULL;
}

const void *cflow_statechart_host_context_state(
    const cflow_statechart_host_context *context) {
    const cflow_statechart_instance_impl *impl =
        context != NULL ? context->impl : NULL;
    return impl != NULL ? impl->extended_states[impl->published] : NULL;
}

uint64_t cflow_statechart_host_context_configuration_version(
    const cflow_statechart_host_context *context) {
    const cflow_statechart_instance_impl *impl =
        context != NULL ? context->impl : NULL;
    return impl != NULL ? impl->configuration_version : UINT64_C(0);
}

bool cflow_statechart_host_context_is_active(
    const cflow_statechart_host_context *context,
    cflow_machine_state_id state) {
    return context != NULL &&
        action_configuration_is_active(
            (void *)&context->staging, state);
}

void *cflow_statechart_host_context_edit_state(
    cflow_statechart_host_context *context, const char **out_error) {
    size_t staged_event_count;
    size_t staged_completion_count;
    if (out_error != NULL) *out_error = NULL;
    if (context == NULL || context->impl == NULL || out_error == NULL) {
        if (out_error != NULL)
            *out_error = "Statechart host context is invalid";
        return NULL;
    }
    if (context->state_edited)
        return context->impl->extended_states[context->staged];
    staged_event_count = context->impl->staged_event_count;
    staged_completion_count = context->impl->staged_completion_count;
    if (!copy_staging_buffers(context->impl, context->staged)) {
        context->staging.raise_status =
            CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
        context->staging.raise_error =
            "Statechart host state copy failed";
        *out_error = context->staging.raise_error;
        return NULL;
    }
    context->impl->staged_event_count = staged_event_count;
    context->impl->staged_completion_count = staged_completion_count;
    context->state_edited = true;
    context->staging.next_state =
        context->impl->extended_states[context->staged];
    context->staging.next_state_live =
        &context->impl->extended_state_live[context->staged];
    return context->staging.next_state;
}

bool cflow_statechart_host_context_raise_internal(
    cflow_statechart_host_context *context, const cflow_event_view *event,
    uint64_t origin_token, const char **out_error) {
    if (context == NULL) {
        if (out_error != NULL)
            *out_error = "Statechart host context is invalid";
        return false;
    }
    return stage_internal_event_tagged(
        &context->staging, event, origin_token, out_error);
}

bool cflow_statechart_host_context_stage_effect(
    cflow_statechart_host_context *context,
    const cflow_statechart_effect_ticket *ticket, const char **out_error) {
    if (context == NULL) {
        if (out_error != NULL)
            *out_error = "Statechart host context is invalid";
        return false;
    }
    return stage_external_effect(&context->staging, ticket, out_error);
}

static void compute_exit_union(cflow_statechart_instance_impl *impl,
                               size_t *out_count) {
    const statechart_configuration_buffer *published =
        &impl->configurations[impl->published];
    size_t transition, position;
    *out_count = 0u;
    memset(impl->exit_union, 0, impl->bitset_bytes);
    for (transition = 0u; transition < impl->request_transition_count;
         ++transition) {
        const unsigned char *exit_set = impl->request_exit_sets +
            transition * impl->bitset_bytes;
        size_t byte;
        for (byte = 0u; byte < impl->bitset_bytes; ++byte)
            impl->exit_union[byte] |= exit_set[byte];
    }
    for (position = 0u; position < published->state_count; ++position) {
        const size_t state = published->states[position];
        if (bit_test(impl->exit_union, state))
            impl->exit_order[(*out_count)++] = state;
    }
    stable_order_states(impl->ir, impl->exit_order, *out_count, exit_before);
}

static void save_affected_history(cflow_statechart_instance_impl *impl,
                                  size_t staged) {
    const statechart_configuration_buffer *published =
        &impl->configurations[impl->published];
    size_t history;
    for (history = 0u; history < impl->ir->state_count; ++history) {
        const cflow_statechart_state_kind kind = impl->ir->states[history].kind;
        const size_t parent = impl->ir->parents[history];
        const size_t slot = impl->history_slots[history];
        unsigned char *bits;
        size_t position, count = 0u;
        if (slot == SIZE_MAX || parent == SIZE_MAX ||
            !bit_test(impl->exit_union, parent))
            continue;
        bits = impl->history_bits[staged] + slot * impl->bitset_bytes;
        memset(bits, 0, impl->bitset_bytes);
        for (position = 0u; position < published->state_count; ++position) {
            const size_t state = published->states[position];
            const bool remember =
                kind == CFLOW_STATECHART_HISTORY_SHALLOW
                    ? impl->ir->parents[state] == parent &&
                          !pseudo_kind(impl->ir->states[state].kind)
                    : leaf_kind(impl->ir->states[state].kind) &&
                          source_is_proper_descendant(
                              impl->ir, state, parent);
            if (remember) {
                bit_set(bits, state);
                ++count;
            }
        }
        impl->history_counts[staged][slot] = count;
    }
}

static bool activate_staged(cflow_statechart_instance_impl *impl,
                            statechart_configuration_buffer *configuration,
                            size_t state, size_t *stack_count) {
    if (state >= impl->ir->state_count ||
        pseudo_kind(impl->ir->states[state].kind))
        return false;
    if (bit_test(configuration->bits, state)) return true;
    if (*stack_count >= impl->ir->state_count) return false;
    bit_set(configuration->bits, state);
    bit_set(impl->entry_bits, state);
    impl->entry_stack[(*stack_count)++] = state;
    return true;
}

static bool activate_path_to(cflow_statechart_instance_impl *impl,
                             statechart_configuration_buffer *configuration,
                             size_t target, size_t *stack_count) {
    size_t current = target, path_count = 0u;
    while (current != SIZE_MAX && !bit_test(configuration->bits, current)) {
        if (path_count >= impl->ir->state_count ||
            pseudo_kind(impl->ir->states[current].kind))
            return false;
        impl->path_stack[path_count++] = current;
        current = impl->ir->parents[current];
    }
    while (path_count != 0u) {
        if (!activate_staged(impl, configuration,
                             impl->path_stack[--path_count], stack_count))
            return false;
    }
    return true;
}

static bool collect_pseudo_transition(cflow_statechart_instance_impl *impl,
                                      size_t pseudo,
                                      size_t *pseudo_count) {
    const size_t transition = impl->ir->default_transition_indices[pseudo];
    const size_t owner = impl->ir->parents[pseudo];
    if (transition == SIZE_MAX || owner == SIZE_MAX ||
        *pseudo_count >= impl->ir->state_count ||
        impl->pseudo_transition_by_state[owner] != SIZE_MAX)
        return false;
    impl->pseudo_transition_by_state[owner] = transition;
    impl->pseudo_transition_order[(*pseudo_count)++] = transition;
    return true;
}

static void reset_pseudo_transition_map(cflow_statechart_instance_impl *impl) {
    size_t state;
    for (state = 0u; state < impl->ir->state_count; ++state)
        impl->pseudo_transition_by_state[state] = SIZE_MAX;
}

static bool restore_history_target(
    cflow_statechart_instance_impl *impl,
    statechart_configuration_buffer *configuration,
    size_t staged, size_t history,
    size_t *stack_count, size_t *pseudo_count,
    bool collect_default_action);

static bool enter_default_descendants(
    cflow_statechart_instance_impl *impl,
    statechart_configuration_buffer *configuration,
    size_t staged,
    size_t *stack_count,
    size_t *pseudo_count) {
    while (*stack_count != 0u) {
        const size_t state = impl->entry_stack[--(*stack_count)];
        const cflow_statechart_state_kind kind = impl->ir->states[state].kind;
        size_t child;
        if (kind == CFLOW_STATECHART_COMPOUND) {
            size_t active_children = 0u, initial = SIZE_MAX;
            for (child = impl->ir->child_offsets[state];
                 child < impl->ir->child_offsets[state + 1u]; ++child) {
                const size_t child_state = impl->ir->children[child];
                if (impl->ir->states[child_state].kind ==
                    CFLOW_STATECHART_INITIAL)
                    initial = child_state;
                else if (!pseudo_kind(impl->ir->states[child_state].kind) &&
                         bit_test(configuration->bits, child_state))
                    ++active_children;
            }
            if (active_children > 1u || initial == SIZE_MAX) return false;
            if (active_children == 0u) {
                const size_t transition =
                    impl->ir->default_transition_indices[initial];
                size_t target_cursor;
                if (!collect_pseudo_transition(
                        impl, initial, pseudo_count))
                    return false;
                for (target_cursor =
                         impl->ir->transition_target_offsets[transition];
                     target_cursor <
                         impl->ir->transition_target_offsets[transition + 1u];
                     ++target_cursor) {
                    const size_t target =
                        impl->ir->transition_target_indices[target_cursor];
                    const cflow_statechart_state_kind target_kind =
                        impl->ir->states[target].kind;
                    if (target_kind == CFLOW_STATECHART_HISTORY_SHALLOW ||
                        target_kind == CFLOW_STATECHART_HISTORY_DEEP) {
                        if (!restore_history_target(
                                impl, configuration, staged, target,
                                stack_count, pseudo_count, false))
                            return false;
                    } else if (!activate_path_to(
                                   impl, configuration, target,
                                   stack_count)) {
                        return false;
                    }
                }
            }
        } else if (kind == CFLOW_STATECHART_PARALLEL) {
            child = impl->ir->child_offsets[state + 1u];
            while (child != impl->ir->child_offsets[state]) {
                --child;
                const size_t child_state = impl->ir->children[child];
                if (!pseudo_kind(impl->ir->states[child_state].kind) &&
                    !activate_staged(
                        impl, configuration, child_state, stack_count))
                    return false;
            }
        }
    }
    return true;
}

static bool restore_history_target(cflow_statechart_instance_impl *impl,
                                   statechart_configuration_buffer *configuration,
                                   size_t staged, size_t history,
                                   size_t *stack_count,
                                   size_t *pseudo_count,
                                   bool collect_default_action) {
    const size_t parent = impl->ir->parents[history];
    const size_t slot = impl->history_slots[history];
    const size_t count = impl->history_counts[staged][slot];
    const unsigned char *bits =
        impl->history_bits[staged] + slot * impl->bitset_bytes;
    size_t index;
    if (!activate_path_to(impl, configuration, parent, stack_count))
        return false;
    if (count == 0u) {
        const size_t transition =
            impl->ir->default_transition_indices[history];
        size_t target_cursor;
        if (collect_default_action &&
            !collect_pseudo_transition(impl, history, pseudo_count))
            return false;
        if (transition == SIZE_MAX) return false;
        for (target_cursor =
                 impl->ir->transition_target_offsets[transition];
             target_cursor <
                 impl->ir->transition_target_offsets[transition + 1u];
             ++target_cursor) {
            if (!activate_path_to(
                    impl, configuration,
                    impl->ir->transition_target_indices[target_cursor],
                    stack_count))
                return false;
        }
        return true;
    }
    for (index = 0u; index < impl->ir->state_count; ++index) {
        const size_t state = impl->ir->document_order_indices[index];
        if (bit_test(bits, state) &&
            !activate_path_to(impl, configuration, state, stack_count))
            return false;
    }
    return true;
}

static bool build_target_configuration(
    cflow_statechart_instance_impl *impl, size_t staged,
    size_t *out_pseudo_count, size_t *out_entry_count) {
    statechart_configuration_buffer *configuration =
        &impl->configurations[staged];
    size_t transition, stack_count = 0u, pseudo_count = 0u, index;
    memset(impl->entry_bits, 0, impl->bitset_bytes);
    reset_pseudo_transition_map(impl);
    for (index = 0u; index < impl->ir->state_count; ++index) {
        if (bit_test(impl->exit_union, index))
            bit_clear(configuration->bits, index);
    }
    for (transition = 0u; transition < impl->request_transition_count;
         ++transition) {
        const size_t transition_index =
            impl->request_transition_indices[transition];
        size_t target_cursor;
        for (target_cursor =
                 impl->ir->transition_target_offsets[transition_index];
             target_cursor <
                 impl->ir->transition_target_offsets[transition_index + 1u];
             ++target_cursor) {
            const size_t target =
                impl->ir->transition_target_indices[target_cursor];
            if (impl->ir->states[target].kind ==
                    CFLOW_STATECHART_HISTORY_SHALLOW ||
                impl->ir->states[target].kind ==
                    CFLOW_STATECHART_HISTORY_DEEP) {
                if (!restore_history_target(
                        impl, configuration, staged, target,
                        &stack_count, &pseudo_count, true))
                    return false;
            } else if (!activate_path_to(
                           impl, configuration, target, &stack_count)) {
                return false;
            }
        }
    }
    if (!enter_default_descendants(
            impl, configuration, staged, &stack_count, &pseudo_count))
        return false;
    configuration->state_count = 0u;
    *out_entry_count = 0u;
    for (index = 0u; index < impl->ir->state_count; ++index) {
        const size_t state = impl->ir->document_order_indices[index];
        if (bit_test(configuration->bits, state))
            configuration->states[configuration->state_count++] = state;
        if (bit_test(impl->entry_bits, state))
            impl->entry_order[(*out_entry_count)++] = state;
    }
    stable_order_states(
        impl->ir, impl->entry_order, *out_entry_count, entry_before);
    *out_pseudo_count = pseudo_count;
    return validate_dense_configuration(
               impl->ir, configuration->bits, configuration->states,
               configuration->state_count) ==
        CFLOW_STATECHART_CONFIGURATION_OK;
}

static cflow_statechart_instance_status run_state_action_span(
    statechart_action_context *context, size_t state,
    cflow_statechart_state_action_kind kind, const char **out_error) {
    cflow_statechart_instance_impl *impl = context->impl;
    const size_t bucket = state * 2u + (size_t)kind;
    size_t action;
    for (action = impl->ir->state_action_offsets[bucket];
         action < impl->ir->state_action_offsets[bucket + 1u]; ++action) {
        const cflow_statechart_state_action *row =
            &impl->ir->state_actions[impl->ir->state_action_indices[action]];
        const cflow_statechart_instance_status status = invoke_executable(
            context, row->executable,
            kind == CFLOW_STATECHART_STATE_ACTION_EXIT
                ? CFLOW_STATECHART_ACTION_EXIT
                : CFLOW_STATECHART_ACTION_ENTRY,
            impl->ir->states[state].id, out_error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) return status;
    }
    return CFLOW_STATECHART_INSTANCE_OK;
}

static cflow_statechart_instance_status run_transition_action_span(
    statechart_action_context *context, size_t transition,
    cflow_statechart_action_phase phase, const char **out_error) {
    cflow_statechart_instance_impl *impl = context->impl;
    size_t action;
    for (action = impl->ir->transition_action_offsets[transition];
         action < impl->ir->transition_action_offsets[transition + 1u];
         ++action) {
        const cflow_statechart_transition_action *row =
            &impl->ir->transition_actions[
                impl->ir->transition_action_indices[action]];
        const cflow_statechart_instance_status status = invoke_executable(
            context, row->executable, phase,
            impl->ir->transitions[transition].source, out_error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) return status;
    }
    return CFLOW_STATECHART_INSTANCE_OK;
}

static cflow_statechart_instance_status
run_pseudo_transition_action(
    statechart_action_context *context, size_t transition, size_t source,
    const char **out_error) {
    cflow_statechart_instance_impl *impl = context->impl;
    cflow_statechart_action_phase phase;
    if (source >= impl->ir->state_count ||
        !pseudo_kind(impl->ir->states[source].kind)) {
        if (out_error != NULL)
            *out_error = "Statechart pseudo transition source is invalid";
        return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
    }
    phase = impl->ir->states[source].kind == CFLOW_STATECHART_INITIAL
        ? CFLOW_STATECHART_ACTION_INITIAL
        : CFLOW_STATECHART_ACTION_HISTORY;
    return run_transition_action_span(context, transition, phase, out_error);
}

static cflow_statechart_instance_status
run_pseudo_transition_action_chain(
    statechart_action_context *context, size_t staged, size_t transition,
    const char **out_error) {
    cflow_statechart_instance_impl *impl = context->impl;
    size_t history_transition = SIZE_MAX;
    size_t source, target = SIZE_MAX;
    cflow_statechart_instance_status status;
    if (transition >= impl->ir->transition_count) {
        if (out_error != NULL)
            *out_error = "Statechart pseudo transition is invalid";
        return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
    }
    source = find_state_index(
        impl->ir, impl->ir->transitions[transition].source);
    if (source != SIZE_MAX &&
        impl->ir->states[source].kind == CFLOW_STATECHART_INITIAL) {
        const size_t target_begin =
            impl->ir->transition_target_offsets[transition];
        if (target_begin <
            impl->ir->transition_target_offsets[transition + 1u])
            target = impl->ir->transition_target_indices[target_begin];
        if (target != SIZE_MAX &&
            (impl->ir->states[target].kind ==
                 CFLOW_STATECHART_HISTORY_SHALLOW ||
             impl->ir->states[target].kind ==
                 CFLOW_STATECHART_HISTORY_DEEP)) {
            const size_t slot = impl->history_slots[target];
            if (slot == SIZE_MAX || slot >= impl->history_count ||
                impl->ir->default_transition_indices[target] == SIZE_MAX) {
                if (out_error != NULL)
                    *out_error = "Statechart initial history chain is invalid";
                return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
            }
            if (impl->history_counts[staged][slot] == 0u)
                history_transition =
                    impl->ir->default_transition_indices[target];
        }
    }
    status = run_pseudo_transition_action(
        context, transition, source, out_error);
    if (status != CFLOW_STATECHART_INSTANCE_OK ||
        history_transition == SIZE_MAX)
        return status;
    return run_pseudo_transition_action(
        context, history_transition, target, out_error);
}

static bool configuration_state_complete(
    const cflow_statechart_instance_impl *impl,
    const statechart_configuration_buffer *configuration,
    size_t state) {
    const cflow_statechart_state_kind kind = impl->ir->states[state].kind;
    size_t child;
    if (!bit_test(configuration->bits, state)) return false;
    if (kind == CFLOW_STATECHART_FINAL) return true;
    if (kind == CFLOW_STATECHART_ATOMIC || pseudo_kind(kind)) return false;
    if (kind == CFLOW_STATECHART_COMPOUND) {
        for (child = impl->ir->child_offsets[state];
             child < impl->ir->child_offsets[state + 1u]; ++child) {
            const size_t child_state = impl->ir->children[child];
            if (!pseudo_kind(impl->ir->states[child_state].kind) &&
                bit_test(configuration->bits, child_state))
                return bit_test(impl->completion_work, child_state);
        }
        return false;
    }
    for (child = impl->ir->child_offsets[state];
         child < impl->ir->child_offsets[state + 1u]; ++child) {
        const size_t child_state = impl->ir->children[child];
        if (pseudo_kind(impl->ir->states[child_state].kind)) continue;
        if (!bit_test(configuration->bits, child_state) ||
            !bit_test(impl->completion_work, child_state))
            return false;
    }
    return true;
}

static cflow_statechart_instance_status stage_completions(
    cflow_statechart_instance_impl *impl, size_t staged) {
    const statechart_configuration_buffer *configuration =
        &impl->configurations[staged];
    size_t order;
    memset(impl->completion_work, 0, impl->bitset_bytes);
    for (order = 0u; order < impl->ir->state_count; ++order) {
        const size_t state = impl->ir->document_order_indices[
            impl->ir->state_count - order - 1u];
        const cflow_statechart_state_kind kind = impl->ir->states[state].kind;
        if (!bit_test(configuration->bits, state)) {
            bit_clear(impl->completion_bits[staged], state);
            continue;
        }
        if (configuration_state_complete(impl, configuration, state))
            bit_set(impl->completion_work, state);
        if (kind != CFLOW_STATECHART_COMPOUND &&
            kind != CFLOW_STATECHART_PARALLEL)
            continue;
        if (!bit_test(impl->completion_work, state)) {
            bit_clear(impl->completion_bits[staged], state);
            continue;
        }
        if (bit_test(impl->completion_bits[staged], state)) continue;
        if (impl->completion_count > impl->completion_capacity ||
            impl->staged_completion_count >=
                impl->completion_capacity - impl->completion_count)
            return CFLOW_STATECHART_INSTANCE_COMPLETION_QUEUE_FULL;
        impl->staged_completion_rows[impl->staged_completion_count++] = state;
        bit_set(impl->completion_bits[staged], state);
    }
    return CFLOW_STATECHART_INSTANCE_OK;
}

static void commit_completions(cflow_statechart_instance_impl *impl) {
    size_t index;
    for (index = 0u; index < impl->staged_completion_count; ++index) {
        const size_t tail =
            (impl->completion_head + impl->completion_count) %
            impl->completion_capacity;
        impl->completion_rows[tail] = impl->staged_completion_rows[index];
        ++impl->completion_count;
    }
    impl->staged_completion_count = 0u;
}

static void microstep_fail(cflow_statechart_instance_impl *impl,
                           cflow_statechart_instance_status status,
                           const char *error) {
    cflow_waker waker = {0};
    reset_transaction_state(impl, 1u - impl->published);
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    discard_staged_effects(impl);
    cmeta_mutex_lock(&impl->lock);
    impl->microstep_result = status;
    if (impl->microstep_failed != UINT64_MAX) ++impl->microstep_failed;
    (void)win_terminal_locked(
        impl, STATECHART_TERMINAL_ERROR, status,
        error != NULL && error[0] != '\0'
            ? error : "Statechart action failed",
        true, status, &waker);
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
}

static void commit_internal_events(cflow_statechart_instance_impl *impl) {
    size_t index;
    for (index = 0u; index < impl->staged_event_count; ++index) {
        const size_t tail =
            (impl->internal_event_head + impl->internal_event_count) %
            impl->internal_event_capacity;
        const size_t type_index = impl->staged_event_slots[index].type_index;
        const cmeta_type_desc *type = impl->ir->events[type_index].payload_type;
        impl->internal_event_slots[tail].type_index = type_index;
        impl->internal_event_slots[tail].origin_token =
            impl->staged_event_slots[index].origin_token;
        memcpy(impl->internal_event_payloads +
                   tail * impl->event_payload_stride,
               impl->staged_event_payloads +
                   index * impl->event_payload_stride,
               type->size);
        ++impl->internal_event_count;
    }
    impl->staged_event_count = 0u;
}

static void commit_staged_effects(cflow_statechart_instance_impl *impl) {
    size_t index;
    const size_t count = impl->staged_effect_count;
    impl->staged_effect_count = 0u;
    for (index = 0u; index < count; ++index)
        impl->staged_effects[index].commit(
            impl->staged_effects[index].user);
}

static void discard_staged_effects(cflow_statechart_instance_impl *impl) {
    size_t index;
    const size_t count = impl->staged_effect_count;
    impl->staged_effect_count = 0u;
    for (index = 0u; index < count; ++index)
        impl->staged_effects[index].discard(
            impl->staged_effects[index].user);
}

static cflow_statechart_host_result run_host_transaction_runtime_hook(
    cflow_statechart_instance_impl *impl,
    cflow_statechart_host_phase phase,
    const cflow_statechart_observed_event *trigger) {
    cflow_statechart_host_context context;
    cflow_statechart_host_result result;
    const size_t published = impl->published;
    const size_t staged = 1u - published;
    const char *error = NULL;
    cflow_statechart_instance_status failure_status =
        CFLOW_STATECHART_INSTANCE_HOOK_FAILED;
    if (impl->hooks.on_host_transaction == NULL)
        return CFLOW_STATECHART_HOST_CONTINUE;
    if ((phase != CFLOW_STATECHART_HOST_PREPARE_TRIGGER &&
         phase != CFLOW_STATECHART_HOST_PREPARE_QUIESCENCE) ||
        (phase == CFLOW_STATECHART_HOST_PREPARE_TRIGGER) !=
            (trigger != NULL) ||
        impl->staged_event_count != 0u ||
        impl->staged_completion_count != 0u ||
        impl->staged_effect_count != 0u) {
        latch_terminal_failure(
            impl, CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION,
            "Statechart host transaction boundary is invalid");
        return CFLOW_STATECHART_HOST_FATAL;
    }
    context = (cflow_statechart_host_context){
        .impl = impl,
        .staging = {
            .impl = impl,
            .current_state = impl->extended_states[published],
            .current_state_live = &impl->extended_state_live[published],
            .configuration_bits = impl->configurations[published].bits,
            .raise_status = CFLOW_STATECHART_INSTANCE_OK,
            .effect_status = CFLOW_STATECHART_INSTANCE_OK},
        .trigger = trigger,
        .phase = phase,
        .staged = staged};
    result = impl->hooks.on_host_transaction(
        impl->hook_user, &context, &error);
    if (context.staging.raise_status != CFLOW_STATECHART_INSTANCE_OK) {
        failure_status = context.staging.raise_status;
        error = context.staging.raise_error;
        goto fail;
    }
    if (context.staging.effect_status != CFLOW_STATECHART_INSTANCE_OK) {
        failure_status = context.staging.effect_status;
        error = context.staging.effect_error;
        goto fail;
    }
    if (result != CFLOW_STATECHART_HOST_CONTINUE &&
        result != CFLOW_STATECHART_HOST_DROP &&
        result != CFLOW_STATECHART_HOST_FATAL) {
        error = "Statechart host transaction returned an invalid result";
        goto fail;
    }
    if (result == CFLOW_STATECHART_HOST_FATAL) goto fail;
    if (error != NULL) {
        error = "Statechart host transaction returned an error without FATAL";
        goto fail;
    }
    if (result == CFLOW_STATECHART_HOST_DROP) {
        if (phase != CFLOW_STATECHART_HOST_PREPARE_TRIGGER ||
            trigger->kind != CFLOW_STATECHART_OBSERVED_EXTERNAL) {
            error = "Statechart host DROP requires an external trigger";
            goto fail;
        }
        impl->staged_event_count = 0u;
        impl->staged_completion_count = 0u;
        discard_staged_effects(impl);
        if (context.state_edited)
            reset_transaction_state(impl, staged);
        return result;
    }

    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome != STATECHART_TERMINAL_NONE ||
        impl->error != NULL) {
        impl->staged_event_count = 0u;
        impl->staged_completion_count = 0u;
        cmeta_mutex_unlock(&impl->lock);
        discard_staged_effects(impl);
        if (context.state_edited)
            reset_transaction_state(impl, staged);
        return CFLOW_STATECHART_HOST_FATAL;
    }
    commit_internal_events(impl);
    impl->staged_completion_count = 0u;
    if (context.state_edited) {
        impl->published = staged;
        if (impl->configuration_version != UINT64_MAX)
            ++impl->configuration_version;
    }
    cmeta_mutex_unlock(&impl->lock);
    commit_staged_effects(impl);
    return result;

fail:
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    discard_staged_effects(impl);
    if (context.state_edited)
        reset_transaction_state(impl, staged);
    latch_terminal_failure(
        impl, failure_status,
        error != NULL && error[0] != '\0'
            ? error : "Statechart host transaction failed");
    return CFLOW_STATECHART_HOST_FATAL;
}

static cflow_statechart_instance_status execute_initial_entry_actions(
    cflow_statechart_instance_impl *impl) {
    const size_t staged = 1u;
    const statechart_configuration_buffer *configuration =
        &impl->configurations[staged];
    statechart_action_context context = {
        .impl = impl,
        .event = NULL,
        .current_state = impl->extended_states[staged],
        .next_state = impl->action_scratch,
        .current_state_live = &impl->extended_state_live[staged],
        .next_state_live = &impl->action_scratch_live,
        .configuration_bits = impl->action_configuration_bits,
        .raise_status = CFLOW_STATECHART_INSTANCE_OK};
    const char *error = NULL;
    cflow_statechart_instance_status status;
    size_t position;
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    impl->staged_effect_count = 0u;
    memset(impl->action_configuration_bits, 0, impl->bitset_bytes);
    for (position = 0u; position < configuration->state_count; ++position)
        impl->entry_order[position] = configuration->states[position];
    stable_order_states(
        impl->ir, impl->entry_order, configuration->state_count,
        entry_before);
    for (position = 0u; position < configuration->state_count; ++position) {
        const size_t state = impl->entry_order[position];
        const size_t transition = impl->pseudo_transition_by_state[state];
        bit_set(impl->action_configuration_bits, state);
        status = run_state_action_span(
            &context, state,
            CFLOW_STATECHART_STATE_ACTION_ENTRY, &error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) goto fail;
        if (transition != SIZE_MAX) {
            status = run_pseudo_transition_action_chain(
                &context, staged, transition, &error);
            if (status != CFLOW_STATECHART_INSTANCE_OK) goto fail;
        }
    }
    status = stage_completions(impl, staged);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        error = "Statechart completion queue is full";
        goto fail;
    }
    if (!finalize_transaction_state(&context, staged)) {
        status = CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
        error = "Statechart action state finalization failed";
        goto fail;
    }
    cmeta_mutex_lock(&impl->lock);
    commit_internal_events(impl);
    commit_completions(impl);
    impl->published = staged;
    impl->configuration_version = 1u;
    if (UINT64_MAX - impl->actions < (uint64_t)context.invoked)
        impl->actions = UINT64_MAX;
    else
        impl->actions += (uint64_t)context.invoked;
    impl->initial_configuration_pending = false;
    cmeta_mutex_unlock(&impl->lock);
    commit_staged_effects(impl);
    return CFLOW_STATECHART_INSTANCE_OK;

fail:
    reset_transaction_state(impl, staged);
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    discard_staged_effects(impl);
    latch_terminal_failure(
        impl, status,
        error != NULL && error[0] != '\0'
            ? error : "Statechart initial entry action failed");
    return status;
}

static cflow_statechart_instance_status execute_controlled_exit(
    cflow_statechart_instance_impl *impl) {
    const size_t staged = 1u - impl->published;
    const statechart_configuration_buffer *published =
        &impl->configurations[impl->published];
    statechart_configuration_buffer *next = &impl->configurations[staged];
    statechart_action_context context;
    cflow_waker waker = {0};
    const char *error = NULL;
    cflow_statechart_instance_status status =
        CFLOW_STATECHART_INSTANCE_OK;
    size_t exit_count, position;

    cmeta_mutex_lock(&impl->lock);
    if (!impl->exit_requested ||
        impl->terminal_outcome != STATECHART_TERMINAL_NONE) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
    }
    cmeta_mutex_unlock(&impl->lock);

    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    impl->staged_effect_count = 0u;
    if (!copy_staging_buffers(impl, staged)) {
        status = CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
        error = "Statechart controlled exit state copy failed";
        goto fail;
    }
    memcpy(impl->action_configuration_bits, published->bits,
           impl->bitset_bytes);
    exit_count = published->state_count;
    for (position = 0u; position < exit_count; ++position)
        impl->exit_order[position] = published->states[position];
    stable_order_states(
        impl->ir, impl->exit_order, exit_count, exit_before);
    context = (statechart_action_context){
        .impl = impl,
        .event = NULL,
        .current_state = impl->extended_states[staged],
        .next_state = impl->action_scratch,
        .current_state_live = &impl->extended_state_live[staged],
        .next_state_live = &impl->action_scratch_live,
        .configuration_bits = impl->action_configuration_bits,
        .raise_status = CFLOW_STATECHART_INSTANCE_OK};
    for (position = 0u; position < exit_count; ++position) {
        const size_t state = impl->exit_order[position];
        status = run_state_action_span(
            &context, state,
            CFLOW_STATECHART_STATE_ACTION_EXIT, &error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) goto fail;
        bit_clear(impl->action_configuration_bits, state);
    }
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    memset(next->bits, 0, impl->bitset_bytes);
    next->state_count = 0u;
    memset(impl->completion_bits[staged], 0, impl->bitset_bytes);
    if (!finalize_transaction_state(&context, staged)) {
        status = CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
        error = "Statechart controlled exit state finalization failed";
        goto fail;
    }

    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome != STATECHART_TERMINAL_NONE) {
        cmeta_mutex_unlock(&impl->lock);
        reset_transaction_state(impl, staged);
        discard_staged_effects(impl);
        return CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
    }
    impl->published = staged;
    if (impl->configuration_version != UINT64_MAX)
        ++impl->configuration_version;
    if (impl->microsteps != UINT64_MAX) ++impl->microsteps;
    if (UINT64_MAX - impl->actions < (uint64_t)context.invoked)
        impl->actions = UINT64_MAX;
    else
        impl->actions += (uint64_t)context.invoked;
    (void)win_terminal_locked(
        impl, STATECHART_TERMINAL_CANCEL,
        CFLOW_STATECHART_INSTANCE_OK, NULL, true,
        CFLOW_STATECHART_INSTANCE_TASK_CANCELLED, &waker);
    cmeta_mutex_unlock(&impl->lock);
    commit_staged_effects(impl);
    finish_terminal_side_effects(impl, waker);
    return CFLOW_STATECHART_INSTANCE_OK;

fail:
    reset_transaction_state(impl, staged);
    impl->staged_event_count = 0u;
    impl->staged_completion_count = 0u;
    discard_staged_effects(impl);
    latch_terminal_failure(
        impl, status,
        error != NULL && error[0] != '\0'
            ? error : "Statechart controlled exit action failed");
    return status;
}

static cflow_statechart_instance_status execute_microstep(
    cflow_statechart_instance_impl *impl) {
    const size_t staged = 1u - impl->published;
    statechart_action_context context;
    size_t exit_count, entry_count = 0u, pseudo_count = 0u, position;
    const char *error = NULL;
    cflow_statechart_instance_status status;
    impl->staged_effect_count = 0u;
    if (!copy_staging_buffers(impl, staged)) {
        microstep_fail(
            impl, CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED,
            "Statechart staged state copy failed");
        return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
    }
    compute_exit_union(impl, &exit_count);
    save_affected_history(impl, staged);
    memcpy(impl->action_configuration_bits,
           impl->configurations[impl->published].bits, impl->bitset_bytes);
    context = (statechart_action_context){
        .impl = impl,
        .event = impl->request_trigger.kind == CFLOW_STATECHART_TRIGGER_EVENT
            ? &impl->request_event : NULL,
        .current_state = impl->extended_states[staged],
        .next_state = impl->action_scratch,
        .current_state_live = &impl->extended_state_live[staged],
        .next_state_live = &impl->action_scratch_live,
        .configuration_bits = impl->action_configuration_bits,
        .raise_status = CFLOW_STATECHART_INSTANCE_OK};
    for (position = 0u; position < exit_count; ++position) {
        const size_t state = impl->exit_order[position];
        status = run_state_action_span(
            &context, state,
            CFLOW_STATECHART_STATE_ACTION_EXIT, &error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) {
            microstep_fail(impl, status, error);
            return status;
        }
        bit_clear(impl->action_configuration_bits, state);
    }
    for (position = 0u; position < impl->request_transition_count; ++position) {
        status = run_transition_action_span(
            &context, impl->request_transition_indices[position],
            CFLOW_STATECHART_ACTION_TRANSITION, &error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) {
            microstep_fail(impl, status, error);
            return status;
        }
    }
    if (!build_target_configuration(
            impl, staged, &pseudo_count, &entry_count)) {
        microstep_fail(
            impl, CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION,
            "Statechart staged configuration is invalid");
        return CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION;
    }
    for (position = 0u; position < pseudo_count; ++position) {
        const size_t transition = impl->pseudo_transition_order[position];
        const size_t pseudo = find_state_index(
            impl->ir, impl->ir->transitions[transition].source);
        const size_t owner = pseudo != SIZE_MAX
            ? impl->ir->parents[pseudo] : SIZE_MAX;
        if (owner != SIZE_MAX && bit_test(impl->entry_bits, owner)) continue;
        status = run_pseudo_transition_action_chain(
            &context, staged, transition, &error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) {
            microstep_fail(impl, status, error);
            return status;
        }
    }
    for (position = 0u; position < entry_count; ++position) {
        const size_t state = impl->entry_order[position];
        const size_t transition = impl->pseudo_transition_by_state[state];
        bit_set(impl->action_configuration_bits, state);
        status = run_state_action_span(
            &context, state,
            CFLOW_STATECHART_STATE_ACTION_ENTRY, &error);
        if (status != CFLOW_STATECHART_INSTANCE_OK) {
            microstep_fail(impl, status, error);
            return status;
        }
        if (transition != SIZE_MAX) {
            status = run_pseudo_transition_action_chain(
                &context, staged, transition, &error);
            if (status != CFLOW_STATECHART_INSTANCE_OK) {
                microstep_fail(impl, status, error);
                return status;
            }
        }
    }
    for (position = 0u; position < impl->ir->state_count; ++position) {
        const cflow_statechart_state_kind kind =
            impl->ir->states[position].kind;
        if (bit_test(impl->exit_union, position) &&
            (kind == CFLOW_STATECHART_COMPOUND ||
             kind == CFLOW_STATECHART_PARALLEL))
            bit_clear(impl->completion_bits[staged], position);
    }
    status = stage_completions(impl, staged);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        microstep_fail(impl, status,
                       "Statechart completion queue is full");
        return status;
    }
    if (!finalize_transaction_state(&context, staged)) {
        microstep_fail(
            impl, CFLOW_STATECHART_INSTANCE_ACTION_FAILED,
            "Statechart action state finalization failed");
        return CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
    }
    for (position = 0u; position < exit_count; ++position)
        impl->timer_exit_scopes[position] =
            impl->ir->states[impl->exit_order[position]].id;
    cmeta_mutex_lock(&impl->lock);
    if (impl->cancelled) {
        impl->staged_event_count = 0u;
        impl->staged_completion_count = 0u;
        impl->microstep_result = CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
        if (impl->microstep_cancelled != UINT64_MAX)
            ++impl->microstep_cancelled;
        cmeta_mutex_unlock(&impl->lock);
        discard_staged_effects(impl);
        reset_transaction_state(impl, staged);
        return CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
    }
    if (impl->timers_initialized)
        (void)cflow_timer_event_queue_cancel_scopes(
            &impl->timers, impl->timer_exit_scopes, exit_count);
    commit_internal_events(impl);
    commit_completions(impl);
    impl->published = staged;
    if (impl->configuration_version != UINT64_MAX)
        ++impl->configuration_version;
    if (impl->microsteps != UINT64_MAX) ++impl->microsteps;
    if (UINT64_MAX - impl->actions < (uint64_t)context.invoked)
        impl->actions = UINT64_MAX;
    else
        impl->actions += (uint64_t)context.invoked;
    impl->microstep_result = CFLOW_STATECHART_INSTANCE_OK;
    if (impl->microstep_completed != UINT64_MAX)
        ++impl->microstep_completed;
    cmeta_mutex_unlock(&impl->lock);
    commit_staged_effects(impl);
    return CFLOW_STATECHART_INSTANCE_OK;
}

static void statechart_microstep_run(void *user) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)user;
    (void)execute_microstep(impl);
}

static void statechart_microstep_cancel(void *user) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)user;
    cflow_waker waker = {0};
    void (*after_cancel)(void *) = NULL;
    void *hook_user = NULL;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome == STATECHART_TERMINAL_NONE) {
        impl->microstep_result = CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED;
        (void)win_terminal_locked(
            impl, STATECHART_TERMINAL_ERROR,
            CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED,
            "Statechart SerialExecutor cancelled a queued microstep",
            true, CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED, &waker);
    } else {
        impl->microstep_result =
            impl->terminal_outcome == STATECHART_TERMINAL_ERROR
            ? impl->last_status
            : CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
    }
    if (impl->microstep_cancelled != UINT64_MAX)
        ++impl->microstep_cancelled;
    after_cancel = impl->test_hooks.after_microstep_cancel;
    hook_user = impl->test_hooks.user;
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
    if (after_cancel != NULL) after_cancel(hook_user);
}

static void statechart_microstep_finalize(void *user) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)user;
    cflow_statechart_instance_status result;
    bool continue_driver;
    cmeta_mutex_lock(&impl->lock);
    if (impl->microstep_finalized != UINT64_MAX)
        ++impl->microstep_finalized;
    result = impl->microstep_result;
    if (impl->driver_after_microstep) {
        impl->driver_after_microstep = false;
        if (result == CFLOW_STATECHART_INSTANCE_OK &&
            impl->terminal_outcome == STATECHART_TERMINAL_NONE) {
            impl->driver_repost = true;
        } else if (!impl->done) {
            const cflow_statechart_instance_status settlement =
                impl->terminal_outcome == STATECHART_TERMINAL_CLOSE &&
                    result == CFLOW_STATECHART_INSTANCE_OK
                ? CFLOW_STATECHART_INSTANCE_OK
                : CFLOW_STATECHART_INSTANCE_TASK_CANCELLED;
            finish_macrostep_locked(impl, settlement);
            clear_semantic_queues_locked(impl);
        }
    }
    impl->microstep_pending = false;
    continue_driver = impl->driver_repost && !impl->done &&
        impl->error == NULL;
    impl->driver_repost = false;
    cmeta_mutex_unlock(&impl->lock);
    deliver_external_settlements(impl);
    if (continue_driver) {
        (void)schedule_statechart_driver_reserved(impl);
    } else {
        cmeta_mutex_lock(&impl->lock);
        release_instance_task_locked(impl);
        cmeta_mutex_unlock(&impl->lock);
    }
}

static bool trigger_matches_selection(
    const cflow_statechart_instance_impl *impl,
    const cflow_statechart_selection_trigger *trigger,
    const cflow_statechart_selection_snapshot *selection) {
    if (trigger->kind != selection->trigger_kind ||
        impl->selection_trigger.kind != selection->trigger_kind)
        return false;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_EVENT)
        return selection->completion == 0u &&
            trigger->event->id == selection->event_id &&
            impl->selection_event.id == selection->event_id;
    if (trigger->kind == CFLOW_STATECHART_TRIGGER_COMPLETION)
        return selection->event_id == 0u &&
            trigger->completion == selection->completion &&
            impl->selection_trigger.completion == selection->completion;
    return selection->event_id == 0u && selection->completion == 0u;
}

cflow_admission_status cflow_statechart_instance_try_microstep_internal(
    cflow_statechart_instance *instance,
    const cflow_statechart_selection_trigger *trigger,
    const cflow_statechart_selection_snapshot *selection) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    const cflow_executor_task task = {
        statechart_microstep_run, statechart_microstep_cancel,
        statechart_microstep_finalize, impl};
    cflow_admission_status admission;
    size_t index;
    if (impl == NULL || trigger == NULL || selection == NULL ||
        !trigger_valid(impl, trigger))
        return CFLOW_ADMISSION_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    if (impl->closed || impl->done || impl->cancelled) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_CLOSED;
    }
    if (impl->microstep_pending || impl->selection_in_progress ||
        impl->error != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_FULL;
    }
    if (!impl->selection_valid || impl->selection_consumed ||
        selection->instance_token != impl->instance_token ||
        selection->generation != impl->selection_generation ||
        selection->configuration_version != impl->configuration_version ||
        selection->configuration_version !=
            impl->selected_configuration_version ||
        selection->transition_count == 0u ||
        selection->transition_ids != impl->selected_transition_ids ||
        selection->exit_sets != impl->selected_exit_sets ||
        selection->exit_set_stride != impl->bitset_bytes ||
        selection->transition_count != impl->selected_count ||
        !trigger_matches_selection(impl, trigger, selection)) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_INVALID_ARGUMENT;
    }
    if (!reserve_instance_task_locked(impl)) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_FULL;
    }
    impl->microstep_pending = true;
    impl->selection_consumed = true;
    impl->request_transition_count = impl->selected_count;
    for (index = 0u; index < impl->selected_count; ++index)
        impl->request_transition_indices[index] =
            impl->selected_transition_indices[index];
    memcpy(impl->request_exit_sets, impl->selected_exit_sets,
           impl->selected_count * impl->bitset_bytes);
    impl->request_trigger = impl->selection_trigger;
    if (impl->selection_trigger.kind == CFLOW_STATECHART_TRIGGER_EVENT) {
        const cmeta_type_desc *type = impl->selection_event.payload_type;
        memcpy(impl->request_event_payload, impl->selection_event_payload,
               type->size);
        impl->request_event = impl->selection_event;
        impl->request_event.payload_type = type;
        impl->request_event.payload = impl->request_event_payload;
        impl->request_trigger.event = &impl->request_event;
    }
    if (impl->microstep_accepted != UINT64_MAX)
        ++impl->microstep_accepted;
    {
        void (*before_post)(void *) =
            impl->test_hooks.before_microstep_post;
        void *hook_user = impl->test_hooks.user;
        cmeta_mutex_unlock(&impl->lock);
        if (before_post != NULL) before_post(hook_user);
    }

    admission = cflow_executor_try_post_task(impl->executor, &task);
    if (admission != CFLOW_ADMISSION_ACCEPTED) {
        cmeta_mutex_lock(&impl->lock);
        impl->microstep_pending = false;
        impl->selection_consumed = false;
        if (impl->microstep_accepted != 0u) --impl->microstep_accepted;
        if (!impl->done &&
            (impl->terminal_outcome == STATECHART_TERMINAL_CLOSE ||
             impl->terminal_outcome == STATECHART_TERMINAL_CANCEL)) {
            finish_macrostep_locked(
                impl, CFLOW_STATECHART_INSTANCE_TASK_CANCELLED);
            clear_semantic_queues_locked(impl);
        }
        cmeta_mutex_unlock(&impl->lock);
        deliver_external_settlements(impl);
        cmeta_mutex_lock(&impl->lock);
        release_instance_task_locked(impl);
        cmeta_mutex_unlock(&impl->lock);
    }
    return admission;
}

static bool root_configuration_complete(
    const cflow_statechart_instance_impl *impl) {
    return (impl->ir->states[impl->ir->root].kind ==
                CFLOW_STATECHART_FINAL &&
            bit_test(impl->configurations[impl->published].bits,
                     impl->ir->root)) ||
        bit_test(impl->completion_bits[impl->published], impl->ir->root);
}

static bool root_completion_ready(
    cflow_statechart_instance_impl *impl) {
    bool ready;
    cmeta_mutex_lock(&impl->lock);
    ready = root_configuration_complete(impl) &&
        (impl->ir->states[impl->ir->root].kind ==
             CFLOW_STATECHART_FINAL ||
         (impl->completion_count != 0u &&
          impl->completion_rows[impl->completion_head] == impl->ir->root));
    cmeta_mutex_unlock(&impl->lock);
    return ready;
}

static bool pop_internal_event(cflow_statechart_instance_impl *impl,
                               cflow_event_view *out,
                               uint64_t *out_origin_token) {
    size_t slot, type_index;
    const cmeta_type_desc *type;
    cmeta_mutex_lock(&impl->lock);
    if (impl->internal_event_count == 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    slot = impl->internal_event_head;
    type_index = impl->internal_event_slots[slot].type_index;
    type = impl->ir->events[type_index].payload_type;
    memcpy(impl->driver_event_payload,
           impl->internal_event_payloads +
               slot * impl->event_payload_stride,
           type->size);
    impl->internal_event_head =
        (impl->internal_event_head + 1u) % impl->internal_event_capacity;
    --impl->internal_event_count;
    *out = (cflow_event_view){
        impl->ir->events[type_index].id, type, impl->driver_event_payload};
    *out_origin_token = impl->internal_event_slots[slot].origin_token;
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

static bool pop_adapter_internal_event(
    cflow_statechart_instance_impl *impl, cflow_event_view *out) {
    cflow_event_id event_id = 0u;
    const cmeta_type_desc *event_type = NULL;
    cflow_mailbox_status status;
    cmeta_mutex_lock(&impl->lock);
    if (!impl->adapter_internal_mailbox_initialized ||
        impl->adapter_internal_pending == 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    status = cflow_mailbox_try_receive(
        &impl->adapter_internal_mailbox, &event_id, &event_type,
        impl->driver_event_payload, impl->driver_event_capacity);
    if (status != CFLOW_MAILBOX_OK) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    --impl->adapter_internal_pending;
    *out = (cflow_event_view){
        event_id, event_type, impl->driver_event_payload};
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

static bool pop_completion(cflow_statechart_instance_impl *impl,
                           cflow_machine_state_id *out) {
    size_t state;
    cmeta_mutex_lock(&impl->lock);
    if (impl->completion_count == 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    state = impl->completion_rows[impl->completion_head];
    impl->completion_head =
        (impl->completion_head + 1u) % impl->completion_capacity;
    --impl->completion_count;
    *out = impl->ir->states[state].id;
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

/* -1 failed, 0 unhandled, 1 posted one semantic microstep. */
static int driver_try_trigger(cflow_statechart_instance_impl *impl,
                              const cflow_statechart_selection_trigger *trigger) {
    cflow_statechart_selection_snapshot selection = {0};
    cflow_statechart_instance handle = {impl};
    cflow_statechart_instance_status status =
        cflow_statechart_instance_select_internal(
            &handle, trigger, &selection);
    cflow_admission_status admission;
    if (status == CFLOW_STATECHART_INSTANCE_TASK_CANCELLED)
        return -1;
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        latch_terminal_failure(
            impl, status, cflow_statechart_instance_error(&handle));
        return -1;
    }
    if (selection.transition_count == 0u) return 0;
    cmeta_mutex_lock(&impl->lock);
    if (impl->macrostep_microsteps >= impl->microstep_limit) {
        cmeta_mutex_unlock(&impl->lock);
        latch_terminal_failure(
            impl, CFLOW_STATECHART_INSTANCE_MICROSTEP_LIMIT_EXCEEDED,
            "Statechart macrostep microstep limit exceeded");
        return -1;
    }
    ++impl->macrostep_microsteps;
    impl->driver_after_microstep = true;
    cmeta_mutex_unlock(&impl->lock);
    admission = cflow_statechart_instance_try_microstep_internal(
        &handle, trigger, &selection);
    if (admission == CFLOW_ADMISSION_ACCEPTED) return 1;
    cmeta_mutex_lock(&impl->lock);
    impl->driver_after_microstep = false;
    if (impl->macrostep_microsteps != 0u) --impl->macrostep_microsteps;
    {
        const bool terminal = impl->closed || impl->done || impl->cancelled;
        cmeta_mutex_unlock(&impl->lock);
        if (terminal) return -1;
    }
    latch_terminal_failure(
        impl,
        admission == CFLOW_ADMISSION_FULL
            ? CFLOW_STATECHART_INSTANCE_EXECUTOR_FULL
            : CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED,
        admission == CFLOW_ADMISSION_FULL
            ? "Statechart SerialExecutor is full"
            : "Statechart SerialExecutor is closed");
    return -1;
}

static int driver_try_completion(
    cflow_statechart_instance_impl *impl,
    cflow_machine_state_id completion) {
    const cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_COMPLETION, NULL, completion};
    const cflow_statechart_observed_event observed = {
        CFLOW_STATECHART_OBSERVED_COMPLETION, NULL, completion, 0u};
    if (run_host_transaction_runtime_hook(
            impl, CFLOW_STATECHART_HOST_PREPARE_TRIGGER, &observed) ==
        CFLOW_STATECHART_HOST_FATAL)
        return -1;
    return driver_try_trigger(impl, &trigger);
}

static void settle_quiescent_macrostep(cflow_statechart_instance_impl *impl) {
    cflow_waker waker = {0};
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome == STATECHART_TERMINAL_NONE &&
        impl->exit_requested) {
        impl->driver_repost = true;
    } else if (impl->terminal_outcome == STATECHART_TERMINAL_NONE &&
        root_configuration_complete(impl)) {
        (void)win_terminal_locked(
            impl, STATECHART_TERMINAL_CLEAN_DONE,
            CFLOW_STATECHART_INSTANCE_OK, NULL, true,
            CFLOW_STATECHART_INSTANCE_OK, &waker);
    } else if (impl->terminal_outcome == STATECHART_TERMINAL_NONE) {
        settle_external_locked(impl, CFLOW_STATECHART_INSTANCE_OK);
        if (impl->macrostep_active) counter_increment(&impl->macrosteps);
        impl->macrostep_active = false;
        impl->macrostep_has_external = false;
        impl->macrostep_microsteps = 0u;
        impl->skip_to_external = true;
    }
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
}

static void invoke_before_root_completion_settle_test_hook(
    cflow_statechart_instance_impl *impl) {
    void (*before_settle)(void *) = NULL;
    void *hook_user = NULL;
    cmeta_mutex_lock(&impl->lock);
    before_settle = impl->test_hooks.before_root_completion_settle;
    hook_user = impl->test_hooks.user;
    cmeta_mutex_unlock(&impl->lock);
    if (before_settle != NULL) before_settle(hook_user);
}

/* Root completion is a hard semantic boundary. Once its queue row reaches
 * the head, only already-staged completion work may run until a completion
 * transition leaves the completed root configuration or the queue drains. */
static bool drain_root_completions(
    cflow_statechart_instance_impl *impl) {
    cflow_machine_state_id completion = 0u;
    bool complete, pending;
    int result;

    cmeta_mutex_lock(&impl->lock);
    complete = root_configuration_complete(impl);
    if (impl->root_completion_draining && !complete)
        impl->root_completion_draining = false;
    if (!impl->root_completion_draining) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    cmeta_mutex_unlock(&impl->lock);

    if (!pop_completion(impl, &completion)) {
        cmeta_mutex_lock(&impl->lock);
        impl->root_completion_draining = false;
        cmeta_mutex_unlock(&impl->lock);
        invoke_before_root_completion_settle_test_hook(impl);
        settle_quiescent_macrostep(impl);
        return true;
    }

    result = driver_try_completion(impl, completion);
    if (result != 0) return true;

    cmeta_mutex_lock(&impl->lock);
    complete = root_configuration_complete(impl);
    pending = impl->completion_count != 0u;
    if (!complete) {
        impl->root_completion_draining = false;
    } else if (pending) {
        impl->driver_repost = true;
    } else {
        impl->root_completion_draining = false;
    }
    cmeta_mutex_unlock(&impl->lock);
    if (!complete) return false;
    if (pending) return true;
    invoke_before_root_completion_settle_test_hook(impl);
    settle_quiescent_macrostep(impl);
    return true;
}

static void statechart_driver_run(void *user) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)user;
    cflow_statechart_selection_trigger trigger = {
        CFLOW_STATECHART_TRIGGER_EVENTLESS, NULL, 0u};
    cflow_event_view event = {0};
    cflow_machine_state_id completion = 0u;
    cflow_event_id event_id = 0u;
    const cmeta_type_desc *event_type = NULL;
    cflow_mailbox_status mailbox_status;
    uint64_t origin_token = UINT64_C(0);
    cflow_statechart_host_result host_result;
    int result;
    bool terminal, exit_requested, skip_to_external, internal_work;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    impl->driver_repost = false;
    terminal = impl->terminal_outcome != STATECHART_TERMINAL_NONE;
    skip_to_external = impl->skip_to_external;
    impl->skip_to_external = false;
    cmeta_mutex_unlock(&impl->lock);
    if (terminal) return;
    if (impl->initial_configuration_pending &&
        execute_initial_entry_actions(impl) !=
            CFLOW_STATECHART_INSTANCE_OK)
        return;

    cmeta_mutex_lock(&impl->lock);
    terminal = impl->terminal_outcome != STATECHART_TERMINAL_NONE;
    exit_requested = impl->exit_requested;
    cmeta_mutex_unlock(&impl->lock);
    if (terminal) return;
    if (exit_requested) {
        (void)execute_controlled_exit(impl);
        return;
    }

    if (drain_root_completions(impl)) return;
    if (skip_to_external) goto external_admission;

    result = driver_try_trigger(impl, &trigger);
    if (result != 0) return;

    /* Root completion may transition away; otherwise it is a hard boundary
     * that entry-raised Events must not cross. */
    if (root_completion_ready(impl)) {
        if (impl->ir->states[impl->ir->root].kind !=
            CFLOW_STATECHART_FINAL) {
            cmeta_mutex_lock(&impl->lock);
            impl->root_completion_draining = true;
            cmeta_mutex_unlock(&impl->lock);
            if (drain_root_completions(impl)) return;
        }
        settle_quiescent_macrostep(impl);
        return;
    }

    if (pop_internal_event(impl, &event, &origin_token)) {
        const cflow_statechart_observed_event observed = {
            CFLOW_STATECHART_OBSERVED_INTERNAL, &event, 0u,
            origin_token};
        if (run_host_transaction_runtime_hook(
                impl, CFLOW_STATECHART_HOST_PREPARE_TRIGGER, &observed) ==
            CFLOW_STATECHART_HOST_FATAL)
            return;
        trigger = (cflow_statechart_selection_trigger){
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        result = driver_try_trigger(impl, &trigger);
        if (result != 0) return;
        cmeta_mutex_lock(&impl->lock);
        impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    if (pop_adapter_internal_event(impl, &event)) {
        const cflow_statechart_observed_event observed = {
            CFLOW_STATECHART_OBSERVED_INTERNAL, &event, 0u, 0u};
        if (run_host_transaction_runtime_hook(
                impl, CFLOW_STATECHART_HOST_PREPARE_TRIGGER, &observed) ==
            CFLOW_STATECHART_HOST_FATAL)
            return;
        trigger = (cflow_statechart_selection_trigger){
            CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
        result = driver_try_trigger(impl, &trigger);
        if (result != 0) return;
        cmeta_mutex_lock(&impl->lock);
        impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    if (pop_completion(impl, &completion)) {
        result = driver_try_completion(impl, completion);
        if (result != 0) return;
        cmeta_mutex_lock(&impl->lock);
        impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }

    cmeta_mutex_lock(&impl->lock);
    terminal = impl->macrostep_active;
    cmeta_mutex_unlock(&impl->lock);
    if (terminal) {
        if (run_host_transaction_runtime_hook(
                impl, CFLOW_STATECHART_HOST_PREPARE_QUIESCENCE, NULL) ==
            CFLOW_STATECHART_HOST_FATAL)
            return;
        cmeta_mutex_lock(&impl->lock);
        internal_work = impl->internal_event_count != 0u ||
            impl->adapter_internal_pending != 0u;
        if (internal_work) impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        if (internal_work) return;
        settle_quiescent_macrostep(impl);
        cmeta_mutex_lock(&impl->lock);
        terminal = impl->done;
        if (!terminal) impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }

external_admission:
    if (!impl->external_mailbox_initialized) return;
    cmeta_mutex_lock(&impl->lock);
    {
        void (*before_receive)(void *) =
            impl->test_hooks.before_external_receive;
        void *hook_user = impl->test_hooks.user;
        cmeta_mutex_unlock(&impl->lock);
        if (before_receive != NULL) before_receive(hook_user);
    }
    cmeta_mutex_lock(&impl->lock);
    if (impl->adapter_internal_pending != 0u) {
        impl->skip_to_external = false;
        impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    if (impl->terminal_outcome != STATECHART_TERMINAL_NONE) {
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    mailbox_status = cflow_mailbox_try_receive(
        &impl->external_mailbox, &event_id, &event_type,
        impl->driver_event_payload, impl->driver_event_capacity);
    if (mailbox_status == CFLOW_MAILBOX_EMPTY) {
        impl->skip_to_external = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    if (mailbox_status == CFLOW_MAILBOX_CLOSED ||
        mailbox_status == CFLOW_MAILBOX_CANCELLED) {
        cmeta_mutex_unlock(&impl->lock);
        latch_terminal_failure(
            impl, CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION,
            "Statechart external mailbox closed without a terminal winner");
        return;
    }
    if (mailbox_status != CFLOW_MAILBOX_OK) {
        cmeta_mutex_unlock(&impl->lock);
        latch_terminal_failure(
            impl, CFLOW_STATECHART_INSTANCE_INVALID_CONFIGURATION,
            "Statechart external mailbox receive failed");
        return;
    }
    if (impl->external_pending != 0u) --impl->external_pending;
    if (impl->external_origin_tokens != NULL) {
        origin_token = impl->external_origin_tokens[
            impl->external_source_head];
        impl->external_origin_tokens[
            impl->external_source_head] = UINT64_C(0);
        impl->external_source_head =
            (impl->external_source_head + 1u) %
            impl->external_event_capacity;
    }
    event = (cflow_event_view){
        event_id, event_type, impl->driver_event_payload};
    impl->external_in_flight = true;
    impl->external_in_flight_origin_token = origin_token;
    impl->macrostep_active = true;
    impl->macrostep_has_external = true;
    impl->macrostep_microsteps = 0u;
    cmeta_mutex_unlock(&impl->lock);
    {
        const cflow_statechart_observed_event observed = {
            CFLOW_STATECHART_OBSERVED_EXTERNAL, &event, 0u,
            origin_token};
        host_result = run_host_transaction_runtime_hook(
            impl, CFLOW_STATECHART_HOST_PREPARE_TRIGGER, &observed);
    }
    if (host_result == CFLOW_STATECHART_HOST_FATAL)
        return;
    cmeta_mutex_lock(&impl->lock);
    internal_work = impl->internal_event_count != 0u ||
        impl->adapter_internal_pending != 0u;
    cmeta_mutex_unlock(&impl->lock);
    if (host_result == CFLOW_STATECHART_HOST_DROP) {
        cmeta_mutex_lock(&impl->lock);
        impl->external_in_flight_dropped = true;
        impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    trigger = (cflow_statechart_selection_trigger){
        CFLOW_STATECHART_TRIGGER_EVENT, &event, 0u};
    result = driver_try_trigger(impl, &trigger);
    if (result != 0) return;
    if (internal_work || impl->hooks.on_host_transaction != NULL) {
        cmeta_mutex_lock(&impl->lock);
        impl->driver_repost = true;
        cmeta_mutex_unlock(&impl->lock);
        return;
    }
    settle_quiescent_macrostep(impl);
    cmeta_mutex_lock(&impl->lock);
    if (!impl->done) impl->driver_repost = true;
    cmeta_mutex_unlock(&impl->lock);
}

static void statechart_driver_cancel(void *user) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)user;
    cflow_waker waker = {0};
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome == STATECHART_TERMINAL_NONE) {
        claim_external_for_failure_locked(impl);
        (void)win_terminal_locked(
            impl, STATECHART_TERMINAL_ERROR,
            CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED,
            "Statechart SerialExecutor cancelled a queued quantum",
            true, CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED, &waker);
    }
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
}

static void statechart_driver_finalize(void *user) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)user;
    bool repost;
    void (*before_repost)(void *) = NULL;
    void *hook_user = NULL;
    cmeta_mutex_lock(&impl->lock);
    impl->driver_scheduled = false;
    repost = impl->driver_repost && !impl->done && impl->error == NULL &&
        !impl->microstep_pending;
    impl->driver_repost = false;
    if (!repost) release_instance_task_locked(impl);
    if (repost) {
        before_repost = impl->test_hooks.before_driver_repost;
        hook_user = impl->test_hooks.user;
    }
    cmeta_mutex_unlock(&impl->lock);
    if (repost) {
        if (before_repost != NULL) before_repost(hook_user);
        (void)schedule_statechart_driver_reserved(impl);
    }
}

static cflow_admission_status schedule_statechart_driver_impl(
    cflow_statechart_instance_impl *impl, bool transfer_reservation) {
    const cflow_executor_task task = {
        statechart_driver_run, statechart_driver_cancel,
        statechart_driver_finalize, impl};
    cflow_admission_status admission;
    if (impl == NULL) return CFLOW_ADMISSION_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    if (impl->done || impl->error != NULL) {
        if (transfer_reservation) release_instance_task_locked(impl);
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_CLOSED;
    }
    if (impl->driver_scheduled || impl->microstep_pending) {
        impl->driver_repost = true;
        if (transfer_reservation) release_instance_task_locked(impl);
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_ACCEPTED;
    }
    if (!transfer_reservation && !reserve_instance_task_locked(impl)) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_ADMISSION_FULL;
    }
    impl->driver_scheduled = true;
    cmeta_mutex_unlock(&impl->lock);
    admission = cflow_executor_try_post_task(impl->executor, &task);
    if (admission != CFLOW_ADMISSION_ACCEPTED) {
        const cflow_statechart_instance_status failure =
            admission == CFLOW_ADMISSION_FULL
                ? CFLOW_STATECHART_INSTANCE_EXECUTOR_FULL
                : CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED;
        const char *message = admission == CFLOW_ADMISSION_FULL
            ? "Statechart SerialExecutor is full"
            : "Statechart SerialExecutor is closed";
        cflow_waker waker = {0};
        cmeta_mutex_lock(&impl->lock);
        impl->driver_scheduled = false;
        if (impl->terminal_outcome == STATECHART_TERMINAL_NONE) {
            claim_external_for_failure_locked(impl);
            (void)win_terminal_locked(
                impl, STATECHART_TERMINAL_ERROR, failure, message,
                true, failure, &waker);
        }
        if (!transfer_reservation) release_instance_task_locked(impl);
        cmeta_mutex_unlock(&impl->lock);
        finish_terminal_side_effects(impl, waker);
        if (transfer_reservation) {
            cmeta_mutex_lock(&impl->lock);
            release_instance_task_locked(impl);
            cmeta_mutex_unlock(&impl->lock);
        }
    }
    return admission;
}

static cflow_admission_status schedule_statechart_driver(
    cflow_statechart_instance_impl *impl) {
    return schedule_statechart_driver_impl(impl, false);
}

static cflow_admission_status schedule_statechart_driver_reserved(
    cflow_statechart_instance_impl *impl) {
    return schedule_statechart_driver_impl(impl, true);
}

bool cflow_statechart_instance_get_microstep_stats_internal(
    const cflow_statechart_instance *instance,
    cflow_statechart_microstep_stats *out) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_statechart_microstep_stats snapshot;
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    snapshot = (cflow_statechart_microstep_stats){
        impl->microstep_accepted, impl->microstep_completed,
        impl->microstep_failed, impl->microstep_cancelled,
        impl->microstep_finalized, impl->microstep_result,
        impl->internal_event_capacity, impl->internal_event_count};
    cmeta_mutex_unlock(&impl->lock);
    *out = snapshot;
    return true;
}

bool cflow_statechart_instance_set_test_hooks_internal(
    cflow_statechart_instance *instance,
    const cflow_statechart_instance_test_hooks *hooks) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    if (impl == NULL || hooks == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    if (impl->driver_scheduled || impl->microstep_pending ||
        impl->terminal_outcome != STATECHART_TERMINAL_NONE) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    impl->test_hooks = *hooks;
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

cflow_statechart_instance_status
cflow_statechart_instance_copy_internal_event_internal(
    const cflow_statechart_instance *instance,
    size_t position,
    cflow_event_id *out_id,
    const cmeta_type_desc **out_type,
    void *out_payload,
    size_t payload_capacity) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    size_t slot, type_index;
    const cmeta_type_desc *type;
    if (out_id != NULL) *out_id = 0u;
    if (out_type != NULL) *out_type = NULL;
    if (impl == NULL || out_id == NULL || out_type == NULL ||
        out_payload == NULL)
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    if (position >= impl->internal_event_count) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_INTERNAL_EVENT_INVALID;
    }
    slot = (impl->internal_event_head + position) %
        impl->internal_event_capacity;
    type_index = impl->internal_event_slots[slot].type_index;
    type = impl->ir->events[type_index].payload_type;
    if (payload_capacity < type->size ||
        ((uintptr_t)out_payload % type->align) != 0u) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    }
    memcpy(out_payload,
           impl->internal_event_payloads + slot * impl->event_payload_stride,
           type->size);
    *out_id = impl->ir->events[type_index].id;
    *out_type = type;
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_STATECHART_INSTANCE_OK;
}

cflow_statechart_snapshot_status
cflow_statechart_instance_copy_history_internal(
    const cflow_statechart_instance *instance,
    cflow_machine_state_id history,
    cflow_machine_state_id *out_states,
    size_t state_capacity,
    size_t *out_state_count) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    size_t history_index, slot, required, index, written = 0u;
    const unsigned char *bits;
    if (impl == NULL || out_state_count == NULL) {
        return CFLOW_STATECHART_SNAPSHOT_INVALID_ARGUMENT;
    }
    history_index = find_state_index(impl->ir, history);
    if (history_index == SIZE_MAX ||
        (impl->ir->states[history_index].kind !=
             CFLOW_STATECHART_HISTORY_SHALLOW &&
         impl->ir->states[history_index].kind !=
             CFLOW_STATECHART_HISTORY_DEEP))
        return CFLOW_STATECHART_SNAPSHOT_INVALID_ARGUMENT;
    slot = impl->history_slots[history_index];
    cmeta_mutex_lock(&impl->lock);
    required = impl->history_counts[impl->published][slot];
    if (state_capacity < required) {
        *out_state_count = required;
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_SNAPSHOT_TOO_SMALL;
    }
    if (required != 0u && out_states == NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_SNAPSHOT_INVALID_ARGUMENT;
    }
    bits = impl->history_bits[impl->published] + slot * impl->bitset_bytes;
    for (index = 0u; index < impl->ir->state_count; ++index) {
        const size_t state = impl->ir->document_order_indices[index];
        if (bit_test(bits, state))
            out_states[written++] = impl->ir->states[state].id;
    }
    *out_state_count = written;
    cmeta_mutex_unlock(&impl->lock);
    return written == required
        ? CFLOW_STATECHART_SNAPSHOT_OK
        : CFLOW_STATECHART_SNAPSHOT_INVALID_ARGUMENT;
}

static cflow_statechart_instance_status statechart_instance_init_with_hook(
    cflow_statechart_instance *instance,
    const cflow_statechart_instance_config *config,
    cflow_statechart_init_wait_hook_internal before_wait,
    void *hook_user) {
    cflow_statechart_instance_impl *impl;
    const cflow_statechart_impl *ir;
    cflow_statechart_storage_requirements requirements;
    cflow_statechart_instance_status status;
    size_t effective_storage_limit, required_storage;
    size_t timer_storage_bytes = 0u, effect_storage_bytes = 0u;
    size_t adapter_internal_storage_bytes = 0u;
    size_t external_source_storage_bytes = 0u;
    size_t event_stride = 0u;
    if (instance == NULL || config == NULL || instance->impl != NULL ||
        config->statechart == NULL || config->initial_state == NULL ||
        config->executor == NULL || config->external_event_capacity == 0u ||
        config->internal_event_capacity == 0u ||
        config->completion_capacity == 0u ||
        config->microstep_limit == 0u ||
        ((config->clock == NULL) != (config->timer_capacity == 0u)) ||
        (config->clock != NULL && !cflow_clock_valid(config->clock)) ||
        !instance_hooks_shape_valid(config->hooks))
        return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    if (!cflow_executor_valid(config->executor) ||
        !cflow_executor_has(config->executor, CMETA_EXEC_CAP_SERIAL) ||
        cflow_executor_has(config->executor, CMETA_EXEC_CAP_MANUAL) ||
        cflow_executor_is_current_internal(config->executor))
        return CFLOW_STATECHART_INSTANCE_INVALID_EXECUTOR;

    ir = cflow_statechart_internal_get(config->statechart);
    if (ir == NULL || !cmeta_type_desc_valid(ir->state_type) ||
        ir->state_type->size == 0u || ir->state_type->align == 0u ||
        (ir->state_type->align & (ir->state_type->align - 1u)) !=
            0u ||
        ir->state_type->align > _Alignof(cmeta_capture_storage) ||
        !cflow_value_type_supported(ir->state_type))
        return CFLOW_STATECHART_INSTANCE_UNSUPPORTED_TYPE;
    if (!binding_rows_shape_valid(ir, config))
        return CFLOW_STATECHART_INSTANCE_BINDING_MISMATCH;
    if (config->max_storage_bytes >
        (size_t)CFLOW_STATECHART_MAX_INSTANCE_BYTES)
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    status = storage_requirements_for_ir(
        ir, config->external_event_capacity,
        config->internal_event_capacity, config->completion_capacity,
        &requirements);
    if (status != CFLOW_STATECHART_INSTANCE_OK) return status;
    required_storage = requirements.total_bytes;
    if (config->hooks != NULL &&
        (!checked_multiply(config->external_event_capacity,
                           sizeof(uint64_t),
                           &external_source_storage_bytes) ||
         !checked_add(required_storage, external_source_storage_bytes,
                      &required_storage)))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    if (!checked_multiply(config->effect_capacity,
                          sizeof(cflow_statechart_effect_ticket),
                          &effect_storage_bytes) ||
        !checked_add(required_storage, effect_storage_bytes,
                     &required_storage))
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    if (config->adapter_internal_event_capacity != 0u) {
        if (ir->event_count == 0u)
            return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
        if (!cflow_mailbox_storage_requirements_internal(
                ir->events, ir->event_count,
                config->adapter_internal_event_capacity,
                &adapter_internal_storage_bytes) ||
            !checked_add(required_storage,
                         adapter_internal_storage_bytes,
                         &required_storage))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    }
    if (config->timer_capacity != 0u) {
        status = measure_event_storage(ir, &event_stride);
        if (status != CFLOW_STATECHART_INSTANCE_OK) return status;
        if (event_stride == 0u)
            return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
        if (!cflow_timer_event_queue_storage_requirements_internal(
                event_stride, config->timer_capacity,
                &timer_storage_bytes) ||
            !checked_add(required_storage, timer_storage_bytes,
                         &required_storage))
            return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    }
    effective_storage_limit = config->max_storage_bytes != 0u
        ? config->max_storage_bytes
        : (size_t)CFLOW_STATECHART_MAX_INSTANCE_BYTES;
    if (required_storage > effective_storage_limit)
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;

    impl = (cflow_statechart_instance_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
    impl->statechart = config->statechart;
    impl->ir = ir;
    impl->executor = config->executor;
    copy_instance_hooks(&impl->hooks, config->hooks);
    impl->hook_user = config->hook_user;
    impl->internal_event_capacity = requirements.internal_event_capacity;
    impl->completion_capacity = requirements.completion_capacity;
    impl->effect_capacity = config->effect_capacity;
    impl->microstep_limit = config->microstep_limit;
    impl->microstep_result = CFLOW_STATECHART_INSTANCE_OK;
    impl->last_status = CFLOW_STATECHART_INSTANCE_OK;
    if (!acquire_instance_token(&impl->instance_token)) {
        instance_impl_free(impl);
        return CFLOW_STATECHART_INSTANCE_LIMIT_EXCEEDED;
    }
    status = copy_bindings(impl, config);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }
    status = allocate_storage(impl, &requirements);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }
    if (external_source_storage_bytes != 0u) {
        impl->external_origin_tokens = (uint64_t *)calloc(
            config->external_event_capacity, sizeof(uint64_t));
        if (impl->external_origin_tokens == NULL) {
            instance_impl_free(impl);
            return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
        }
        impl->external_event_capacity = config->external_event_capacity;
    }
    cmeta_mutex_init(&impl->lock);
    cmeta_cond_init(&impl->tasks_changed);
    if (impl->lock == NULL || impl->tasks_changed == NULL) {
        instance_impl_free(impl);
        return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
    }
    if (ir->event_count != 0u) {
        const cflow_mailbox_status mailbox_status = cflow_mailbox_init(
            &impl->external_mailbox, ir->events, ir->event_count,
            config->external_event_capacity);
        if (mailbox_status != CFLOW_MAILBOX_OK) {
            instance_impl_free(impl);
            return mailbox_status == CFLOW_MAILBOX_ALLOCATION_FAILED
                ? CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED
                : CFLOW_STATECHART_INSTANCE_UNSUPPORTED_TYPE;
        }
        impl->external_mailbox_initialized = true;
    }
    if (config->adapter_internal_event_capacity != 0u) {
        const cflow_mailbox_status mailbox_status = cflow_mailbox_init(
            &impl->adapter_internal_mailbox, ir->events, ir->event_count,
            config->adapter_internal_event_capacity);
        if (mailbox_status != CFLOW_MAILBOX_OK) {
            instance_impl_free(impl);
            return mailbox_status == CFLOW_MAILBOX_ALLOCATION_FAILED
                ? CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED
                : CFLOW_STATECHART_INSTANCE_UNSUPPORTED_TYPE;
        }
        impl->adapter_internal_mailbox_initialized = true;
    }
    if (config->timer_capacity != 0u) {
        const cflow_timer_event_target_internal target = {
            impl, statechart_timer_contract, statechart_timer_send};
        const cflow_timer_event_status timer_status =
            cflow_timer_event_queue_init_target_internal(
                &impl->timers, config->clock, config->timer_capacity,
                target, impl->driver_event_capacity);
        if (timer_status != CFLOW_TIMER_EVENT_OK) {
            instance_impl_free(impl);
            return timer_status == CFLOW_TIMER_EVENT_ALLOCATION_FAILED
                ? CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED
                : CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
        }
        impl->timers_initialized = true;
    }
    if (!copy_state_value(
            impl, impl->extended_states[0],
            &impl->extended_state_live[0], config->initial_state) ||
        !copy_state_value(
            impl, impl->extended_states[1],
            &impl->extended_state_live[1], config->initial_state)) {
        instance_impl_free(impl);
        return CFLOW_STATECHART_INSTANCE_ALLOCATION_FAILED;
    }
    status = build_initial_configuration(impl, 1u);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }
    impl->initial_configuration_pending = true;
    impl->macrostep_active = true;
    {
        const cflow_admission_status admission =
            schedule_statechart_driver(impl);
        if (admission != CFLOW_ADMISSION_ACCEPTED) {
            instance_impl_free(impl);
            return admission == CFLOW_ADMISSION_FULL
                ? CFLOW_STATECHART_INSTANCE_EXECUTOR_FULL
                : CFLOW_STATECHART_INSTANCE_EXECUTOR_CLOSED;
        }
    }
    if (before_wait != NULL) before_wait(hook_user);
    status = wait_instance_tasks(impl);
    if (status != CFLOW_STATECHART_INSTANCE_OK) {
        instance_impl_free(impl);
        return status;
    }
    if (impl->error != NULL) {
        status = impl->last_status;
        instance_impl_free(impl);
        return status != CFLOW_STATECHART_INSTANCE_OK
            ? status : CFLOW_STATECHART_INSTANCE_ACTION_FAILED;
    }
    instance->impl = impl;
    return CFLOW_STATECHART_INSTANCE_OK;
}

cflow_statechart_instance_status cflow_statechart_instance_init(
    cflow_statechart_instance *instance,
    const cflow_statechart_instance_config *config) {
    return statechart_instance_init_with_hook(
        instance, config, NULL, NULL);
}

cflow_statechart_instance_status cflow_statechart_instance_init_test_internal(
    cflow_statechart_instance *instance,
    const cflow_statechart_instance_config *config,
    cflow_statechart_init_wait_hook_internal before_wait,
    void *hook_user) {
    return statechart_instance_init_with_hook(
        instance, config, before_wait, hook_user);
}

cflow_statechart_snapshot_status
cflow_statechart_instance_copy_configuration(
    const cflow_statechart_instance *instance,
    cflow_machine_state_id *out_states,
    size_t state_capacity,
    size_t *out_state_count,
    uint64_t *out_version) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    size_t index, required, published;
    if (impl == NULL || out_state_count == NULL || out_version == NULL)
        return CFLOW_STATECHART_SNAPSHOT_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    published = impl->published;
    required = impl->configurations[published].state_count;
    if (state_capacity < required) {
        *out_state_count = required;
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_SNAPSHOT_TOO_SMALL;
    }
    if (required != 0u && out_states == NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return CFLOW_STATECHART_SNAPSHOT_INVALID_ARGUMENT;
    }
    for (index = 0u; index < required; ++index) {
        const size_t state = impl->configurations[published].states[index];
        out_states[index] = impl->ir->states[state].id;
    }
    *out_state_count = required;
    *out_version = impl->configuration_version;
    cmeta_mutex_unlock(&impl->lock);
    return CFLOW_STATECHART_SNAPSHOT_OK;
}

cflow_mailbox_status cflow_statechart_instance_try_send(
    cflow_statechart_instance *instance, const cflow_event_view *event) {
    return cflow_statechart_instance_try_send_tagged(
        instance, event, UINT64_C(0));
}

cflow_mailbox_status cflow_statechart_instance_try_send_tagged(
    cflow_statechart_instance *instance, const cflow_event_view *event,
    uint64_t origin_token) {
    cflow_waker waker = {0};
    cflow_mailbox_status status = cflow_statechart_instance_try_send_detach_internal(
        instance, event, origin_token, &waker);
    invoke_detached_waker(waker);
    return status;
}

static void statechart_send_wake(void *user) {
    (void)schedule_statechart_driver((cflow_statechart_instance_impl *)user);
}

cflow_mailbox_status cflow_statechart_instance_try_send_detach_internal(
    cflow_statechart_instance *instance, const cflow_event_view *event,
    uint64_t origin_token, cflow_waker *out_waker) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_mailbox_status status;
    bool cancelled;
    if (out_waker != NULL) *out_waker = (cflow_waker){0};
    if (impl == NULL || out_waker == NULL || !impl->external_mailbox_initialized)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    cancelled = impl->cancelled || impl->exit_requested;
    if (impl->closed || impl->done || impl->error != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return cancelled ? CFLOW_MAILBOX_CANCELLED : CFLOW_MAILBOX_CLOSED;
    }
    status = cflow_mailbox_try_send(&impl->external_mailbox, event);
    if (status == CFLOW_MAILBOX_OK) {
        if (impl->external_origin_tokens != NULL) {
            impl->external_origin_tokens[
                impl->external_source_tail] = origin_token;
            impl->external_source_tail =
                (impl->external_source_tail + 1u) %
                impl->external_event_capacity;
        }
        counter_increment(&impl->external_accepted);
        ++impl->external_pending;
        /* A queued/running driver owns progress; retain its existing rerun
         * signal without another scheduler call on every ingress message. */
        if (impl->driver_scheduled || impl->microstep_pending)
            impl->driver_repost = true;
        else
            *out_waker = (cflow_waker){statechart_send_wake, impl};
    }
    cancelled = impl->cancelled || impl->exit_requested;
    cmeta_mutex_unlock(&impl->lock);
    if (status != CFLOW_MAILBOX_OK) {
        if (status == CFLOW_MAILBOX_CANCELLED)
            return cancelled ? CFLOW_MAILBOX_CANCELLED
                             : CFLOW_MAILBOX_CLOSED;
        return status;
    }
    return CFLOW_MAILBOX_OK;
}

cflow_mailbox_status cflow_statechart_instance_try_send_internal(
    cflow_statechart_instance *instance, const cflow_event_view *event) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_mailbox_status status;
    bool cancelled;
    if (impl == NULL || !impl->adapter_internal_mailbox_initialized)
        return CFLOW_MAILBOX_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    cancelled = impl->cancelled || impl->exit_requested;
    if (impl->closed || impl->done || impl->error != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return cancelled ? CFLOW_MAILBOX_CANCELLED : CFLOW_MAILBOX_CLOSED;
    }
    status = cflow_mailbox_try_send(
        &impl->adapter_internal_mailbox, event);
    if (status == CFLOW_MAILBOX_OK) {
        counter_increment(&impl->adapter_internal_accepted);
        ++impl->adapter_internal_pending;
        impl->macrostep_active = true;
        impl->skip_to_external = false;
    }
    cancelled = impl->cancelled || impl->exit_requested;
    cmeta_mutex_unlock(&impl->lock);
    if (status != CFLOW_MAILBOX_OK) {
        if (status == CFLOW_MAILBOX_CANCELLED)
            return cancelled ? CFLOW_MAILBOX_CANCELLED
                             : CFLOW_MAILBOX_CLOSED;
        return status;
    }
    (void)schedule_statechart_driver(impl);
    return CFLOW_MAILBOX_OK;
}

static bool statechart_timer_scope_active_locked(
    const cflow_statechart_instance_impl *impl,
    cflow_machine_state_id scope) {
    const size_t state = find_state_index(impl->ir, scope);
    return state != SIZE_MAX && !pseudo_kind(impl->ir->states[state].kind) &&
        bit_test(impl->configurations[impl->published].bits, state);
}

cflow_timer_event_schedule_result cflow_statechart_instance_try_schedule_at(
    cflow_statechart_instance *instance,
    cflow_machine_state_id scope,
    cflow_deadline deadline,
    const cflow_event_view *event) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_timer_event_schedule_result result = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u};
    if (impl == NULL || scope == 0u || !impl->timers_initialized)
        return result;
    cmeta_mutex_lock(&impl->lock);
    if (impl->closed ||
        impl->terminal_outcome != STATECHART_TERMINAL_NONE) {
        result.status = CFLOW_TIMER_EVENT_CLOSED;
    } else if (statechart_timer_scope_active_locked(impl, scope)) {
        result = cflow_timer_event_queue_try_schedule_scoped_at(
            &impl->timers, deadline, event, scope);
    }
    cmeta_mutex_unlock(&impl->lock);
    return result;
}

cflow_timer_event_schedule_result cflow_statechart_instance_try_schedule_after(
    cflow_statechart_instance *instance,
    cflow_machine_state_id scope,
    cflow_duration delay,
    const cflow_event_view *event) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_timer_event_schedule_result result = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u};
    if (impl == NULL || scope == 0u || !impl->timers_initialized)
        return result;
    cmeta_mutex_lock(&impl->lock);
    if (impl->closed ||
        impl->terminal_outcome != STATECHART_TERMINAL_NONE) {
        result.status = CFLOW_TIMER_EVENT_CLOSED;
    } else if (statechart_timer_scope_active_locked(impl, scope)) {
        result = cflow_timer_event_queue_try_schedule_scoped_after(
            &impl->timers, delay, event, scope);
    }
    cmeta_mutex_unlock(&impl->lock);
    return result;
}

cflow_timer_event_status cflow_statechart_instance_cancel_timer(
    cflow_statechart_instance *instance,
    cflow_timer_event_id timer_id) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_timer_event_status status;
    if (impl == NULL || !impl->timers_initialized)
        return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
    cmeta_mutex_lock(&impl->lock);
    status = impl->closed ||
        impl->terminal_outcome != STATECHART_TERMINAL_NONE
        ? CFLOW_TIMER_EVENT_CLOSED
        : cflow_timer_event_queue_cancel(&impl->timers, timer_id);
    cmeta_mutex_unlock(&impl->lock);
    return status;
}

cflow_timer_event_fire_result cflow_statechart_instance_run_one_ready_timer(
    cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    const cflow_timer_event_fire_result invalid = {
        CFLOW_TIMER_EVENT_FIRE_INVALID_ARGUMENT, 0u,
        CFLOW_MAILBOX_INVALID_ARGUMENT};
    return impl != NULL && impl->timers_initialized
        ? cflow_timer_event_queue_run_one_ready(&impl->timers)
        : invalid;
}

bool cflow_statechart_instance_claim_timer_internal(
    cflow_statechart_instance *instance,
    cflow_timer_event_claim *claim,
    cflow_timer_event_fire_result *out_result) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    return impl != NULL && impl->timers_initialized &&
        cflow_timer_event_queue_claim_one_ready(
            &impl->timers, claim, out_result);
}

static bool statechart_terminal_wait_arm(void *state, cflow_waker waker) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    bool ready;
    if (impl == NULL || waker.wake == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    ready = impl->done || impl->error != NULL;
    if (!ready && impl->downstream_waiter.wake != NULL) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    if (!ready) impl->downstream_waiter = waker;
    cmeta_mutex_unlock(&impl->lock);
    if (ready) invoke_detached_waker(waker);
    return true;
}

static void statechart_terminal_wait_cancel(void *state) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    impl->downstream_waiter = (cflow_waker){0};
    cmeta_mutex_unlock(&impl->lock);
}

CMETA_IMPLEMENTS(cflow_waitable, cflow_statechart_terminal_waitable, 0,
    .arm = statechart_terminal_wait_arm,
    .cancel = statechart_terminal_wait_cancel
);

static cflow_step statechart_terminal_resume(
    void *state, cflow_publish_context *context, void *out_value) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    const char *error;
    bool done;
    (void)context;
    if (impl == NULL || out_value == NULL)
        return (cflow_step){
            CFLOW_STEP_ERROR, {0},
            "Statechart terminal adapter is invalid"};
    cmeta_mutex_lock(&impl->lock);
    error = impl->error;
    done = impl->done;
    cmeta_mutex_unlock(&impl->lock);
    if (error != NULL)
        return (cflow_step){CFLOW_STEP_ERROR, {0}, error};
    if (done)
        return (cflow_step){CFLOW_STEP_DONE, {0}, NULL};
    return (cflow_step){
        CFLOW_STEP_WAIT,
        cflow_statechart_terminal_waitable_as_cflow_waitable(impl),
        NULL};
}

static void statechart_terminal_cancel(void *state) {
    cflow_statechart_instance instance = {state};
    cflow_statechart_instance_cancel(&instance);
}

static void statechart_terminal_detach(void *state) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    if (impl == NULL) return;
    statechart_terminal_cancel(impl);
    cmeta_mutex_lock(&impl->lock);
    impl->downstream_waiter = (cflow_waker){0};
    impl->terminal_waiter = (cflow_waker){0};
    impl->adapter_attached = false;
    cmeta_mutex_unlock(&impl->lock);
}

static const cflow_resumable_ops statechart_terminal_resumable_ops = {
    statechart_terminal_resume,
    statechart_terminal_cancel,
    statechart_terminal_detach
};

static const char *statechart_terminal_source_name(void *state) {
    (void)state;
    return "statechart-terminal";
}

static const cmeta_type_desc *statechart_terminal_source_type(void *state) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    return impl != NULL ? impl->ir->state_type : NULL;
}

static void statechart_terminal_source_bind(
    void *state, cflow_waker waker) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    bool terminal;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    terminal = impl->done || impl->error != NULL;
    impl->terminal_waiter = terminal ? (cflow_waker){0} : waker;
    cmeta_mutex_unlock(&impl->lock);
    if (terminal) invoke_detached_waker(waker);
}

static cflow_publisher_terminal statechart_terminal_source_poll(
    void *state, const char **out_error) {
    cflow_statechart_instance_impl *impl =
        (cflow_statechart_instance_impl *)state;
    cflow_publisher_terminal result = CFLOW_PUBLISHER_OPEN;
    if (out_error != NULL) *out_error = NULL;
    if (impl == NULL) {
        if (out_error != NULL)
            *out_error = "Statechart terminal Source is invalid";
        return CFLOW_PUBLISHER_ERROR;
    }
    cmeta_mutex_lock(&impl->lock);
    if (impl->error != NULL) {
        result = CFLOW_PUBLISHER_ERROR;
        if (out_error != NULL) *out_error = impl->error;
    } else if (impl->done) {
        result = CFLOW_PUBLISHER_DONE;
    }
    cmeta_mutex_unlock(&impl->lock);
    return result;
}

CMETA_IMPLEMENTS(cflow_publisher, cflow_statechart_terminal_source, 0,
    .name = statechart_terminal_source_name,
    .output_type = statechart_terminal_source_type,
    .resume = statechart_terminal_resume,
    .cancel = statechart_terminal_cancel,
    .destroy = statechart_terminal_detach,
    .bind_terminal_waker = statechart_terminal_source_bind,
    .poll_terminal = statechart_terminal_source_poll
);

bool cflow_statechart_instance_as_terminal_resumable(
    cflow_statechart_instance *instance, cflow_resumable *out) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    if (impl == NULL || out == NULL || out->name != NULL ||
        out->output_type != NULL || out->ops != NULL || out->state != NULL)
        return false;
    cmeta_mutex_lock(&impl->lock);
    if (impl->adapter_attached) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    impl->adapter_attached = true;
    cmeta_mutex_unlock(&impl->lock);
    *out = (cflow_resumable){
        "statechart-terminal", impl->ir->state_type,
        &statechart_terminal_resumable_ops, impl};
    return true;
}

bool cflow_statechart_instance_as_terminal_publisher(
    cflow_statechart_instance *instance, cflow_publisher *out) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    if (impl == NULL || out == NULL || out->self != NULL ||
        out->vtable != NULL)
        return false;
    cmeta_mutex_lock(&impl->lock);
    if (impl->adapter_attached) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    impl->adapter_attached = true;
    cmeta_mutex_unlock(&impl->lock);
    *out = cflow_statechart_terminal_source_as_cflow_publisher(impl);
    return true;
}

void cflow_statechart_instance_close(cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_waker waker = {0};
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    if (!impl->exit_requested) {
        (void)win_terminal_locked(
            impl, STATECHART_TERMINAL_CLOSE,
            CFLOW_STATECHART_INSTANCE_OK, NULL, !impl->microstep_pending,
            CFLOW_STATECHART_INSTANCE_TASK_CANCELLED, &waker);
    }
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
}

void cflow_statechart_instance_request_exit(
    cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_waker waker = {0};
    bool accepted = false;
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    if (impl->terminal_outcome == STATECHART_TERMINAL_NONE &&
        !impl->exit_requested) {
        impl->exit_requested = true;
        impl->closed = true;
        impl->internal_event_head = 0u;
        impl->internal_event_count = 0u;
        impl->completion_head = 0u;
        impl->completion_count = 0u;
        impl->root_completion_draining = false;
        cancel_external_admission_locked(impl, &waker);
        if (impl->timers_initialized)
            (void)cflow_timer_event_queue_close_begin_internal(
                &impl->timers);
        accepted = true;
    }
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
    if (accepted) (void)schedule_statechart_driver(impl);
}

void cflow_statechart_instance_cancel(cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_waker waker = {0};
    if (impl == NULL) return;
    cmeta_mutex_lock(&impl->lock);
    (void)win_terminal_locked(
        impl, STATECHART_TERMINAL_CANCEL,
        CFLOW_STATECHART_INSTANCE_OK, NULL, !impl->microstep_pending,
        CFLOW_STATECHART_INSTANCE_TASK_CANCELLED, &waker);
    cmeta_mutex_unlock(&impl->lock);
    finish_terminal_side_effects(impl, waker);
}

cflow_machine_state_id cflow_statechart_instance_current_state(
    const cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_machine_state_id result = 0u;
    size_t index, leaf_count = 0u;
    if (impl == NULL) return 0u;
    cmeta_mutex_lock(&impl->lock);
    for (index = 0u;
         index < impl->configurations[impl->published].state_count; ++index) {
        const size_t state =
            impl->configurations[impl->published].states[index];
        if (!leaf_kind(impl->ir->states[state].kind)) continue;
        result = impl->ir->states[state].id;
        if (++leaf_count > 1u) {
            result = 0u;
            break;
        }
    }
    cmeta_mutex_unlock(&impl->lock);
    return result;
}

bool cflow_statechart_instance_copy_state(
    const cflow_statechart_instance *instance,
    const cmeta_type_desc **out_type,
    void *out_state,
    size_t state_capacity) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    if (out_type != NULL) *out_type = NULL;
    if (impl == NULL || out_type == NULL || out_state == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    if (state_capacity < impl->ir->state_type->size ||
        !cflow_value_storage_type_supported(impl->ir->state_type)) {
        cmeta_mutex_unlock(&impl->lock);
        return false;
    }
    memcpy(out_state, impl->extended_states[impl->published],
           impl->ir->state_type->size);
    *out_type = impl->ir->state_type;
    cmeta_mutex_unlock(&impl->lock);
    return true;
}

bool cflow_statechart_instance_get_stats(
    const cflow_statechart_instance *instance,
    cflow_statechart_instance_stats *out) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    cflow_statechart_instance_stats snapshot = {0};
    size_t index;
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->lock);
    snapshot.configuration_version = impl->configuration_version;
    snapshot.external_accepted = impl->external_accepted;
    snapshot.external_completed = impl->external_completed;
    snapshot.external_failed = impl->external_failed;
    snapshot.external_cancelled = impl->external_cancelled;
    snapshot.external_pending = impl->external_pending;
    snapshot.external_in_flight = impl->external_in_flight ? 1u : 0u;
    snapshot.macrosteps = impl->macrosteps;
    snapshot.microsteps = impl->microsteps;
    snapshot.actions = impl->actions;
    snapshot.internal_pending = impl->internal_event_count;
    snapshot.completion_pending = impl->completion_count;
    snapshot.adapter_internal_accepted = impl->adapter_internal_accepted;
    snapshot.adapter_internal_pending = impl->adapter_internal_pending;
    snapshot.active_state_count =
        impl->configurations[impl->published].state_count;
    for (index = 0u; index < snapshot.active_state_count; ++index) {
        const size_t state =
            impl->configurations[impl->published].states[index];
        if (leaf_kind(impl->ir->states[state].kind))
            ++snapshot.active_leaf_count;
    }
    snapshot.closed = impl->closed;
    snapshot.cancelled = impl->cancelled;
    snapshot.done = impl->done;
    snapshot.errored = impl->error != NULL;
    snapshot.last_status = impl->last_status;
    if (impl->timers_initialized)
        (void)cflow_timer_event_queue_get_stats(
            &impl->timers, &snapshot.timers);
    cmeta_mutex_unlock(&impl->lock);
    *out = snapshot;
    return true;
}

const char *cflow_statechart_instance_error(
    const cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl = instance != NULL
        ? (cflow_statechart_instance_impl *)instance->impl : NULL;
    const char *error;
    if (impl == NULL) return NULL;
    cmeta_mutex_lock(&impl->lock);
    error = impl->error;
    cmeta_mutex_unlock(&impl->lock);
    return error;
}

cflow_statechart_instance_status cflow_statechart_instance_destroy(
    cflow_statechart_instance *instance) {
    cflow_statechart_instance_impl *impl;
    bool adapter_attached;
    if (instance == NULL) return CFLOW_STATECHART_INSTANCE_INVALID_ARGUMENT;
    impl = (cflow_statechart_instance_impl *)instance->impl;
    if (impl == NULL) return CFLOW_STATECHART_INSTANCE_OK;
    cmeta_mutex_lock(&impl->lock);
    adapter_attached = impl->adapter_attached;
    cmeta_mutex_unlock(&impl->lock);
    if (adapter_attached) return CFLOW_STATECHART_INSTANCE_WOULD_BLOCK;
    if (wait_instance_tasks(impl) != CFLOW_STATECHART_INSTANCE_OK)
        return CFLOW_STATECHART_INSTANCE_WOULD_BLOCK;
    instance->impl = NULL;
    instance_impl_free(impl);
    return CFLOW_STATECHART_INSTANCE_OK;
}
