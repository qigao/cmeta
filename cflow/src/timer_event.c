#include <cflow/timer_event.h>

#include "machine_instance_internal.h"
#include "timer_event_internal.h"
#include "timer_queue.h"

#include <thread.h>

#include <stdlib.h>
#include <string.h>

typedef enum timer_event_slot_state {
    TIMER_EVENT_SLOT_FREE = 0,
    TIMER_EVENT_SLOT_PENDING,
    TIMER_EVENT_SLOT_FIRING
} timer_event_slot_state;

typedef struct timer_event_slot {
    cflow_timer_event_id timer_id;
    cflow_event_id event_id;
    cflow_machine_state_id scope;
    const cmeta_type_desc *payload_type;
    timer_event_slot_state state;
} timer_event_slot;

typedef struct cflow_timer_event_queue_impl {
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    cflow_clock *clock;
    cflow_timer_event_target_internal target;
    cflow_timer_queue timers;
    timer_event_slot *slots;
    unsigned char *payloads;
    size_t capacity;
    size_t payload_stride;
    size_t reserved_payload_bytes;
    size_t reserved_bytes;
    size_t in_flight;
    size_t peak_pending;
    uint64_t scheduled;
    uint64_t delivered;
    uint64_t cancelled;
    uint64_t cancelled_on_close;
    uint64_t mailbox_rejected;
    uint64_t mailbox_rejected_full;
    uint64_t mailbox_rejected_closed;
    uint64_t mailbox_rejected_cancelled;
    uint64_t mailbox_rejected_other;
    uint64_t rejected_full;
    uint64_t rejected_closed;
    bool closed;
    bool consumer_active;
} cflow_timer_event_queue_impl;

static void timer_event_marker(void *user) { (void)user; }

static bool checked_mul(size_t left, size_t right, size_t *out) {
    if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return false;
    *out = left * right;
    return true;
}

static bool checked_add(size_t left, size_t right, size_t *out) {
    if (out == NULL || right > SIZE_MAX - left) return false;
    *out = left + right;
    return true;
}

static bool aligned_stride(size_t size, size_t *out) {
    const size_t alignment = CMETA_ALIGNOF(cmeta_capture_storage);
    size_t expanded;
    if (size == 0u || alignment == 0u ||
        !checked_add(size, alignment - 1u, &expanded))
        return false;
    *out = expanded & ~(alignment - 1u);
    return *out >= size;
}

bool cflow_timer_event_queue_storage_requirements_internal(
    size_t payload_capacity,
    size_t capacity,
    size_t *out_bytes) {
    size_t stride, slot_bytes, timer_bytes, payload_bytes, total;
    if (out_bytes == NULL || capacity == 0u ||
        !aligned_stride(payload_capacity, &stride) ||
        !checked_mul(capacity, sizeof(timer_event_slot), &slot_bytes) ||
        !checked_mul(capacity, sizeof(cflow_timer_task), &timer_bytes) ||
        !checked_mul(capacity, stride, &payload_bytes) ||
        !checked_add(sizeof(cflow_timer_event_queue_impl), slot_bytes,
                     &total) ||
        !checked_add(total, timer_bytes, &total) ||
        !checked_add(total, payload_bytes, &total))
        return false;
    *out_bytes = total;
    return true;
}

static unsigned char *slot_payload(cflow_timer_event_queue_impl *impl,
                                   const timer_event_slot *slot) {
    const size_t index = (size_t)(slot - impl->slots);
    return impl->payloads + index * impl->payload_stride;
}

static timer_event_slot *find_free_slot(cflow_timer_event_queue_impl *impl) {
    size_t index;
    for (index = 0u; index < impl->capacity; ++index) {
        if (impl->slots[index].state == TIMER_EVENT_SLOT_FREE)
            return &impl->slots[index];
    }
    return NULL;
}

static timer_event_slot *find_timer_slot(cflow_timer_event_queue_impl *impl,
                                         cflow_timer_event_id timer_id) {
    size_t index;
    for (index = 0u; index < impl->capacity; ++index) {
        if (impl->slots[index].state != TIMER_EVENT_SLOT_FREE &&
            impl->slots[index].timer_id == timer_id)
            return &impl->slots[index];
    }
    return NULL;
}

static void release_slot(timer_event_slot *slot) {
    if (slot == NULL) return;
    *slot = (timer_event_slot){0};
}

