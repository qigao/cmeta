#ifndef CMETA_CNET_POLICY_H
#define CMETA_CNET_POLICY_H

/* Optional composition adapter. Native storage, policy execution and I/O stay
 * in Salts::CNet. Include explicitly; Core must never depend on this header. */
#include <cnet/cnet.h>
#include <cnet/client_pool.h>
#include <cnet/recovery_policy.h>
#include <cmeta/data_reflect.h>
#include <cmeta/cleanup.h>

/* Views grant inspection only, not writable configuration or construction of
 * live owners. Mutation must still go through the native owner's API. */
cmeta_reflect_data(cnet_send_limits, "cnet.SendLimits",
    cmeta_data_field(size_t, operations, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, bytes, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, backing_bytes, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, connection_operations, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, connection_bytes, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, connection_backing_bytes, &cmeta_data_size, &cmeta_type_size)
);
cmeta_reflect_data(cnet_send_usage, "cnet.SendUsage",
    cmeta_data_field(size_t, operations, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, bytes, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, backing_bytes, &cmeta_data_size, &cmeta_type_size)
);
cmeta_reflect_data(cnet_backoff_input, "cnet.BackoffInput",
    cmeta_data_field(uint32_t, attempts, &cmeta_data_uint32, &cmeta_type_uint32)
    cmeta_data_field(uint64_t, initial_ms, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, maximum_ms, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, exponential_ms, &cmeta_data_uint64, &cmeta_type_uint64)
    cmeta_data_field(uint64_t, random_value, &cmeta_data_uint64, &cmeta_type_uint64)
);
cmeta_reflect_data(cnet_pool_snapshot, "cnet.PoolSnapshot",
    cmeta_data_field(size_t, max_connections, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, max_connecting, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, max_leases, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, connecting, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, ready, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, draining, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, terminal_waiting_for_leases, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, physical_in_use, &cmeta_data_size, &cmeta_type_size)
    cmeta_data_field(size_t, active_leases, &cmeta_data_size, &cmeta_type_size)
    cmeta_field(bool, sealed)
    cmeta_field(bool, drained)
);

CMETA_INLINE void cmeta_cnet_buffer_release_(void *authority, void *resource) {
    mem_buffer_t **slot = (mem_buffer_t **)resource;
    mem_buffer_t *buffer = *slot;
    (void)authority;
    *slot = NULL;
    mem_buffer_release(buffer);
}
CMETA_INLINE void cmeta_cnet_slice_release_(void *authority, void *resource) {
    (void)authority;
    mem_slice_release((mem_slice_t *)resource);
}
/* Arm only caller-owned references. No retain, no pool destruction, no I/O.
 * Storage and cleanup record stay address-stable. Native CNet retains its own
 * references on send admission, so lexical release is safe while I/O continues.
 * In C use cmeta_with_cleanups; in C++ use cmeta::cleanup_scope. */
CMETA_INLINE cmeta_status cmeta_cnet_buffer_cleanup(cmeta_cleanup *cleanup,
                                                   mem_buffer_t **owned) {
    if (owned == NULL) return CMETA_INVALID_ARGUMENT;
    return cmeta_cleanup_arm(cleanup, cmeta_cnet_buffer_release_, NULL, owned);
}
CMETA_INLINE cmeta_status cmeta_cnet_slice_cleanup(cmeta_cleanup *cleanup,
                                                  mem_slice_t *owned) {
    if (owned == NULL) return CMETA_INVALID_ARGUMENT;
    return cmeta_cleanup_arm(cleanup, cmeta_cnet_slice_release_, NULL, owned);
}

#endif
