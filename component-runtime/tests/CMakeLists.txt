# Canonical Configurator/ServiceHost tests on the only installed runtime.
# No Component/ComponentPlugin coexistence or alternate Owner.
cmake_add_test(cmeta_native_configurator_test
  SOURCES configurator_test.c
  LIBS Salts::Configurator Salts::TinyTest
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_configurator_cpp_test
  SOURCES configurator_cpp_test.cpp
  LIBS Salts::Configurator Salts::TinyTest
  FOLDER "component-runtime/tests")
set_target_properties(cmeta_native_configurator_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_native_configurator_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Real provider DSO uses the existing provider ABI; only the host uses new APIs.
add_library(salts_service_host_provider_fixture SHARED
  fixture_component_provider.c)
target_link_libraries(salts_service_host_provider_fixture
  PRIVATE Salts::ComponentProviderABI Salts::PluginABI)
set_target_properties(salts_service_host_provider_fixture PROPERTIES
  FOLDER "component-runtime/tests/fixtures"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_library(salts_service_host_provider_secondary_fixture SHARED
  fixture_component_provider_secondary.c)
target_link_libraries(salts_service_host_provider_secondary_fixture
  PRIVATE Salts::ComponentProviderABI Salts::PluginABI)
set_target_properties(salts_service_host_provider_secondary_fixture PROPERTIES
  FOLDER "component-runtime/tests/fixtures"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

cmake_add_test(cmeta_native_service_host_generation_test
  SOURCES service_host_generation_test.c
  LIBS Salts::ServiceHost Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_fixture>\""
       "COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_secondary_fixture>\""
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_publication_test
  SOURCES service_host_publication_test.c
  LIBS Salts::ServiceHost Salts::NativeIO Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_fixture>\""
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_concurrency_test
  SOURCES service_host_concurrency_test.c
  LIBS Salts::ServiceHost Salts::Platform Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_fixture>\""
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_exclusive_fence_test
  SOURCES service_host_exclusive_fence_test.c
  LIBS Salts::ServiceHost Salts::TinyTest
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_scope_cpp_test
  SOURCES service_host_scope_cpp_test.cpp
  LIBS Salts::ServiceHost
  FOLDER "component-runtime/tests")
cmake_add_test(cmeta_native_service_host_runtime_failure_test
  SOURCES service_host_runtime_failure_test.c ../src/service_host_runtime.c
  LIBS Salts::Configurator Salts::ComponentProviderABI Salts::Platform Salts::TinyTest
  FOLDER "component-runtime/tests")

foreach(host_test IN ITEMS
  cmeta_native_service_host_generation_test
  cmeta_native_service_host_publication_test
  cmeta_native_service_host_concurrency_test)
  add_dependencies(${host_test} salts_service_host_provider_fixture)
endforeach()
add_dependencies(cmeta_native_service_host_generation_test
  salts_service_host_provider_secondary_fixture)
foreach(host_c_test IN ITEMS
  cmeta_native_service_host_generation_test
  cmeta_native_service_host_publication_test
  cmeta_native_service_host_concurrency_test
  cmeta_native_service_host_exclusive_fence_test
  cmeta_native_service_host_runtime_failure_test)
  set_target_properties(${host_c_test} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()
set_target_properties(cmeta_native_service_host_scope_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set_tests_properties(cmeta_native_service_host_concurrency_test PROPERTIES TIMEOUT 60)