static cflow_timer_event_schedule_result schedule_at(
    cflow_timer_event_queue_impl *impl,
    cflow_deadline deadline,
    const cflow_event_view *event,
    cflow_machine_state_id scope) {
    cflow_timer_event_schedule_result result = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u
    };
    const cmeta_type_desc *canonical_type = NULL;
    cflow_mailbox_status contract_status;
    timer_event_slot *slot;
    cflow_schedule_result timer_result;

    if (impl == NULL || event == NULL) return result;
    contract_status = impl->target.contract(
        impl->target.user, event, &canonical_type);
    if (contract_status != CFLOW_MAILBOX_OK) {
        result.status = contract_status == CFLOW_MAILBOX_TYPE_MISMATCH
            ? CFLOW_TIMER_EVENT_TYPE_MISMATCH
            : CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
        return result;
    }
    if (!cmeta_type_desc_valid(canonical_type) ||
        canonical_type->size > impl->payload_stride) {
        result.status = CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
        return result;
    }

    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        ++impl->rejected_closed;
        result.status = CFLOW_TIMER_EVENT_CLOSED;
        cmeta_mutex_unlock(&impl->mutex);
        return result;
    }
    slot = find_free_slot(impl);
    if (slot == NULL) {
        ++impl->rejected_full;
        result.status = CFLOW_TIMER_EVENT_FULL;
        cmeta_mutex_unlock(&impl->mutex);
        return result;
    }

    slot->event_id = event->id;
    slot->scope = scope;
    slot->payload_type = canonical_type;
    slot->state = TIMER_EVENT_SLOT_PENDING;
    memcpy(slot_payload(impl, slot), event->payload, canonical_type->size);
    timer_result = cflow_timer_queue_try_schedule(
        &impl->timers, deadline, timer_event_marker, slot);
    if (timer_result.status != CFLOW_ADMISSION_ACCEPTED) {
        release_slot(slot);
        ++impl->rejected_full;
        result.status = CFLOW_TIMER_EVENT_FULL;
        cmeta_mutex_unlock(&impl->mutex);
        return result;
    }

    slot->timer_id = timer_result.task_id;
    ++impl->scheduled;
    if (cflow_timer_queue_pending(&impl->timers) > impl->peak_pending)
        impl->peak_pending = cflow_timer_queue_pending(&impl->timers);
    result.status = CFLOW_TIMER_EVENT_OK;
    result.timer_id = slot->timer_id;
    cmeta_mutex_unlock(&impl->mutex);
    return result;
}

cflow_timer_event_status cflow_timer_event_queue_init_target_internal(
    cflow_timer_event_queue *queue,
    cflow_clock *clock,
    size_t capacity,
    cflow_timer_event_target_internal target,
    size_t payload_capacity) {
    cflow_timer_event_queue_impl *impl;
    size_t total_storage_bytes;

    if (queue == NULL || queue->impl != NULL || clock == NULL ||
        !cflow_clock_valid(clock) || capacity == 0u ||
        target.contract == NULL || target.send == NULL ||
        payload_capacity == 0u)
        return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
    if (!aligned_stride(payload_capacity, &payload_capacity) ||
        !cflow_timer_event_queue_storage_requirements_internal(
            payload_capacity, capacity, &total_storage_bytes))
        return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;

    impl = (cflow_timer_event_queue_impl *)calloc(1u, sizeof(*impl));
    if (impl == NULL) return CFLOW_TIMER_EVENT_ALLOCATION_FAILED;
    impl->capacity = capacity;
    impl->payload_stride = payload_capacity;
    if (!checked_mul(capacity, impl->payload_stride,
                     &impl->reserved_payload_bytes)) {
        free(impl);
        return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
    }
    impl->reserved_bytes = total_storage_bytes - sizeof(*impl);

    impl->slots = (timer_event_slot *)calloc(capacity,
                                               sizeof(*impl->slots));
    impl->payloads = (unsigned char *)calloc(
        1u, impl->reserved_payload_bytes);
    cmeta_mutex_init(&impl->mutex);
    cmeta_cond_init(&impl->changed);
    if (impl->slots == NULL || impl->payloads == NULL || impl->mutex == NULL ||
        impl->changed == NULL ||
        !cflow_timer_queue_init_with_capacity(&impl->timers,
                                              capacity)) {
        cflow_timer_queue_destroy(&impl->timers);
        cmeta_cond_destroy(&impl->changed);
        cmeta_mutex_destroy(&impl->mutex);
        free(impl->payloads);
        free(impl->slots);
        free(impl);
        return CFLOW_TIMER_EVENT_ALLOCATION_FAILED;
    }

    impl->clock = clock;
    impl->target = target;
    queue->impl = impl;
    return CFLOW_TIMER_EVENT_OK;
}

