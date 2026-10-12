#include <salts/component.h>
#include <salts/component_plugin.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION == UINT32_C(1),
              "Original Component provider binding ABI epoch must remain stable");
static_assert(SALTS_COMPONENT_PROVIDER_CONTRACT_VERSION == UINT32_C(1),
              "Original Component Plugin export contract must remain stable");
static_assert(std::is_standard_layout<salts_component_plugin_scope>::value,
              "Original ComponentPlugin scope must retain its C-compatible layout");
static_assert(offsetof(salts_component_plugin_scope, owner_address) >
                  offsetof(salts_component_plugin_scope, generation_id),
              "Scope identity sentinel must remain in the existing native ABI");
static_assert(offsetof(salts_component_plugin_scope, live) >
                  offsetof(salts_component_plugin_scope, owner_address),
              "Scope live flag must remain in the existing native ABI");

int main() {
    salts_component_context context = {};
    if (salts_component_context_init(
            &context, nullptr, 0u, nullptr, 0u, nullptr, 0u,
            nullptr, 0u, nullptr, 0u) != SALTS_COMPONENT_OK)
        return 1;
    if (salts_component_context_resolve(&context) != SALTS_COMPONENT_OK)
        return 2;
    if (salts_component_context_start(&context) != SALTS_COMPONENT_OK)
        return 3;
    if (salts_component_context_stop(&context) != SALTS_COMPONENT_OK)
        return 4;

    salts_component_plugin_runtime runtime = {};
    if (salts_component_plugin_runtime_init(&runtime) != SALTS_COMPONENT_PLUGIN_OK)
        return 5;
    if (salts_component_plugin_runtime_destroy(&runtime) != SALTS_COMPONENT_PLUGIN_OK)
        return 6;
    return 0;
}
