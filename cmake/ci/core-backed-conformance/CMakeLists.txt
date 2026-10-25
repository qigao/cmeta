cmake_minimum_required(VERSION 3.20)
project(Salts3OriginalSourceParity LANGUAGES C CXX)
include(CTest)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Salts 3.0 CONFIG REQUIRED)

# All application/platform implementations are imported from the real 3.0
# Salts package and its exact 0.1 SHARED SaltsCore dependency.
foreach(_target IN ITEMS CMeta ComponentABI PluginABI Plugin
    ComponentProviderABI Configurator ServiceHost CFlow TinyTest)
  if(NOT TARGET "Salts::${_target}")
    message(FATAL_ERROR "Missing installed Salts::${_target}")
  endif()
endforeach()
foreach(_old IN ITEMS Core Platform Concurrency Coroutine NativeIO CNet)
  if(TARGET "Salts::${_old}")
    message(FATAL_ERROR "Old Salts binary owner imported: ${_old}")
  endif()
endforeach()
foreach(_native IN ITEMS Core Platform Concurrency NativeIO)
  get_target_property(_kind "SaltsCore::${_native}" TYPE)
  if(NOT _kind STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "Native shared owner required: ${_native}")
  endif()
endforeach()
if(NOT DEFINED SALTS_FIXTURE_ROOT OR
   NOT EXISTS "${SALTS_FIXTURE_ROOT}/cflow/tests/cflow_stream_slice_test.c" OR
   NOT EXISTS "${SALTS_FIXTURE_ROOT}/component-runtime/tests/fixture_component_provider.c")
  message(FATAL_ERROR "Original test source root missing")
endif()
set(_source "${SALTS_FIXTURE_ROOT}")

