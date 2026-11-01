#ifndef CFLOW_EXECUTOR_INTERNAL_H
#define CFLOW_EXECUTOR_INTERNAL_H

#include <cflow/executor.h>
#include <thread_pool.h>

int cmeta_threadpool_is_current_internal(const cmeta_threadpool_t *pool);

/* True only while this thread is executing a callback owned by the same
 * repository-provided Executor. Foreign implementations return false. */
bool cflow_executor_is_current_internal(const cflow_executor *executor);

/* True only for this repository's owner-affine provider, on its recorded
 * owner thread. Does not imply a callback is currently executing. */
bool cflow_executor_owner_is_thread_internal(const cflow_executor *executor);

#endif /* CFLOW_EXECUTOR_INTERNAL_H */
