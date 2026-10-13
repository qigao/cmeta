/* Caller never shares or persists borrowed CMeta pointers across image
 * lifetime; it snapshots their semantic shape while both DSOs remain loaded.
 * The first and second arguments can be reversed to check load-order bias.
 */
#define _POSIX_C_SOURCE 200809L
#include "snapshot.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
typedef int (*probe_fn)(salts_cflow_probe_snapshot *);

static int query(void *module, salts_cflow_probe_snapshot *out) {
    probe_fn fn = NULL;
    const char *error;
    dlerror();
    *(void **)(&fn) = dlsym(module, "salts_cflow_probe_read");
    error = dlerror();
    if (error != NULL || fn == NULL) {
        fprintf(stderr, "CFLOW_EXPORT_MISSING: %s\n",
                error ? error : "null");
        return 0;
    }
    return fn(out);
}

int main(int argc, char **argv) {
    void *first, *second;
    salts_cflow_probe_snapshot a = {0}, b = {0};
    int shared;
    if (argc != 4 || (strcmp(argv[3], "shared") != 0 &&
                      strcmp(argv[3], "isolated") != 0)) {
        fputs("usage: loader <client.so> <server.so> shared|isolated\n", stderr);
        return 2;
    }
    first = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!first) {
        fprintf(stderr, "CFLOW_DSO_FIRST_LOAD_FAILED: %s\n", dlerror());
        return 3;
    }
    second = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!second) {
        fprintf(stderr, "CFLOW_DSO_SECOND_LOAD_FAILED: %s\n", dlerror());
        (void)dlclose(first);
        return 4;
    }
    if (!query(first, &a) || !query(second, &b)) {
        fputs("CFLOW_DESCRIPTOR_QUERY_FAILED\n", stderr);
        return 5;
    }
    if (a.descriptor_address == 0 || b.descriptor_address == 0 ||
        a.size != sizeof(void *) || a.size != b.size ||
        a.align != b.align || a.kind != b.kind ||
        a.pointee_size == 0 || a.pointee_size != b.pointee_size ||
        a.pointee_align == 0 || a.pointee_align != b.pointee_align ||
        a.pointee_kind != b.pointee_kind ||
        strcmp(a.name, b.name) != 0 ||
        strcmp(a.pointee_name, b.pointee_name) != 0 ||
        strcmp(a.name, "cflow_executor_protocol_stats *") != 0 ||
        strcmp(a.pointee_name, "cflow_executor_protocol_stats") != 0) {
        fputs("CFLOW_CROSS_IMAGE_SEMANTIC_SHAPE_MISMATCH\n", stderr);
        return 6;
    }
    shared = a.descriptor_address == b.descriptor_address;
    if (shared != (strcmp(argv[3], "shared") == 0)) {
        fprintf(stderr, "CFLOW_OWNER_IDENTITY_MISMATCH expected=%s actual=%s\n",
                argv[3], shared ? "shared" : "isolated");
        return 7;
    }
    printf("CFLOW_DESCRIPTOR_SHAPE_MATCH=1\n");
    printf("CFLOW_DESCRIPTOR_OWNER=%s\n", shared ? "single-shared" : "image-local");
    fflush(stdout);
    if (dlclose(second) != 0 || dlclose(first) != 0) {
        fprintf(stderr, "CFLOW_DSO_CLOSE_FAILED: %s\n", dlerror());
        return 8;
    }
    /* a/b are COPIED VALUE snapshots only. Do not access the borrowed
     * desc/pointee addresses after dlclose, even if they were once equal. */
    puts("CFLOW_DSO_LOAD_QUERY_UNLOAD_OK");
    return 0;
}
