#ifndef CFLOW_READINESS_INTERNAL_H
#define CFLOW_READINESS_INTERNAL_H

#include <cflow/readiness.h>

/* Test-only observer; the owner and Source must remain live for the call. */
readiness_registration *
cflow_readiness_publisher_owner_observe_registration(
    cflow_readiness_publisher_owner *owner);

#endif /* CFLOW_READINESS_INTERNAL_H */
