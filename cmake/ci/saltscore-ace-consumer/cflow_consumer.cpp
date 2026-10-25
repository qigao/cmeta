#include <cflow/cflow.h>
#include <cflow/status.h>
#include <cmeta/cmeta.h>
#include <salts/native_io.h>
#include <salts_core/runtime_abi.h>
#include <cstring>
#include <type_traits>

static_assert(std::is_standard_layout<cflow_graph>::value,
              "Installed CFlow graph must remain a C-compatible record");
int main() {
  salts_core_runtime_abi_0_1_required();
  cflow_graph graph{};
  cflow_graph_init(&graph, &cmeta_type_int);
  const bool valid =
      salts_core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION &&
      cflow_graph_input_type(&graph) != nullptr &&
      cflow_graph_take(&graph, 1u) &&
      !native_io_endpoint_valid(native_io_endpoint{}) &&
      std::strcmp(cflow_status_string(CFLOW_STATUS_OK), "ok") == 0;
  cflow_graph_destroy(&graph);
  return valid ? 0 : 1;
}