# Exact original provider and ACE test sources (byte-for-byte from M6).
add_library(salts3_parity_provider SHARED ace_provider.c)
target_compile_features(salts3_parity_provider PRIVATE c_std_11)
target_link_libraries(salts3_parity_provider PRIVATE
  Salts::ComponentABI SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts3_parity_provider PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(salts3_parity_cmeta_c11 consumer.c)
target_include_directories(salts3_parity_cmeta_c11 PRIVATE
  "${_source}/component-runtime/tests")
target_compile_features(salts3_parity_cmeta_c11 PRIVATE c_std_11)
target_link_libraries(salts3_parity_cmeta_c11 PRIVATE
  Salts::Configurator salts3_parity_provider
  SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts3_parity_cmeta_c11 PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME sdk.salts3.parity.cmeta_ace_c11 COMMAND salts3_parity_cmeta_c11)

add_executable(salts3_parity_cmeta_cpp17 consumer.cpp)
target_compile_features(salts3_parity_cmeta_cpp17 PRIVATE cxx_std_17)
target_link_libraries(salts3_parity_cmeta_cpp17 PRIVATE
  Salts::Configurator salts3_parity_provider
  SaltsCore::Core SaltsCore::Platform)
add_test(NAME sdk.salts3.parity.cmeta_ace_cpp17
  COMMAND salts3_parity_cmeta_cpp17)

# Exact original plugin Provider DSOs and original ServiceHost source suites.
add_library(salts3_parity_fixture_primary SHARED
  "${_source}/component-runtime/tests/fixture_component_provider.c")
add_library(salts3_parity_fixture_secondary SHARED
  "${_source}/component-runtime/tests/fixture_component_provider_secondary.c")
foreach(_fixture IN ITEMS salts3_parity_fixture_primary
                          salts3_parity_fixture_secondary)
  target_compile_features(${_fixture} PRIVATE c_std_11)
  target_link_libraries(${_fixture} PRIVATE
    Salts::ComponentProviderABI Salts::PluginABI)
  set_target_properties(${_fixture} PROPERTIES
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()

add_executable(salts3_parity_host_generation
  "${_source}/component-runtime/tests/service_host_generation_test.c")
target_include_directories(salts3_parity_host_generation PRIVATE
  "${_source}/component-runtime/tests")
target_compile_features(salts3_parity_host_generation PRIVATE c_std_11)
target_compile_definitions(salts3_parity_host_generation PRIVATE
  "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts3_parity_fixture_primary>\""
  "COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH=\"$<TARGET_FILE:salts3_parity_fixture_secondary>\"")
target_link_libraries(salts3_parity_host_generation PRIVATE
  Salts::ServiceHost Salts::TinyTest)
add_dependencies(salts3_parity_host_generation
  salts3_parity_fixture_primary salts3_parity_fixture_secondary)
add_test(NAME sdk.salts3.parity.service_host_generation
  COMMAND salts3_parity_host_generation)

add_executable(salts3_parity_host_concurrency
  "${_source}/component-runtime/tests/service_host_concurrency_test.c")
target_include_directories(salts3_parity_host_concurrency PRIVATE
  "${_source}/component-runtime/tests")
target_compile_features(salts3_parity_host_concurrency PRIVATE c_std_11)
target_compile_definitions(salts3_parity_host_concurrency PRIVATE
  "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts3_parity_fixture_primary>\"")
target_link_libraries(salts3_parity_host_concurrency PRIVATE
  Salts::ServiceHost Salts::TinyTest SaltsCore::Platform)
add_dependencies(salts3_parity_host_concurrency salts3_parity_fixture_primary)
add_test(NAME sdk.salts3.parity.service_host_concurrency
  COMMAND salts3_parity_host_concurrency)

add_executable(salts3_parity_host_cpp17
  "${_source}/component-runtime/tests/service_host_scope_cpp_test.cpp")
target_compile_features(salts3_parity_host_cpp17 PRIVATE cxx_std_17)
target_link_libraries(salts3_parity_host_cpp17 PRIVATE Salts::ServiceHost)
add_test(NAME sdk.salts3.parity.service_host_cpp17
  COMMAND salts3_parity_host_cpp17)

add_executable(salts3_parity_plugin_utf8 utf8_plugin_path_test.c)
target_compile_features(salts3_parity_plugin_utf8 PRIVATE c_std_11)
target_link_libraries(salts3_parity_plugin_utf8 PRIVATE
  Salts::Plugin SaltsCore::Core)
add_test(NAME sdk.salts3.parity.plugin_utf8
  COMMAND salts3_parity_plugin_utf8)

# Original seven CFlow tests: reactive processing, concurrent Event Mailbox,
# real NativeIO cancellation/ack, Sharded routing, and Machine transactions.
foreach(_suite IN ITEMS
    status stream_slice ace_pipes_filters
    event_mailbox io_native_adapter io_native_sharded_adapter machine_instance)
  add_executable(salts3_parity_cflow_${_suite}
    "${_source}/cflow/tests/cflow_${_suite}_test.c")
  target_include_directories(salts3_parity_cflow_${_suite} PRIVATE
    "${_source}/cflow/src")
  target_compile_features(salts3_parity_cflow_${_suite} PRIVATE c_std_11)
  target_link_libraries(salts3_parity_cflow_${_suite} PRIVATE
    Salts::CFlow Salts::TinyTest
    SaltsCore::NativeIO SaltsCore::Concurrency SaltsCore::Platform)
  set_target_properties(salts3_parity_cflow_${_suite} PROPERTIES
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(WIN32)
    target_link_libraries(salts3_parity_cflow_${_suite} PRIVATE ws2_32)
  endif()
  if(MSVC)
    target_compile_options(salts3_parity_cflow_${_suite} PRIVATE
      $<$<COMPILE_LANGUAGE:C>:-experimental:c11atomics>)
  endif()
  add_test(NAME sdk.salts3.parity.cflow_${_suite}
    COMMAND salts3_parity_cflow_${_suite})
endforeach()
