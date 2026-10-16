#include <salts/component.h>
#include <salts/configurator.h>
#include <salts/component_plugin.h>
#include <salts/service_host.h>
#include <type_traits>

static_assert(!std::is_same<salts_component_context, salts_configurator_context>::value,
              "Configurator and Component have independently-owned context records");
static_assert(!std::is_same<salts_component_plugin_scope, salts_service_host_scope>::value,
              "Scope admission belongs to independent publication hosts");
static_assert(std::is_standard_layout<salts_service_host_scope>::value,
              "Candidate scope must preserve C-compatible layout");
static_assert(std::is_standard_layout<salts_component_plugin_scope>::value,
              "Original scope must preserve C-compatible layout");

int main() {
    salts_component_plugin_scope legacy = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_service_host_scope candidate = SALTS_SERVICE_HOST_SCOPE_INIT;
    return legacy.live || candidate.live ||
                   legacy.owner_address || candidate.owner_address
               ? 1 : 0;
}
