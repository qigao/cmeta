#include "snapshot.h"
#include <cflow/executor.h>
#include <cmeta/cmeta.h>
#include <string.h>
#if defined(__GNUC__)
#  define CFLOW_PROBE_PUBLIC __attribute__((visibility("default")))
#else
#  define CFLOW_PROBE_PUBLIC
#endif

/* Each of two INDEPENDENT real native DSOs links the exact installed
 * Salts::CFlow SDK target. This is an actual cflow/src/executor.c descriptor,
 * not a stand-in or a mock reflection provider.
 */
CFLOW_PROBE_PUBLIC int
salts_cflow_probe_read(salts_cflow_probe_snapshot *out) {
    const cmeta_type_desc *desc = &cflow_type_executor_protocol_stats_ptr;
    const cmeta_type_desc *value;
    if (out == NULL || desc->kind != CMETA_T_POINTER ||
        desc->pointee == NULL || desc->name == NULL ||
        desc->pointee->name == NULL ||
        desc->size != sizeof(void *) ||
        desc->pointee->size != sizeof(cflow_executor_protocol_stats))
        return 0;
    value = desc->pointee;
    memset(out, 0, sizeof(*out));
    out->descriptor_address = (uintptr_t)desc;
    out->pointee_address = (uintptr_t)value;
    out->size = desc->size;
    out->align = desc->align;
    out->pointee_size = value->size;
    out->pointee_align = value->align;
    out->kind = desc->kind;
    out->pointee_kind = value->kind;
    if (strlen(desc->name) >= sizeof(out->name) ||
        strlen(value->name) >= sizeof(out->pointee_name))
        return 0;
    strcpy(out->name, desc->name);
    strcpy(out->pointee_name, value->name);
    return 1;
}

/* Compare real PUBLIC CMeta types while BOTH borrowed-owner DSOs are live.
 * This does not retain the foreign descriptor or extend its lifetime. */
CFLOW_PROBE_PUBLIC const void *salts_cflow_probe_borrowed(void) {
    return &cflow_type_executor_protocol_stats_ptr;
}
CFLOW_PROBE_PUBLIC int salts_cflow_probe_semantic_equal(const void *foreign) {
    return foreign != NULL &&
           cmeta_type_equal(&cflow_type_executor_protocol_stats_ptr,
                            (const cmeta_type_desc *)foreign) ? 1 : 0;
}