static cflow_mailbox_status machine_timer_contract(
    void *user, const cflow_event_view *event,
    const cmeta_type_desc **out_canonical_type) {
    return cflow_machine_instance_timer_event_contract(
        (cflow_machine_instance *)user, event, out_canonical_type);
}

static cflow_mailbox_status machine_timer_send(
    void *user, const cflow_event_view *event) {
    return cflow_machine_instance_try_send(
        (cflow_machine_instance *)user, event);
}

cflow_timer_event_status cflow_timer_event_queue_init(
    cflow_timer_event_queue *queue,
    const cflow_timer_event_queue_config *config) {
    size_t payload_capacity;
    cflow_timer_event_target_internal target;
    if (config == NULL || config->machine == NULL ||
        !cflow_machine_instance_timer_payload_capacity(
            config->machine, &payload_capacity))
        return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
    target = (cflow_timer_event_target_internal){
        config->machine, machine_timer_contract, machine_timer_send};
    return cflow_timer_event_queue_init_target_internal(
        queue, config->clock, config->capacity, target, payload_capacity);
}

cflow_timer_event_schedule_result cflow_timer_event_queue_try_schedule_at(
    cflow_timer_event_queue *queue,
    cflow_deadline deadline,
    const cflow_event_view *event) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    return schedule_at(impl, deadline, event, 0u);
}

cflow_timer_event_schedule_result cflow_timer_event_queue_try_schedule_after(
    cflow_timer_event_queue *queue,
    cflow_duration delay,
    const cflow_event_view *event) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    cflow_timer_event_schedule_result invalid = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u
    };
    cflow_instant now;
    if (impl == NULL) return invalid;
    now = cflow_clock_now(impl->clock);
    return schedule_at(impl, cflow_deadline_after(now, delay), event, 0u);
}

cflow_timer_event_schedule_result cflow_timer_event_queue_try_schedule_scoped_at(
    cflow_timer_event_queue *queue,
    cflow_deadline deadline,
    const cflow_event_view *event,
    cflow_machine_state_id scope) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    cflow_timer_event_schedule_result invalid = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u
    };
    if (impl == NULL || scope == 0u) return invalid;
    return schedule_at(impl, deadline, event, scope);
}

cflow_timer_event_schedule_result cflow_timer_event_queue_try_schedule_scoped_after(
    cflow_timer_event_queue *queue,
    cflow_duration delay,
    const cflow_event_view *event,
    cflow_machine_state_id scope) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    cflow_timer_event_schedule_result invalid = {
        CFLOW_TIMER_EVENT_INVALID_ARGUMENT, 0u
    };
    cflow_instant now;
    if (impl == NULL || scope == 0u) return invalid;
    now = cflow_clock_now(impl->clock);
    return schedule_at(impl, cflow_deadline_after(now, delay), event, scope);
}

static bool scope_list_contains(const cflow_machine_state_id *scopes,
                                size_t scope_count,
                                cflow_machine_state_id scope) {
    size_t index;
    for (index = 0u; index < scope_count; ++index) {
        if (scopes[index] == scope) return true;
    }
    return false;
}

size_t cflow_timer_event_queue_cancel_scopes(
    cflow_timer_event_queue *queue,
    const cflow_machine_state_id *scopes,
    size_t scope_count) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    size_t cancelled = 0u;
    size_t index;
    if (impl == NULL || (scope_count != 0u && scopes == NULL)) return 0u;
    if (scope_count == 0u) return 0u;
    cmeta_mutex_lock(&impl->mutex);
    if (!impl->closed) {
        for (index = 0u; index < impl->capacity; ++index) {
            timer_event_slot *slot = &impl->slots[index];
            if (slot->state != TIMER_EVENT_SLOT_PENDING ||
                !scope_list_contains(scopes, scope_count, slot->scope))
                continue;
            if (cflow_timer_queue_cancel(&impl->timers, slot->timer_id)) {
                release_slot(slot);
                ++impl->cancelled;
                ++cancelled;
            }
        }
    }
    cmeta_mutex_unlock(&impl->mutex);
    return cancelled;
}

