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
typedef const void *(*borrowed_fn)(void);
typedef int (*equal_fn)(const void *);
static void *required_symbol(void *module, const char *name) {
    const char *error;
    void *symbol;
    dlerror();
    symbol = dlsym(module, name);
    error = dlerror();
    if (error != NULL || symbol == NULL)
        fprintf(stderr, "CFLOW_SYMBOL_MISSING=%s: %s\n",
                name, error ? error : "null");
    return symbol;
}

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
    salts_cflow_probe_snapshot a = {0}, b = {0}, survivor = {0};
    borrowed_fn borrow_first = NULL, borrow_second = NULL;
    equal_fn equal_first = NULL, equal_second = NULL;
    const void *first_borrowed = NULL, *second_borrowed = NULL;
    int shared;
    if (argc != 4 || (strcmp(argv[3], "shared") != 0 &&
                      strcmp(argv[3], "isolated") != 0)) {
        fputs("usage: loader <client.so> <server.so> shared|isolated\n", stderr);
        return 2;
    }
    /* Match the CHttp process's globally visible first native DSO. The
     * second separately loaded image remains RTLD_LOCAL. */
    first = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
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
    *(void **)(&borrow_first) =
        required_symbol(first, "salts_cflow_probe_borrowed");
    *(void **)(&borrow_second) =
        required_symbol(second, "salts_cflow_probe_borrowed");
    *(void **)(&equal_first) =
        required_symbol(first, "salts_cflow_probe_semantic_equal");
    *(void **)(&equal_second) =
        required_symbol(second, "salts_cflow_probe_semantic_equal");
    if (!borrow_first || !borrow_second || !equal_first || !equal_second)
        return 8;
    first_borrowed = borrow_first();
    second_borrowed = borrow_second();
    if (!first_borrowed || !second_borrowed ||
        (uintptr_t)first_borrowed != a.descriptor_address ||
        (uintptr_t)second_borrowed != b.descriptor_address ||
        equal_first(second_borrowed) != 1 ||
        equal_second(first_borrowed) != 1) {
        fputs("CFLOW_CROSS_DSO_CMETA_TYPE_EQUAL_FAILED\n", stderr);
        return 9;
    }
    puts("CFLOW_SEMANTIC_EQUAL_BOTH_DIRECTIONS=1");
    printf("CFLOW_DESCRIPTOR_SHAPE_MATCH=1\n");
    printf("CFLOW_DESCRIPTOR_OWNER=%s\n", shared ? "single-shared" : "image-local");
    fflush(stdout);
    /* Close SECOND first. Never touch second_borrowed after this call. */
    if (dlclose(second) != 0) {
        fputs("CFLOW_SECOND_DSO_CLOSE_FAILED\n", stderr);
        return 10;
    }
    if (!query(first, &survivor) ||
        survivor.descriptor_address != a.descriptor_address ||
        survivor.pointee_address != a.pointee_address ||
        survivor.size != a.size ||
        strcmp(survivor.name, a.name) != 0 ||
        equal_first(first_borrowed) != 1) {
        fputs("CFLOW_LIVE_OWNER_BROKEN_AFTER_OTHER_UNLOAD\n", stderr);
        return 11;
    }
    puts("CFLOW_LIVE_OWNER_SURVIVED_OTHER_UNLOAD");
    if (dlclose(first) != 0) {
        fputs("CFLOW_FIRST_DSO_CLOSE_FAILED\n", stderr);
        return 12;
    }
    /* Snapshots are copied values, never borrowed post-unload pointers. */
    puts("CFLOW_DSO_LOAD_QUERY_UNLOAD_OK");
    return 0;

}
