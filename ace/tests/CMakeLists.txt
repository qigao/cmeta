# Canonical Configurator/ServiceHost tests on the only installed runtime.
# ABI v2 and the sole Configurator/ServiceHost owner.
cmake_add_test(cmeta_native_configurator_test
  SOURCES configurator_test.c
  LIBS Salts::Configurator Salts::TinyTest
  FOLDER "ace/tests")
cmake_add_test(cmeta_native_configurator_cpp_test
  SOURCES configurator_cpp_test.cpp
  LIBS Salts::Configurator Salts::TinyTest
  FOLDER "ace/tests")
set_target_properties(cmeta_native_configurator_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_native_configurator_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Real provider DSO uses the existing provider ABI; only the host uses new APIs.
add_library(salts_service_host_provider_fixture SHARED
  fixture_component_provider.c)
target_link_libraries(salts_service_host_provider_fixture
  PRIVATE Salts::ServiceHostProviderABI Salts::PluginABI)
set_target_properties(salts_service_host_provider_fixture PROPERTIES
  FOLDER "ace/tests/fixtures"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_library(salts_service_host_provider_secondary_fixture SHARED
  fixture_component_provider_secondary.c)
target_link_libraries(salts_service_host_provider_secondary_fixture
  PRIVATE Salts::ServiceHostProviderABI Salts::PluginABI)
set_target_properties(salts_service_host_provider_secondary_fixture PROPERTIES
  FOLDER "ace/tests/fixtures"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

cmake_add_test(cmeta_native_service_host_generation_test
  SOURCES service_host_generation_test.c
  LIBS Salts::ServiceHost Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_fixture>\""
       "COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_secondary_fixture>\""
  FOLDER "ace/tests")
cmake_add_test(cmeta_native_service_host_publication_test
  SOURCES service_host_publication_test.c
  LIBS Salts::ServiceHost Salts::NativeIO Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_fixture>\""
  FOLDER "ace/tests")
cmake_add_test(cmeta_native_service_host_concurrency_test
  SOURCES service_host_concurrency_test.c
  LIBS Salts::ServiceHost Salts::Platform Salts::TinyTest
  DEFS "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_service_host_provider_fixture>\""
  FOLDER "ace/tests")
cmake_add_test(cmeta_native_service_host_exclusive_fence_test
  SOURCES service_host_exclusive_fence_test.c
  LIBS Salts::ServiceHost Salts::TinyTest
  FOLDER "ace/tests")
cmake_add_test(cmeta_native_service_host_scope_cpp_test
  SOURCES service_host_scope_cpp_test.cpp
  LIBS Salts::ServiceHost
  FOLDER "ace/tests")
cmake_add_test(cmeta_native_service_host_runtime_failure_test
  SOURCES service_host_runtime_failure_test.c
          service_host_runtime_failure_adapter.c
  LIBS Salts::Configurator Salts::ServiceHostProviderABI Salts::Platform Salts::TinyTest
  FOLDER "ace/tests")

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

foreach(case IN ITEMS runtime_abi runtime_abi_cpp configurator_abi)
  if(case STREQUAL "runtime_abi_cpp")
    set(source runtime_abi_cpp_test.cpp)
  else()
    set(source ${case}_test.c)
  endif()
  cmake_add_test(cmeta_native_${case}_test
    SOURCES ${source}
    LIBS Salts::ServiceHost
    FOLDER "ace/tests")
endforeach()

foreach(index IN ITEMS 1 2)
  add_library(salts_service_host_abi_provider_${index} SHARED
    fixture_service_host_abi_provider.c)
  target_compile_definitions(salts_service_host_abi_provider_${index}
    PRIVATE ABI_TEST_PROVIDER_INDEX=${index})
  target_link_libraries(salts_service_host_abi_provider_${index}
    PRIVATE Salts::ServiceHostProviderABI Salts::PluginABI)
endforeach()
foreach(language IN ITEMS c cpp)
  if(language STREQUAL "c")
    set(source service_host_abi_dso_test.c)
  else()
    set(source service_host_abi_dso_cpp_test.cpp)
  endif()
  cmake_add_executable(cmeta_native_service_host_abi_dso_${language}_test
    SOURCES ${source}
    LIBS Salts::ServiceHost
    FOLDER "ace/tests")
  add_test(NAME cmeta_native_service_host_abi_dso_${language}_test
    COMMAND cmeta_native_service_host_abi_dso_${language}_test
      "$<TARGET_FILE:salts_service_host_abi_provider_1>"
      "$<TARGET_FILE:salts_service_host_abi_provider_2>")
  add_dependencies(cmeta_native_service_host_abi_dso_${language}_test
    salts_service_host_abi_provider_1 salts_service_host_abi_provider_2)
endforeach()

