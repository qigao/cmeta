#ifndef CMETA_PLUGIN_LINKER_H
#define CMETA_PLUGIN_LINKER_H

#include <cmeta/plugin_decl.h>

/* One explicit discovery set per DSO. Link fragments as objects (or explicitly
 * extract their archive members). The checked count makes omission fail closed.
 * Rows are immutable after DSO initialization; their order is linker-defined. */
#define CMETA_PLUGIN_HAS_LINKER_EXPORTS (CMETA_SECTION_BACKEND != CMETA_SECTION_NONE)

#if CMETA_PLUGIN_HAS_LINKER_EXPORTS
#define CMETA_PLUGIN_LINKER_ALIGNMENT 8
#ifdef __cplusplus
#define CMETA_PLUGIN_LINKER_EXTERN extern "C"
#else
#define CMETA_PLUGIN_LINKER_EXTERN extern
#endif

#if CMETA_SECTION_BACKEND == CMETA_SECTION_COFF
#define CMETA_PLUGIN_LINKER_STORAGE CMETA_ATTR_SECTION(".sltexp$m")
#define CMETA_PLUGIN_LINKER_KEEP(name) CMETA_COFF_RETAIN(CMETA_PP_STRINGIFY(name))
#define CMETA_PLUGIN_LINKER_LOCAL
/* Full rows keep sentinel size/alignment identical to every fragment. */
#define CMETA_PLUGIN_LINKER_BOUNDS \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export cmeta_plugin_linker_begin; \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export cmeta_plugin_linker_end; \
    CMETA_ATTR_SECTION(".sltexp$a") CMETA_ATTR_ALIGNED(CMETA_PLUGIN_LINKER_ALIGNMENT) \
    const cmeta_plugin_export cmeta_plugin_linker_begin = {0}; \
    CMETA_ATTR_SECTION(".sltexp$z") CMETA_ATTR_ALIGNED(CMETA_PLUGIN_LINKER_ALIGNMENT) \
    const cmeta_plugin_export cmeta_plugin_linker_end = {0}; \
    CMETA_PLUGIN_LINKER_KEEP(cmeta_plugin_linker_begin) \
    CMETA_PLUGIN_LINKER_KEEP(cmeta_plugin_linker_end)
#define CMETA_PLUGIN_LINKER_FIRST (&cmeta_plugin_linker_begin + 1)
#define CMETA_PLUGIN_LINKER_LAST (&cmeta_plugin_linker_end)
#elif CMETA_SECTION_BACKEND == CMETA_SECTION_ELF
#define CMETA_PLUGIN_LINKER_STORAGE CMETA_ATTR_SECTION("cmeta_exports") CMETA_ATTR_USED CMETA_ATTR_RETAIN
#define CMETA_PLUGIN_LINKER_KEEP(name)
#define CMETA_PLUGIN_LINKER_LOCAL CMETA_ATTR_HIDDEN
#define CMETA_PLUGIN_LINKER_BOUNDS \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export __start_cmeta_exports[] CMETA_ATTR_HIDDEN; \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export __stop_cmeta_exports[] CMETA_ATTR_HIDDEN;
#define CMETA_PLUGIN_LINKER_FIRST __start_cmeta_exports
#define CMETA_PLUGIN_LINKER_LAST __stop_cmeta_exports
#elif CMETA_SECTION_BACKEND == CMETA_SECTION_MACHO
#define CMETA_PLUGIN_LINKER_STORAGE CMETA_ATTR_SECTION("__DATA,cmeta_exports") CMETA_ATTR_USED
#define CMETA_PLUGIN_LINKER_KEEP(name)
#define CMETA_PLUGIN_LINKER_LOCAL CMETA_ATTR_HIDDEN
#define CMETA_PLUGIN_LINKER_BOUNDS \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export cmeta_plugin_linker_begin[] \
        __asm__("section$start$__DATA$cmeta_exports"); \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export cmeta_plugin_linker_end[] \
        __asm__("section$end$__DATA$cmeta_exports");
