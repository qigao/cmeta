#include <cflow/cflow.h>
#include <cflow/status.h>
#include <cmeta/cmeta.h>
#include <native_io.h>
#include <threads/threads.h>
#include <core/error_codes.h>
#include <core/api.h>
#include <cstring>
#include <type_traits>

static_assert(std::is_standard_layout<cflow_graph>::value,
              "Installed CFlow graph must remain a C-compatible record");
int main() {
  core_runtime_abi_0_1_required();
  cflow_graph graph{};
  native_io_backend backend{};
  cflow_graph_init(&graph, &cmeta_type_int);
  const bool valid =
      core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION &&
      cflow_graph_input_type(&graph) != nullptr &&
      cflow_graph_take(&graph, 1u) &&
      !native_io_endpoint_valid(native_io_endpoint{}) &&
      native_io_backend_init(&backend, nullptr) == SALTS_EINVAL &&
      backend.impl == nullptr &&
      sync_thread_current_token() != nullptr &&
      std::strcmp(cflow_status_string(CFLOW_STATUS_OK), "ok") == 0;
  cflow_graph_destroy(&graph);
  return valid ? 0 : 1;
}
