/* Compile the exact production lifecycle implementation against a test-local
 * resource adapter. Unique names keep the installed Platform DSO as the sole
 * owner of its exported cmeta_mutex_* symbols on every Windows ABI. */
#include "service_host_runtime_failure_adapter.h"

#define cmeta_mutex_init salts_service_host_test_mutex_init
#define cmeta_mutex_destroy salts_service_host_test_mutex_destroy
#define cmeta_mutex_lock salts_service_host_test_mutex_lock
#define cmeta_mutex_unlock salts_service_host_test_mutex_unlock

#include "../src/service_host_runtime.c"
