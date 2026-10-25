cmake_minimum_required(VERSION 3.20)
project(Salts3RealInstalledSourceConformance LANGUAGES C CXX)
include(CTest)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Only the ORIGINAL test sources are compiled, never production Salts objects.
# The entire production graph comes from real installed SaltsConfig 3.0.
find_package(Salts 3.0 CONFIG REQUIRED)
if(NOT Salts_VERSION VERSION_EQUAL "3.0.0")
  message(FATAL_ERROR "Original conformance requires exact Salts 3.0")
endif()
if(NOT DEFINED SALTS_SOURCE_ROOT OR
   NOT IS_ABSOLUTE "${SALTS_SOURCE_ROOT}" OR
   NOT EXISTS "${SALTS_SOURCE_ROOT}/cflow/tests/cflow_machine_instance_test.c")
  message(FATAL_ERROR "Exact original Salts test source tree is required")
endif()

foreach(_retained IN ITEMS CMeta ComponentABI PluginABI Plugin
    ComponentProviderABI Configurator ServiceHost CFlow)
  if(NOT TARGET "Salts::${_retained}")
    message(FATAL_ERROR "Missing installed Salts3 target: ${_retained}")
  endif()
endforeach()
if(TARGET Salts::TinyTest)
  message(FATAL_ERROR "Installed 3.0 package must not export test-only TinyTest")
endif()
foreach(_legacy IN ITEMS Core Platform Concurrency Coroutine NativeIO CNet)
  if(TARGET "Salts::${_legacy}")
    message(FATAL_ERROR "Original conformance refuses old Runtime: ${_legacy}")
  endif()
endforeach()
foreach(_foundation IN ITEMS Core Platform Concurrency Coroutine NativeIO)
  if(NOT TARGET "SaltsCore::${_foundation}")
    message(FATAL_ERROR "Missing native SaltsCore::${_foundation}")
  endif()
  get_target_property(_kind "SaltsCore::${_foundation}" TYPE)
  get_target_property(_imported "SaltsCore::${_foundation}" IMPORTED)
  if(NOT _kind STREQUAL "SHARED_LIBRARY" OR NOT _imported)
    message(FATAL_ERROR "Must import a single installed SHARED ${_foundation}")
  endif()
endforeach()

# Only the independent conformance suite compiles TinyTest. It is deliberately
# absent from the real production Salts 3.0 export set and ABI closure.
add_library(salts3_original_tinytest STATIC
  "${SALTS_SOURCE_ROOT}/tinytest/src/tinytest.c")
target_compile_features(salts3_original_tinytest PRIVATE c_std_11)
target_include_directories(salts3_original_tinytest PUBLIC
  "${SALTS_SOURCE_ROOT}/tinytest/include")
if(MSVC)
  target_compile_options(salts3_original_tinytest PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-experimental:c11atomics>)
endif()

function(salts3_original_case name source)
  add_executable(salts3_original_${name} "${source}")
  get_filename_component(_extension "${source}" EXT)
  if(_extension STREQUAL ".cpp")
    target_compile_features(salts3_original_${name} PRIVATE cxx_std_17)
  else()
    target_compile_features(salts3_original_${name} PRIVATE c_std_11)
    if(MSVC)
      target_compile_options(salts3_original_${name} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-experimental:c11atomics>)
    endif()
  endif()
  target_include_directories(salts3_original_${name} PRIVATE
    "${SALTS_SOURCE_ROOT}/component-runtime/tests"
    "${SALTS_SOURCE_ROOT}/cflow/src")
  target_link_libraries(salts3_original_${name} PRIVATE
    salts3_original_tinytest Salts::CMeta Salts::CFlow Salts::ServiceHost
    Salts::Plugin SaltsCore::Core SaltsCore::Platform
    SaltsCore::NativeIO SaltsCore::Concurrency)
  if(WIN32)
    target_link_libraries(salts3_original_${name} PRIVATE ws2_32)
  endif()
  add_test(NAME salts3.original.${name}
    COMMAND salts3_original_${name})
  set_tests_properties(salts3.original.${name} PROPERTIES TIMEOUT 120)
endfunction()

# Original provider fixture DSOs; only the installed CMeta/Component ABI
# is linked. Actual lease, quiescent stop and module unloading execute below.
add_library(salts3_reference_primary SHARED
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/fixture_component_provider.c")
add_library(salts3_reference_secondary SHARED
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/fixture_component_provider_secondary.c")
foreach(_fixture IN ITEMS salts3_reference_primary salts3_reference_secondary)
  target_compile_features("${_fixture}" PRIVATE c_std_11)
  set_target_properties("${_fixture}" PROPERTIES
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries("${_fixture}" PRIVATE
    Salts::ComponentProviderABI Salts::PluginABI)
endforeach()

salts3_original_case(configurator_c11
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/configurator_test.c")
salts3_original_case(configurator_cpp17
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/configurator_cpp_test.cpp")
salts3_original_case(service_host_generation
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/service_host_generation_test.c")
salts3_original_case(service_host_concurrency
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/service_host_concurrency_test.c")
salts3_original_case(service_host_cpp17
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/service_host_scope_cpp_test.cpp")

target_compile_definitions(salts3_original_service_host_generation PRIVATE
  "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts3_reference_primary>\""
  "COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH=\"$<TARGET_FILE:salts3_reference_secondary>\"")
target_compile_definitions(salts3_original_service_host_concurrency PRIVATE
  "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts3_reference_primary>\"")
add_dependencies(salts3_original_service_host_generation
  salts3_reference_primary salts3_reference_secondary)
add_dependencies(salts3_original_service_host_concurrency
  salts3_reference_primary)

salts3_original_case(plugin_utf8_path
  "${CMAKE_CURRENT_SOURCE_DIR}/utf8_plugin_path_test.c")
foreach(_flow IN ITEMS
    status stream_slice ace_pipes_filters event_mailbox
    io_native_adapter io_native_sharded_adapter machine_instance)
  salts3_original_case("cflow_${_flow}"
    "${SALTS_SOURCE_ROOT}/cflow/tests/cflow_${_flow}_test.c")
endforeach()
