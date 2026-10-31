#ifndef CMETA_COMPONENT_H
#define CMETA_COMPONENT_H

#include <cmeta/manifest.h>
#include <cmeta/interface.h>
#include <cmeta/data.h>

#include <stdbool.h>

#define CMETA_COMPONENT_DECLARATION_VERSION UINT32_C(2)

typedef enum cmeta_component_role {
    CMETA_COMPONENT_PROVIDES = 1,
    CMETA_COMPONENT_REQUIRES = 2
} cmeta_component_role;

/* Membership only: canonical interface rows remain the semantic authority.
 * No handle, callback, module state or lease is stored in this declaration. */
typedef struct cmeta_component_capability {
    cmeta_component_role role;
    const cmeta_interface_desc *interface_desc;
} cmeta_component_capability;

/* Format 2 is exact-size; metadata is borrowed under the provider's negotiated
 * Reflection ABI and lifetime. stable_id is the semantic component/provider
 * identity used for explicit selection and diagnostics; generated declarations
 * use the expanded C identifier spelling. Descriptor address is never identity.
 * config is an optional borrowed canonical DataDesc describing the exact native
 * configuration value accepted by the runtime provider binding. When present,
 * config must have a valid native storage_type; kind-only container DataDesc
 * is not sufficient for a Component configuration. CMeta does not
 * parse external configuration formats. Manual empty tables and repeated/ordered
 * role rows are preserved. */
typedef struct cmeta_component_desc {
    size_t size;
    uint32_t format_version;
    const char *stable_id;
    const cmeta_data_desc *config;
    const cmeta_component_capability *capabilities;
    size_t capability_count;
} cmeta_component_desc;

#ifdef __cplusplus
extern "C" {
#endif

/** Validate one immutable canonical component declaration. */
bool cmeta_component_desc_valid(const cmeta_component_desc *desc);

#ifdef __cplusplus
}
#endif

/* Flat nonempty 1..16 role rows, using the existing PP schema machinery. */
#define CMETA_COMPONENT_ROW_(role_, interface_) \
    {(role_), CMETA_MANIFEST_TYPED_POINTER_(cmeta_interface_desc, interface_)}
#define CMETA_COMPONENT_ROW_APPLY_(row_, ignored_) CMETA_COMPONENT_ROW_ row_
#define CMETA_COMPONENT_DESC_(name_, config_, capabilities_, count_) \
    CMETA_LOCAL const cmeta_component_desc CMETA_PP_CAT(name_,__component_meta) = { \
        sizeof(cmeta_component_desc), CMETA_COMPONENT_DECLARATION_VERSION, \
        CMETA_PP_STRINGIFY(name_), (config_), (capabilities_), (count_) \
    }; \
    typedef char CMETA_PP_CAT(name_,__component_declaration_complete)[1]
#define CMETA_COMPONENT_DECLARE_(name_, config_, sentinel_, ...) \
    CMETA_LOCAL const cmeta_component_capability CMETA_PP_CAT(name_,__component_capabilities)[] = { \
        CMETA_PP_MAP_COMMA(CMETA_COMPONENT_ROW_APPLY_, ~, __VA_ARGS__) \
    }; \
    CMETA_COMPONENT_DESC_(name_, (config_), \
        CMETA_PP_CAT(name_,__component_capabilities), \
        sizeof(CMETA_PP_CAT(name_,__component_capabilities)) / \
        sizeof(CMETA_PP_CAT(name_,__component_capabilities)[0]))
#define CMETA_COMPONENT_EXPAND_(...) CMETA_COMPONENT_DECLARE_(__VA_ARGS__)
#define cmeta_component(name_, ...) \
    CMETA_COMPONENT_EXPAND_(name_, NULL, ~ __VA_ARGS__)
#define cmeta_component_configured(name_, config_, ...) \
    CMETA_COMPONENT_EXPAND_(name_, \
        CMETA_MANIFEST_TYPED_POINTER_(cmeta_data_desc, config_), ~ __VA_ARGS__)
/* Empty membership still emits an ordinary immutable descriptor, not a live
 * Configurator instance or a discovery entry. Publication remains explicit. */
#define cmeta_component_empty(name_) \
    CMETA_COMPONENT_DESC_(name_, NULL, NULL, 0u)
#define cmeta_component_configured_empty(name_, config_) \
    CMETA_COMPONENT_DESC_(name_, \
        CMETA_MANIFEST_TYPED_POINTER_(cmeta_data_desc, config_), NULL, 0u)
#define cmeta_provides(interface_) \
    , (CMETA_COMPONENT_PROVIDES, &CMETA_PP_CAT(interface_, _interface_meta))
#define cmeta_requires(interface_) \
    , (CMETA_COMPONENT_REQUIRES, &CMETA_PP_CAT(interface_, _interface_meta))
#define cmeta_component_meta(name_) (&CMETA_PP_CAT(name_, __component_meta))

#endif