cflow_timer_event_status cflow_timer_event_queue_cancel(
    cflow_timer_event_queue *queue,
    cflow_timer_event_id timer_id) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    timer_event_slot *slot;
    if (impl == NULL || timer_id == 0u)
        return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        cmeta_mutex_unlock(&impl->mutex);
        return CFLOW_TIMER_EVENT_CLOSED;
    }
    slot = find_timer_slot(impl, timer_id);
    if (slot == NULL) {
        cmeta_mutex_unlock(&impl->mutex);
        return CFLOW_TIMER_EVENT_NOT_FOUND;
    }
    if (slot->state == TIMER_EVENT_SLOT_FIRING) {
        cmeta_mutex_unlock(&impl->mutex);
        return CFLOW_TIMER_EVENT_FIRE_WON;
    }
    if (!cflow_timer_queue_cancel(&impl->timers, timer_id)) {
        cmeta_mutex_unlock(&impl->mutex);
        return CFLOW_TIMER_EVENT_NOT_FOUND;
    }
    release_slot(slot);
    ++impl->cancelled;
    cmeta_mutex_unlock(&impl->mutex);
    return CFLOW_TIMER_EVENT_OK;
}

bool cflow_timer_event_queue_claim_one_ready(
    cflow_timer_event_queue *queue,
    cflow_timer_event_claim *claim,
    cflow_timer_event_fire_result *out_result) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    cflow_timer_event_fire_result result = {
        CFLOW_TIMER_EVENT_FIRE_INVALID_ARGUMENT, 0u,
        CFLOW_MAILBOX_INVALID_ARGUMENT
    };
    cflow_instant now;
    cflow_timer_task task;
    timer_event_slot *slot;

    if (out_result != NULL) *out_result = result;
    if (impl == NULL || claim == NULL || out_result == NULL ||
        claim->queue_impl != NULL || claim->slot != NULL)
        return false;
    now = cflow_clock_now(impl->clock);
    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        result.status = CFLOW_TIMER_EVENT_FIRE_CLOSED;
        cmeta_mutex_unlock(&impl->mutex);
        *out_result = result;
        return false;
    }
    if (impl->consumer_active) {
        result.status = CFLOW_TIMER_EVENT_FIRE_BUSY;
        cmeta_mutex_unlock(&impl->mutex);
        *out_result = result;
        return false;
    }
    if (!cflow_timer_queue_take_ready(&impl->timers, now, &task)) {
        result.status = CFLOW_TIMER_EVENT_FIRE_NOT_READY;
        cmeta_mutex_unlock(&impl->mutex);
        *out_result = result;
        return false;
    }

    slot = (timer_event_slot *)task.user;
    if (slot == NULL || slot->state != TIMER_EVENT_SLOT_PENDING ||
        slot->timer_id != task.id) {
        cmeta_mutex_unlock(&impl->mutex);
        *out_result = result;
        return false;
    }
    slot->state = TIMER_EVENT_SLOT_FIRING;
    impl->consumer_active = true;
    impl->in_flight = 1u;
    result.timer_id = slot->timer_id;
    claim->queue_impl = impl;
    claim->slot = slot;
    cmeta_mutex_unlock(&impl->mutex);
    *out_result = result;
    return true;
}

cflow_timer_event_fire_result cflow_timer_event_queue_commit_claim(
    cflow_timer_event_claim *claim) {
    cflow_timer_event_fire_result result = {
        CFLOW_TIMER_EVENT_FIRE_INVALID_ARGUMENT, 0u,
        CFLOW_MAILBOX_INVALID_ARGUMENT
    };
    cflow_timer_event_queue_impl *impl;
    timer_event_slot *slot;
    cflow_event_view event;

    if (claim == NULL || claim->queue_impl == NULL || claim->slot == NULL)
        return result;
    impl = (cflow_timer_event_queue_impl *)claim->queue_impl;
    slot = (timer_event_slot *)claim->slot;
    claim->queue_impl = NULL;
    claim->slot = NULL;
    result.timer_id = slot->timer_id;
    event = (cflow_event_view){
        slot->event_id, slot->payload_type, slot_payload(impl, slot)
    };

    result.mailbox_status = impl->target.send(
        impl->target.user, &event);

    cmeta_mutex_lock(&impl->mutex);
    if (result.mailbox_status == CFLOW_MAILBOX_OK) {
        result.status = CFLOW_TIMER_EVENT_FIRE_DELIVERED;
        ++impl->delivered;
    } else {
        result.status = CFLOW_TIMER_EVENT_FIRE_MAILBOX_REJECTED;
        ++impl->mailbox_rejected;
        if (result.mailbox_status == CFLOW_MAILBOX_FULL)
            ++impl->mailbox_rejected_full;
        else if (result.mailbox_status == CFLOW_MAILBOX_CLOSED)
            ++impl->mailbox_rejected_closed;
        else if (result.mailbox_status == CFLOW_MAILBOX_CANCELLED)
            ++impl->mailbox_rejected_cancelled;
        else
            ++impl->mailbox_rejected_other;
    }
    release_slot(slot);
    impl->in_flight = 0u;
    impl->consumer_active = false;
    cmeta_cond_broadcast(&impl->changed);
    cmeta_mutex_unlock(&impl->mutex);
    return result;
}

