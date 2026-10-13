#ifndef SALTS_CFLOW_DSO_SNAPSHOT_H
#define SALTS_CFLOW_DSO_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>
/* Value-only snapshot: never retain or dereference a foreign DSO's borrowed
 * cmeta_type_desc after that image has been unloaded. No descriptor pointer
 * is persisted or exchanged across owner generations. */
typedef struct salts_cflow_probe_snapshot {
    uintptr_t descriptor_address;
    uintptr_t pointee_address;
    size_t size;
    size_t align;
    size_t pointee_size;
    size_t pointee_align;
    int kind;
    int pointee_kind;
    char name[96];
    char pointee_name[96];
} salts_cflow_probe_snapshot;
#endif
