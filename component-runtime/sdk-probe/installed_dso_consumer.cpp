#include <type_traits>
#include "installed_dso_scenario.h"
static_assert(std::is_standard_layout<salts_service_host_scope>::value,
              "ServiceHost scope must use the public C ABI");
static_assert(std::is_standard_layout<salts_configurator_context>::value,
              "Configurator context must use the public C ABI");
int main(int argc, char **argv) { return installed_dso_run(argc, argv); }
