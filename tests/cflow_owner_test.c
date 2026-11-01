/* Native CFlow regression on the single NativeIO owner. */
#include <cflow/cflow.h>
#include <cflow/status.h>
#include <cmeta/cmeta.h>
#include <native_io.h>
#include <thread.h>
#include <error_codes.h>
#include <runtime_abi.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "M6 installed CFlow C11 line %d: %s\n",__LINE__,#expr); \
  return __LINE__; } } while (0)

int main(void) {
  cflow_graph graph = {0};
  native_io_backend backend = {0};
  const cmeta_type_desc *type;
  salts_core_runtime_abi_0_1_required();
  CHECK(salts_core_runtime_abi_version() == SALTS_CORE_RUNTIME_ABI_VERSION);
  CHECK(strcmp(cflow_status_string(CFLOW_STATUS_OK), "ok") == 0);
  CHECK(cflow_status_result_is_ok((cflow_status_result){CFLOW_STATUS_OK}));
  CHECK(!native_io_endpoint_valid((native_io_endpoint){0}));
  /* Force a real NativeIO dynamic import, rather than merely including a
   * static-inline handle validator that permits --as-needed elision. */
  CHECK(native_io_backend_init(&backend, NULL) == SALTS_EINVAL);
  CHECK(backend.impl == NULL && cmeta_thread_current_token() != NULL);
  cflow_graph_init(&graph, &cmeta_type_int);
  type = cflow_graph_input_type(&graph);
  CHECK(type != NULL && cmeta_type_equal(type, &cmeta_type_int));
  CHECK(cflow_graph_take(&graph, 2u));
  cflow_graph_destroy(&graph);
  return 0;
}
