#ifndef CMETA_PLUGIN_DECL_H
#define CMETA_PLUGIN_DECL_H

#include <cmeta/plugin.h>
#include <cmeta/invoke_decl.h>

/* Publication is explicit and immutable. No registration, allocation or lease
 * acquisition occurs here. The loader still validates every foreign manifest.
 * An export list uses X(kind, source, export_id, contract_id, version, caps).
 * function source: FunctionInvokeDecl symbol; interface source: (Type, &value).
 * Each native function appears once per list; use distinct wrappers for aliases. */
#define CMETA_PLUGIN_DECL_CHECK(kind,source,id,contract,version,caps) \
    CMETA_STATIC_ASSERT((version) > 0 && (((version) & UINT32_MAX) == (version)), \
        "Plugin contract version must fit a nonzero uint32_t"); \
    CMETA_STATIC_ASSERT((caps) >= 0 && (((caps) & UINT64_MAX) == (caps)), \
        "Plugin capabilities must fit a nonnegative uint64_t bit set"); \
    CMETA_PP_CAT(CMETA_PLUGIN_DECL_CHECK_,kind)(source)
#define CMETA_PLUGIN_DECL_CHECK_function(source) \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&FunctionInvoke(source),cmeta_exact_invoke_fn), \
        "Plugin function requires a canonical exact invoke declaration");
#define CMETA_PLUGIN_DECL_CHECK_interface(source) \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(CMETA_PP_TUPLE_GET_1(source), \
        CMETA_PP_TUPLE_GET_0(source) *), "Plugin interface carrier type mismatch");

#define CMETA_PLUGIN_DECL_BRIDGE(kind,source,id,contract,version,caps) \
    CMETA_PP_CAT(CMETA_PLUGIN_DECL_BRIDGE_,kind)(source)
#define CMETA_PLUGIN_DECL_BRIDGE_function(source) \
    CMETA_LOCAL bool CMETA_PLUGIN_CALL source##__plugin_invoke( \
        void *context, void *result, void *const *params, size_t count) { \
        if (context != NULL) return false; \
        return FunctionInvoke(source)(result,params,count); \
    }
#define CMETA_PLUGIN_DECL_BRIDGE_interface(source)

/* C++17 cannot designate a non-first union member in an aggregate initializer.
 * These local value constructors initialize the immutable table; they never
 * mutate a registry or publish anything outside the explicit manifest. */
#ifdef __cplusplus
CMETA_INLINE cmeta_plugin_export_value cmeta_plugin_decl_function_value(
    const cmeta_function_desc *desc, const cmeta_function_abi_desc *abi,
    cmeta_plugin_function_invoke_fn invoke) {
    cmeta_plugin_export_value value = {};
    value.function = {desc, abi, nullptr, invoke};
    return value;
}
#define CMETA_PLUGIN_DECL_VALUE_function(source) \
    cmeta_plugin_decl_function_value(FunctionMeta(source),FunctionAbi(source), \
        CMETA_PP_CAT(source,__plugin_invoke))
#else
#define CMETA_PLUGIN_DECL_VALUE_function(source) \
    { .function = {FunctionMeta(source),FunctionAbi(source),NULL, \
        CMETA_PP_CAT(source,__plugin_invoke)} }
#endif
#define CMETA_PLUGIN_DECL_VALUE_interface(source) \
    { { &CMETA_PP_CAT(CMETA_PP_TUPLE_GET_0(source),_interface_meta), \
        CMETA_PP_TUPLE_GET_1(source) } }
#define CMETA_PLUGIN_DECL_KIND_function CMETA_PLUGIN_EXPORT_FUNCTION
#define CMETA_PLUGIN_DECL_KIND_interface CMETA_PLUGIN_EXPORT_INTERFACE
#define CMETA_PLUGIN_DECL_ROW(kind,source,id,contract,version,caps) \
    { CMETA_PLUGIN_EXPORT_SIZE,CMETA_PP_CAT(CMETA_PLUGIN_DECL_KIND_,kind), \
      (version),(caps),(id),(contract), \
      CMETA_PP_CAT(CMETA_PLUGIN_DECL_VALUE_,kind)(source) },

/* Managed callbacks are function identifiers, not nullable expressions. */
#define CMETA_PLUGIN_PASSIVE() (passive,~)
#define CMETA_PLUGIN_LIFECYCLE(self,start,stop,quiet,destroy) \
    (managed,self,start,stop,quiet,destroy)
