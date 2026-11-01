#ifndef CFLOW_READINESS_H
#define CFLOW_READINESS_H

#include <cflow/publishers.h>
#include <readiness.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Move-only external cleanup owner for a readiness-backed Publisher. Initialize to
 * zero and do not copy impl. The caller must keep the owner alive until the
 * Publisher has been destroyed, then call cflow_readiness_publisher_owner_close().
 */
typedef struct cflow_readiness_publisher_owner {
    void *impl;
} cflow_readiness_publisher_owner;

/*
 * Adapt an already registered Platform resource to the CFlow WAIT protocol.
 * read remains the only producer of values and terminal data semantics.
 *
 * out and owner are zero-state outputs. Success moves registration into shared
 * Publisher/owner state and clears the caller registration. Any failure clears
 * out and owner while leaving registration and user ownership with the caller.
 * Only trivial-copy, trivial-destroy value types are admitted.
 *
 * Publisher cancellation is terminal: it quiescently closes the registration,
 * then invokes close(user) once. Publisher destroy releases its state reference
 * even if Platform close fails; owner keeps registration/user reachable.
 *
 * owner is not thread-safe and must not be moved or closed concurrently.
 * owner_close returns SALTS_EBUSY without side effects while the Publisher still
 * exists. After Publisher destroy, it returns the exact Platform close error and
 * retains owner for retry. SALTS_OK clears owner and releases its final ref;
 * callers must close owner even when Publisher cleanup already succeeded.
 */
int cflow_publisher_from_readiness_registration(
    cflow_publisher *out,
    cflow_readiness_publisher_owner *owner,
    cmeta_readiness_registration *registration,
    cmeta_readiness_events events,
    const char *name,
    const cmeta_type_desc *type,
    cflow_read_fn read,
    cflow_resource_close_fn close,
    void *user);

int cflow_readiness_publisher_owner_close(cflow_readiness_publisher_owner *owner);

#ifdef __cplusplus
}
#endif

#endif /* CFLOW_READINESS_H */
