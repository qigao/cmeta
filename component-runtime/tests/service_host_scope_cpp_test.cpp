#include <salts/service_host.h>
#include <type_traits>

static_assert(std::is_standard_layout<salts_service_host_scope>::value,
              "A Component scope remains a C-compatible public record");

int main() {
    salts_service_host_scope scope = SALTS_SERVICE_HOST_SCOPE_INIT;
    return scope.runtime == nullptr &&
                   scope.generation == nullptr &&
                   scope.generation_id == UINT64_C(0) &&
                   scope.owner_address == nullptr && !scope.live
               ? 0
               : 1;
}
