/* Real installed-Salts CFlow descriptor owner test for macOS and Windows.
 * The two independent consumer DSOs retain their own borrowed descriptors.
 * Both modules stay loaded during public cmeta_type_equal; after closing
 * the second, only the first module's borrowed descriptor is accessed.
 */
#include "../consumer/snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   typedef HMODULE module_handle;
   static module_handle module_open(const char *path, int global) {
       module_handle h;
       (void)global;
       h = LoadLibraryA(path);
       if (!h)
           fprintf(stderr, "CFLOW_CROSS_DSO_LOAD_FAILED=%lu\n",
                   (unsigned long)GetLastError());
       return h;
   }
   static void *module_symbol(module_handle h, const char *name) {
       FARPROC proc = GetProcAddress(h, name);
       if (!proc)
           fprintf(stderr, "CFLOW_CROSS_SYMBOL_MISSING=%s:%lu\n",
                   name, (unsigned long)GetLastError());
       return (void *)(uintptr_t)proc;
   }
   static int module_close(module_handle h) { return FreeLibrary(h) != 0; }
#else
#  include <dlfcn.h>
   typedef void *module_handle;
   static module_handle module_open(const char *path, int global) {
       module_handle h = dlopen(path, RTLD_NOW |
                             (global ? RTLD_GLOBAL : RTLD_LOCAL));
       if (!h) fprintf(stderr, "CFLOW_CROSS_DSO_LOAD_FAILED=%s\n", dlerror());
       return h;
   }
   static void *module_symbol(module_handle h, const char *name) {
       void *p;
       const char *error;
       dlerror();
       p = dlsym(h, name);
       error = dlerror();
       if (!p || error)
           fprintf(stderr, "CFLOW_CROSS_SYMBOL_MISSING=%s:%s\n",
                   name, error ? error : "null");
       return p;
   }
   static int module_close(module_handle h) { return dlclose(h) == 0; }
#endif

typedef int (*read_fn)(salts_cflow_probe_snapshot *);
typedef const void *(*borrow_fn)(void);
typedef int (*equal_fn)(const void *);

static int load_api(module_handle h, read_fn *read,
                    borrow_fn *borrow, equal_fn *equal) {
    void *p;
    if (!h) return 0;
    p = module_symbol(h, "salts_cflow_probe_read");
    if (!p) return 0;
    *(void **)read = p;
    p = module_symbol(h, "salts_cflow_probe_borrowed");
    if (!p) return 0;
    *(void **)borrow = p;
    p = module_symbol(h, "salts_cflow_probe_semantic_equal");
    if (!p) return 0;
    *(void **)equal = p;
    return 1;
}

int main(int argc, char **argv) {
    module_handle first, second;
    read_fn read_first = NULL, read_second = NULL;
    borrow_fn borrow_first = NULL, borrow_second = NULL;
    equal_fn equal_first = NULL, equal_second = NULL;
    salts_cflow_probe_snapshot a = {0}, b = {0}, survivor = {0};
    const void *a_desc, *b_desc;
    int shared;
    if (argc != 4 ||
        (strcmp(argv[3], "isolated") != 0 &&
         strcmp(argv[3], "shared") != 0)) {
        fputs("usage: cflow_cross_loader <client> <server> isolated|shared\n", stderr);
        return 2;
    }
    first = module_open(argv[1], 1);
    if (!first) return 3;
    second = module_open(argv[2], 0);
    if (!second) { (void)module_close(first); return 4; }
    if (!load_api(first, &read_first, &borrow_first, &equal_first) ||
        !load_api(second, &read_second, &borrow_second, &equal_second))
        return 5;
    if (!read_first(&a) || !read_second(&b)) {
        fputs("CFLOW_CROSS_INVALID_ACTUAL_REFLECTION\n", stderr);
        return 6;
    }
    if (a.descriptor_address == 0 || b.descriptor_address == 0 ||
        a.pointee_address == 0 || b.pointee_address == 0 ||
        a.size != sizeof(void *) || a.size != b.size ||
        a.align != b.align || a.pointee_size == 0 ||
        a.pointee_size != b.pointee_size || a.pointee_align != b.pointee_align ||
        a.kind != b.kind || a.pointee_kind != b.pointee_kind ||
        strcmp(a.name, "cflow_executor_protocol_stats *") != 0 ||
        strcmp(a.name, b.name) != 0 ||
        strcmp(a.pointee_name, "cflow_executor_protocol_stats") != 0 ||
        strcmp(a.pointee_name, b.pointee_name) != 0) {
        fputs("CFLOW_CROSS_DSO_DESCRIPTOR_SHAPE_MISMATCH\n", stderr);
        return 7;
    }
    shared = a.descriptor_address == b.descriptor_address;
    if (shared != (strcmp(argv[3], "shared") == 0)) {
        fprintf(stderr, "CFLOW_CROSS_OWNER_MISMATCH expected=%s actual=%s\n",
                argv[3], shared ? "shared" : "isolated");
        return 8;
    }
    a_desc = borrow_first();
    b_desc = borrow_second();
    if (!a_desc || !b_desc ||
        (uintptr_t)a_desc != a.descriptor_address ||
        (uintptr_t)b_desc != b.descriptor_address ||
        equal_first(b_desc) != 1 || equal_second(a_desc) != 1) {
        fputs("CFLOW_CROSS_PUBLIC_SEMANTIC_EQUAL_FAILED\n", stderr);
        return 9;
    }
    puts("CFLOW_CROSS_TYPE_SHAPE_EQUAL=1");
    puts("CFLOW_CROSS_PUBLIC_SEMANTIC_EQUAL_BOTH=1");
    printf("CFLOW_CROSS_OWNER=%s\n", shared ? "shared-single" : "image-local");
    fflush(stdout);
    /* End SECOND's descriptor lease now. Never access b_desc again. */
    if (!module_close(second)) {
        fputs("CFLOW_CROSS_SECOND_UNLOAD_FAILED\n", stderr);
        return 10;
    }
    if (!read_first(&survivor) ||
        survivor.descriptor_address != a.descriptor_address ||
        survivor.pointee_address != a.pointee_address ||
        strcmp(survivor.name, a.name) != 0 ||
        equal_first(a_desc) != 1) {
        fputs("CFLOW_CROSS_LIVE_OWNER_DAMAGED_AFTER_UNLOAD\n", stderr);
        return 11;
    }
    puts("CFLOW_CROSS_LIVE_FIRST_OWNER_OK");
    if (!module_close(first)) {
        fputs("CFLOW_CROSS_FIRST_UNLOAD_FAILED\n", stderr);
        return 12;
    }
    /* Only copied value snapshots are live after the final module close. */
    puts("CFLOW_CROSS_TWO_DSO_LOAD_UNLOAD_OK");
    return 0;
}
