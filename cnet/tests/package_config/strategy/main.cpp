#include <type_traits>
#include <cnet/name_lookup.h>
#include <cnet/sg_host.h>
#include <cnet/managed_dial.h>
#include <cnet/client_pool.h>
#include <salts/native_io_sharded.h>
static_assert(std::is_standard_layout<cnet_tls_client_config>::value,
              "RFC8998 installed client TLS policy must be stable C ABI data");
static_assert(std::is_standard_layout<cnet_tls_server_config>::value,
              "RFC8998 installed server TLS policy must be stable C ABI data");
static_assert(std::is_standard_layout<cnet_name_query>::value,
              "installed name lookup query identity must be C ABI data");
static_assert(std::is_standard_layout<cnet_ip_address>::value,
              "installed numeric address must be C ABI data");
static_assert(std::is_standard_layout<cnet_pool_lease>::value,
              "installed pool lease must be C ABI data");
static_assert(std::is_standard_layout<cnet_reconnect_ticket>::value,
              "installed reconnect ticket must be C ABI data");
static_assert(std::is_standard_layout<native_io_sharded_host_lease>::value,
              "installed host lease must be C ABI data");
#include "main.c"