#define CMETA_PLUGIN_DECL_LIFE_CHECK_passive(ignored)
#define CMETA_PLUGIN_DECL_LIFE_CHECK_managed(self,start,stop,quiet,destroy) \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&(start),cmeta_plugin_start_fn), \
        "Plugin start callback type mismatch"); \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&(stop),cmeta_plugin_request_stop_fn), \
        "Plugin stop callback type mismatch"); \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&(quiet),cmeta_plugin_is_quiescent_fn), \
        "Plugin quiescence callback type mismatch"); \
    CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&(destroy),cmeta_plugin_destroy_fn), \
        "Plugin destroy callback type mismatch");
#define CMETA_PLUGIN_DECL_LIFE_CHECK_I(kind,...) \
    CMETA_PP_CAT(CMETA_PLUGIN_DECL_LIFE_CHECK_,kind)(__VA_ARGS__)
#define CMETA_PLUGIN_DECL_LIFE_passive(ignored) NULL,NULL,NULL,NULL,NULL
#define CMETA_PLUGIN_DECL_LIFE_managed(self,start,stop,quiet,destroy) \
    (self),(start),(stop),(quiet),(destroy)
#define CMETA_PLUGIN_DECL_LIFE_I(kind,...) \
    CMETA_PP_CAT(CMETA_PLUGIN_DECL_LIFE_,kind)(__VA_ARGS__)

/* Runtime fields are uint32_t. Reject truncation before initializing them;
 * unlike a contract version, a semantic-version component may be zero. */
#define CMETA_PLUGIN_DECL_VERSION_PART(part) \
    CMETA_STATIC_ASSERT((part) >= 0 && (((part) & UINT32_MAX) == (part)), \
        "Plugin semantic version component must fit uint32_t");
#define CMETA_PLUGIN_DECL_VERSION(major,minor,patch) \
    CMETA_PLUGIN_DECL_VERSION_PART(major) \
    CMETA_PLUGIN_DECL_VERSION_PART(minor) \
    CMETA_PLUGIN_DECL_VERSION_PART(patch)

/* version is a (major,minor,patch) tuple; exports is a nonempty X-list.
 * The generated query belongs in exactly one TU per DSO. */
#define CMETA_PLUGIN_DECLARE(...) CMETA_PLUGIN_DECLARE_I(__VA_ARGS__)
#define CMETA_PLUGIN_DECLARE_I(name,id,version,exports,lifecycle) \
    exports(CMETA_PLUGIN_DECL_CHECK) \
    exports(CMETA_PLUGIN_DECL_BRIDGE) \
    static const cmeta_plugin_export name##__exports[] = { \
        exports(CMETA_PLUGIN_DECL_ROW) \
    }; \
    CMETA_STATIC_ASSERT(sizeof(name##__exports)/sizeof(name##__exports[0]) \
        <= CMETA_PLUGIN_MAX_EXPORTS, "Plugin export capacity exceeded"); \
    CMETA_PLUGIN_DECL_MANIFEST(name,id,version,name##__exports, \
        sizeof(name##__exports)/sizeof(name##__exports[0]),lifecycle)

/* Empty manifests have no export array, including in strict C11/C++17. */
#define CMETA_PLUGIN_DECLARE_EMPTY(name,id,version,lifecycle) \
    CMETA_PLUGIN_DECL_MANIFEST(name,id,version,NULL,0u,lifecycle)

#define CMETA_PLUGIN_DECL_MANIFEST(name,id,version,exports,count,lifecycle) \
    CMETA_PP_TUPLE_APPLY(CMETA_PLUGIN_DECL_VERSION,version) \
    CMETA_PP_TUPLE_APPLY(CMETA_PLUGIN_DECL_LIFE_CHECK_I,lifecycle) \
    static const cmeta_plugin_manifest name##__manifest = { \
        CMETA_PLUGIN_MANIFEST_SIZE,CMETA_PLUGIN_ABI_VERSION,(id), \
        {CMETA_PP_UNPAREN version},(exports),(count), \
        CMETA_PP_TUPLE_APPLY(CMETA_PLUGIN_DECL_LIFE_I,lifecycle) \
    }; \
    CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL \
    cmeta_plugin_query(uint32_t host_abi) { \
        return host_abi == CMETA_PLUGIN_ABI_VERSION ? &name##__manifest : NULL; \
    } \
    typedef char name##__plugin_declaration_complete[1]

#endif
