# Copied/renamed behavior tests from the corresponding existing modules.
# All candidates are compiled only with SALTS_ENABLE_COMPONENT_RUNTIME_CANDIDATE=ON.
cmake_add_test(cmeta_native_configurator_candidate_test
  SOURCES configurator_test.c
  LIBS Salts::ConfiguratorCandidate Salts::TinyTest
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_configurator_candidate_cpp_test
  SOURCES configurator_cpp_test.cpp
  LIBS Salts::ConfiguratorCandidate Salts::TinyTest
  FOLDER "component-runtime/tests")
set_target_properties(cmeta_native_configurator_candidate_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_native_configurator_candidate_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Real provider DSO uses the existing provider ABI; only the host uses new APIs.
add_library(salts_service_host_candidate_provider_fixture SHARED
  fixture_component_provider.c)
target_link_libraries(salts_service_host_candidate_provider_fixture
  PRIVATE Salts::ComponentPluginABI Salts::PluginABI)
set_target_properties(salts_service_host_candidate_provider_fixture PROPERTIES
  FOLDER "component-runtime/tests/fixtures"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

cmake_add_test(cmeta_native_service_host_candidate_generation_test
  SOURCES service_host_generation_test.c
  LIBS Salts::ServiceHostCandidate Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_candidate_provider_fixture>\""
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_candidate_publication_test
  SOURCES service_host_publication_test.c
  LIBS Salts::ServiceHostCandidate Salts::NativeIO Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_candidate_provider_fixture>\""
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_candidate_concurrency_test
  SOURCES service_host_concurrency_test.c
  LIBS Salts::ServiceHostCandidate Salts::Platform Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_candidate_provider_fixture>\""
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_candidate_exclusive_fence_test
  SOURCES service_host_exclusive_fence_test.c
  LIBS Salts::ServiceHostCandidate Salts::TinyTest
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_candidate_scope_cpp_test
  SOURCES service_host_scope_cpp_test.cpp
  LIBS Salts::ServiceHostCandidate
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_candidate_runtime_failure_test
  SOURCES service_host_runtime_failure_test.c ../src/service_host_runtime.c
  LIBS Salts::ConfiguratorCandidate Salts::ComponentPluginABI Salts::Platform Salts::TinyTest
  FOLDER "component-runtime/tests")

foreach(candidate_test IN ITEMS
  cmeta_native_service_host_candidate_generation_test
  cmeta_native_service_host_candidate_publication_test
  cmeta_native_service_host_candidate_concurrency_test)
  add_dependencies(${candidate_test} salts_service_host_candidate_provider_fixture)
endforeach()
foreach(candidate_c_test IN ITEMS
  cmeta_native_service_host_candidate_generation_test
  cmeta_native_service_host_candidate_publication_test
  cmeta_native_service_host_candidate_concurrency_test
  cmeta_native_service_host_candidate_exclusive_fence_test
  cmeta_native_service_host_candidate_runtime_failure_test)
  set_target_properties(${candidate_c_test} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()
set_target_properties(cmeta_native_service_host_candidate_scope_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set_tests_properties(cmeta_native_service_host_candidate_concurrency_test PROPERTIES TIMEOUT 60)
