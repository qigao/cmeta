#ifndef CFLOW_IO_PUBLISHER_INTERNAL_H
#define CFLOW_IO_PUBLISHER_INTERNAL_H

#include <cflow/io_publisher.h>

#include <thread.h>

#include "value_storage.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct cflow_io_publisher_entry {
    cflow_value_slot result;
    cflow_io_request_id request_id;
    cflow_io_lease_id lease_id;
    uint64_t delivery_sequence;
    cflow_read_status result_status;
    const char *result_error;
    bool occupied;
    bool submission_in_progress;
    bool result_encoding;
    bool result_ready;
    bool completion_delivered;
    bool acknowledged;
    bool demand_reserved;
} cflow_io_publisher_entry;

typedef struct cflow_io_publisher_state {
    cflow_io_actor actor;
    cflow_executor executor;
    cflow_value_slot result;
    cflow_io_publisher_entry *entries;
    cmeta_mutex_t gate;
    cmeta_cond_t changed;
    cflow_waker source_waker;
    cflow_io_request_id request_id;
    size_t wake_inflight;
    size_t wake_waiters;
    size_t drive_inflight;
    size_t window_capacity;
    size_t window_occupied;
    size_t window_demand_reserved;
    size_t window_results_ready;
    size_t window_peak_occupied;
    uint64_t drive_generation;
    uint64_t next_delivery_sequence;
    bool publisher_live;
    bool owner_live;
    bool close_requested;
    atomic_bool driver_active;
    bool drive_pending;
    bool submission_in_progress;
    bool result_encoding;
    bool result_ready;
    bool completion_delivered;
    bool acknowledged;
    bool prepare_done;
    bool terminal_delivery_seen;
    cflow_read_status result_status;
    const char *result_error;
    const char *name;
    const cmeta_type_desc *type;
    cflow_io_publisher_prepare_fn prepare;
    cflow_io_publisher_encode_fn encode;
    void *user;
    cflow_io_wake_fn drive;
    void *drive_user;
    cflow_publisher_terminal terminal;
    const char *terminal_error;
} cflow_io_publisher_state;

/* NativeIO manual-batch fast path. The caller must serialize NativeIO
   observation, use a Manual Scheduler, and configure no Publisher drive
   callback, so one ACK/Actor/Executor/post-ACK phase cannot receive a
   reentrant or concurrent backend edge. */
int cflow_io_publisher_owner_run_serial_batch_phase_internal(
    cflow_io_publisher_owner *owner,
    size_t max_steps,
    size_t *progressed);

/* Test-only seam for placing the deterministic Executor barrier in the
   adapter-driver tail window while the owner remains live. */
static inline cflow_executor *cflow_io_publisher_test_executor(
    cflow_io_publisher_owner *owner) {
    cflow_io_publisher_state *state;

    if (owner == NULL || owner->impl == NULL)
        return NULL;
    state = (cflow_io_publisher_state *)owner->impl;
    return &state->executor;
}

#endif /* CFLOW_IO_PUBLISHER_INTERNAL_H */