cflow_timer_event_fire_result cflow_timer_event_queue_run_one_ready(
    cflow_timer_event_queue *queue) {
    cflow_timer_event_claim claim = {0};
    cflow_timer_event_fire_result result;
    if (!cflow_timer_event_queue_claim_one_ready(queue, &claim, &result))
        return result;
    return cflow_timer_event_queue_commit_claim(&claim);
}

bool cflow_timer_event_queue_get_stats(
    const cflow_timer_event_queue *queue,
    cflow_timer_event_stats *out) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    if (impl == NULL || out == NULL) return false;
    cmeta_mutex_lock(&impl->mutex);
    *out = (cflow_timer_event_stats){
        .capacity = impl->capacity,
        .pending = cflow_timer_queue_pending(&impl->timers),
        .in_flight = impl->in_flight,
        .peak_pending = impl->peak_pending,
        .payload_stride = impl->payload_stride,
        .reserved_payload_bytes = impl->reserved_payload_bytes,
        .reserved_bytes = impl->reserved_bytes,
        .scheduled = impl->scheduled,
        .delivered = impl->delivered,
        .cancelled = impl->cancelled,
        .cancelled_on_close = impl->cancelled_on_close,
        .mailbox_rejected = impl->mailbox_rejected,
        .mailbox_rejected_full = impl->mailbox_rejected_full,
        .mailbox_rejected_closed = impl->mailbox_rejected_closed,
        .mailbox_rejected_cancelled = impl->mailbox_rejected_cancelled,
        .mailbox_rejected_other = impl->mailbox_rejected_other,
        .rejected_full = impl->rejected_full,
        .rejected_closed = impl->rejected_closed,
        .closed = impl->closed
    };
    cmeta_mutex_unlock(&impl->mutex);
    return true;
}

cflow_timer_event_status cflow_timer_event_queue_close_begin_internal(
    cflow_timer_event_queue *queue) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    cflow_timer_event_status status = CFLOW_TIMER_EVENT_OK;
    size_t index;
    if (impl == NULL) return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;

    cmeta_mutex_lock(&impl->mutex);
    if (impl->closed) {
        status = CFLOW_TIMER_EVENT_CLOSED;
    } else {
        impl->closed = true;
        for (index = 0u; index < impl->capacity; ++index) {
            timer_event_slot *slot = &impl->slots[index];
            if (slot->state == TIMER_EVENT_SLOT_PENDING) {
                release_slot(slot);
                ++impl->cancelled;
                ++impl->cancelled_on_close;
            }
        }
        impl->timers.count = 0u;
    }
    cmeta_mutex_unlock(&impl->mutex);
    return status;
}

cflow_timer_event_status cflow_timer_event_queue_close(
    cflow_timer_event_queue *queue) {
    cflow_timer_event_queue_impl *impl = queue != NULL
        ? (cflow_timer_event_queue_impl *)queue->impl : NULL;
    cflow_timer_event_status status;
    if (impl == NULL) return CFLOW_TIMER_EVENT_INVALID_ARGUMENT;
    status = cflow_timer_event_queue_close_begin_internal(queue);
    cmeta_mutex_lock(&impl->mutex);
    while (impl->in_flight != 0u)
        cmeta_cond_wait(&impl->changed, &impl->mutex);
    cmeta_mutex_unlock(&impl->mutex);
    return status;
}

void cflow_timer_event_queue_destroy(cflow_timer_event_queue *queue) {
    cflow_timer_event_queue_impl *impl;
    if (queue == NULL || queue->impl == NULL) return;
    impl = (cflow_timer_event_queue_impl *)queue->impl;
    (void)cflow_timer_event_queue_close(queue);
    cflow_timer_queue_destroy(&impl->timers);
    cmeta_cond_destroy(&impl->changed);
    cmeta_mutex_destroy(&impl->mutex);
    free(impl->payloads);
    free(impl->slots);
    free(impl);
    queue->impl = NULL;
}
