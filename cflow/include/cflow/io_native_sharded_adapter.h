#ifndef CFLOW_IO_NATIVE_SHARDED_ADAPTER_H
#define CFLOW_IO_NATIVE_SHARDED_ADAPTER_H

#include <cflow/io_actor.h>
#include <native_io_sharded.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Thin CFlow backend bridge over an existing NativeIO Sharded runtime.
 *
 * The adapter owns no NativeIO backend, endpoint, owner thread, observe loop,
 * Actor, Publisher, payload, or address storage. It only retains fixed bridge
 * metadata while a CFlow request is mapped to one native_io_sharded request.
 */
typedef struct cflow_io_native_sharded_adapter {
    void *impl;
} cflow_io_native_sharded_adapter;

typedef struct cflow_io_native_sharded_adapter_config {
    /** Borrowed until adapter destroy; the adapter never shuts it down. */
    native_io_sharded *runtime;
    /** Fixed bridge capacity. No bridge storage grows after init. */
    size_t bridge_capacity;
} cflow_io_native_sharded_adapter_config;

typedef struct cflow_io_native_sharded_adapter_stats {
    native_io_sharded_stats native;
    size_t bridge_capacity;
    size_t active_bridges;
    uint64_t accepted_routes;
    uint64_t raw_admissions;
    uint64_t raw_admission_failures;
    uint64_t terminal_completions;
    uint64_t stale_actor_completions;
    uint64_t cancel_routes;
    uint64_t cancel_route_rejections;
    uint64_t native_cancel_errors;
    bool closed;
} cflow_io_native_sharded_adapter_stats;

/**
 * Initializes a fixed-capacity bridge over runtime.
 *
 * runtime remains owned by the caller. The adapter may be used by exactly one
 * cflow_io_actor for its lifetime. NativeIO Sharded remains responsible for
 * endpoint affinity, owner execution, observe/progress, terminal I/O truth and
 * runtime shutdown.
 */
int cflow_io_native_sharded_adapter_init(
    cflow_io_native_sharded_adapter *adapter,
    const cflow_io_native_sharded_adapter_config *config);

/**
 * Returns cflow_io_actor backend operations for native_io_sharded_operation.
 *
 * operation_user must point to a native_io_sharded_operation whose descriptor,
 * payload and address storage remain owned by the submitted cflow_io_operation
 * token until Actor acknowledgement. This existing CFlow move-owned lifetime is
 * used as the cross-shard ownership guarantee; the adapter never copies
 * payload storage and never releases the operation token itself.
 *
 * Backend submit uses native_io_sharded_try_submit_owned(). Same-owner calls
 * therefore retain NativeIO's zero-hop path; off-owner calls use the runtime's
 * bounded queue and fail explicitly when that route is full.
 *
 * NativeIO terminal callbacks run on the endpoint owner and publish completion
 * into the CFlow IO Actor. That semantic completion/wake edge does not route a
 * task back through NativeIO Sharded. Actor delivery may subsequently use the
 * Actor's configured CFlow executor, which is a higher-layer scheduling choice
 * and must not be confused with an I/O-owner reverse hop.
 */
cflow_io_backend_ops cflow_io_native_sharded_adapter_actor_ops(void);

/** Stops new backend submissions. Accepted operations continue to terminal. */
int cflow_io_native_sharded_adapter_close(
    cflow_io_native_sharded_adapter *adapter);

/** Copies adapter counters plus a concurrent NativeIO Sharded stats snapshot. */
bool cflow_io_native_sharded_adapter_get_stats(
    const cflow_io_native_sharded_adapter *adapter,
    cflow_io_native_sharded_adapter_stats *out_stats);

/**
 * Releases bridge storage after close and complete terminal/cancel-task drain.
 * The borrowed NativeIO Sharded runtime is never destroyed by this function.
 */
int cflow_io_native_sharded_adapter_destroy(
    cflow_io_native_sharded_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif /* CFLOW_IO_NATIVE_SHARDED_ADAPTER_H */