#define CMETA_PLUGIN_LINKER_FIRST cmeta_plugin_linker_begin
#define CMETA_PLUGIN_LINKER_LAST cmeta_plugin_linker_end
#endif

/* The section alignment must divide every row size. Fixed 8-byte alignment
 * supports the current 32/64-bit ABI; other layouts are rejected at compile time. */
CMETA_STATIC_ASSERT(CMETA_ALIGNOF(cmeta_plugin_export) <= CMETA_PLUGIN_LINKER_ALIGNMENT &&
    sizeof(cmeta_plugin_export) % CMETA_PLUGIN_LINKER_ALIGNMENT == 0,
    "Plugin linker row alignment unsupported");

#define CMETA_PLUGIN_EXPORT_FRAGMENT(name,exports) \
    CMETA_PLUGIN_EXPORT_FRAGMENT_I(name,exports)
#define CMETA_PLUGIN_EXPORT_FRAGMENT_I(name,exports) \
    exports(CMETA_PLUGIN_DECL_CHECK) \
    exports(CMETA_PLUGIN_DECL_BRIDGE) \
    CMETA_PLUGIN_LINKER_EXTERN const cmeta_plugin_export name[] CMETA_PLUGIN_LINKER_LOCAL; \
    CMETA_PLUGIN_LINKER_STORAGE CMETA_ATTR_ALIGNED(CMETA_PLUGIN_LINKER_ALIGNMENT) \
    const cmeta_plugin_export name[] = { exports(CMETA_PLUGIN_DECL_ROW) }; \
    CMETA_PLUGIN_LINKER_KEEP(name) \
    CMETA_STATIC_ASSERT(sizeof(name)/sizeof(name[0]) > 0 && \
        sizeof(name)/sizeof(name[0]) <= CMETA_PLUGIN_MAX_EXPORTS, \
        "Plugin fragment count must be within capacity")

#define CMETA_PLUGIN_DECLARE_LINKER(name,id,version,count,lifecycle) \
    CMETA_PLUGIN_DECLARE_LINKER_I(name,id,version,count,lifecycle)
#define CMETA_PLUGIN_DECLARE_LINKER_I(name,id,version,count,lifecycle) \
    CMETA_STATIC_ASSERT((count) > 0 && (count) <= CMETA_PLUGIN_MAX_EXPORTS && \
        (((count) & UINT32_MAX) == (count)), \
        "Plugin linker count must be an integer within capacity"); \
    CMETA_PLUGIN_LINKER_BOUNDS \
    CMETA_PP_TUPLE_APPLY(CMETA_PLUGIN_DECL_VERSION,version) \
    CMETA_PP_TUPLE_APPLY(CMETA_PLUGIN_DECL_LIFE_CHECK_I,lifecycle) \
    static const cmeta_plugin_manifest name##__manifest = { \
        CMETA_PLUGIN_MANIFEST_SIZE,CMETA_PLUGIN_ABI_VERSION,(id), \
        {CMETA_PP_UNPAREN version},CMETA_PLUGIN_LINKER_FIRST,(count), \
        CMETA_PP_TUPLE_APPLY(CMETA_PLUGIN_DECL_LIFE_I,lifecycle) \
    }; \
    CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL \
    cmeta_plugin_query(uint32_t host_abi) { \
        const uintptr_t first = (uintptr_t)(const void *)CMETA_PLUGIN_LINKER_FIRST; \
        const uintptr_t last = (uintptr_t)(const void *)CMETA_PLUGIN_LINKER_LAST; \
        if (host_abi != CMETA_PLUGIN_ABI_VERSION || last < first || \
            last - first != (count) * sizeof(cmeta_plugin_export)) return NULL; \
        return &name##__manifest; \
    } \
    typedef char name##__plugin_linker_declaration_complete[1]
#endif

#endif
