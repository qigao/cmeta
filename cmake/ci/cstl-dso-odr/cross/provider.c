/* Borrow CMeta generic identity only while this source-built CSTL DSO lives. */
#include <cstl/detail/instance_meta.h>
#include <cmeta/type_identity.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#  define CSTL_PROBE_PUBLIC __declspec(dllexport)
#elif defined(__GNUC__)
#  define CSTL_PROBE_PUBLIC __attribute__((visibility("default")))
#else
#  define CSTL_PROBE_PUBLIC
#endif
typedef struct probe_snapshot {
    uintptr_t address;
    uintptr_t container;
    unsigned min_arity;
    unsigned max_arity;
    unsigned category;
    char stable_id[128];
} probe_snapshot;
CSTL_PROBE_PUBLIC int salts_cstl_probe_read(probe_snapshot *result) {
    const cmeta_generic_desc *g=&stl_bplus_tree_generic_desc;
    const cmeta_container_desc *c=&stl_bplus_tree_container_desc;
    if(!result||!g->stable_id||strlen(g->stable_id)==0||
       strlen(g->stable_id)>=sizeof(result->stable_id)||!c) return 0;
    memset(result,0,sizeof(*result));
    result->address=(uintptr_t)g;
    result->container=(uintptr_t)c;
    result->min_arity=g->min_arity;
    result->max_arity=g->max_arity;
    result->category=(unsigned)g->category;
    strcpy(result->stable_id,g->stable_id);
    return 1;
}
CSTL_PROBE_PUBLIC const void *salts_cstl_probe_borrow(void) {
    return &stl_bplus_tree_generic_desc;
}
CSTL_PROBE_PUBLIC int salts_cstl_probe_equal(const void *foreign) {
    return foreign && cmeta_generic_desc_equal(
        &stl_bplus_tree_generic_desc,(const cmeta_generic_desc *)foreign);
}
