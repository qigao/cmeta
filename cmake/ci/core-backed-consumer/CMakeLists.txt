cmake_minimum_required(VERSION 3.20)
project(SaltsCoreBackedIndependentSDK LANGUAGES C CXX)
include(CTest)

# Real SaltsConfig/SaltsTargets, not the separate SaltsM6 test-only package.
# Salts 2.3 must never be admitted as the 3.0 ABI implementation.
find_package(Salts 3.0 CONFIG REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/../core-backed-consumer/check-package-exports.cmake")
if(NOT Salts_VERSION VERSION_EQUAL "3.0.0")
  message(FATAL_ERROR "Wrong Salts ABI epoch: ${Salts_VERSION}")
endif()
foreach(_target IN ITEMS CMeta ComponentABI PluginABI Plugin
                          ComponentProviderABI Configurator ServiceHost CFlow)
  if(NOT TARGET "Salts::${_target}")
    message(FATAL_ERROR "Real Salts 3.0 target missing: ${_target}")
  endif()
endforeach()
# ASN.1 belongs exclusively to the one installed static Salts::Crypto,
# not Salts or SaltsCore. Never reintroduce a split native target.
if(TARGET Salts::ASN1)
  message(FATAL_ERROR "Retired Salts::ASN1 exported by Core or Salts")
endif()
foreach(_native IN ITEMS Core Platform Concurrency Coroutine NativeIO)
  if(NOT TARGET "Salts::${_native}")
    message(FATAL_ERROR "Missing Core Salts::${_native}")
  endif()
  get_target_property(_kind "Salts::${_native}" TYPE)
  get_target_property(_imported "Salts::${_native}" IMPORTED)
  if(NOT _imported OR NOT _kind STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "Core Salts::${_native} must be imported SHARED")
  endif()
endforeach()

# Only the original test provider's source is compiled here. No original
# Salts state-machine/source/object file enters these independent consumers.
if(NOT DEFINED SALTS_FIXTURE_ROOT OR
   NOT EXISTS "${SALTS_FIXTURE_ROOT}/component-runtime/tests/fixture_component_provider.c")
  message(FATAL_ERROR "Need the original test fixture source, not legacy runtime")
endif()
add_library(salts3_test_provider SHARED
  "${SALTS_FIXTURE_ROOT}/component-runtime/tests/fixture_component_provider.c")
target_compile_features(salts3_test_provider PRIVATE c_std_11)
target_link_libraries(salts3_test_provider PRIVATE
  Salts::ComponentProviderABI Salts::PluginABI)
set_target_properties(salts3_test_provider PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(salts3_consumer_c11 consumer.c)
target_compile_features(salts3_consumer_c11 PRIVATE c_std_11)
target_compile_definitions(salts3_consumer_c11 PRIVATE
  "SALTS_M6_INSTALLED_PROVIDER=\"$<TARGET_FILE:salts3_test_provider>\"")
target_link_libraries(salts3_consumer_c11 PRIVATE
  Salts::ServiceHost Salts::Plugin Salts::Core Salts::Platform)
add_dependencies(salts3_consumer_c11 salts3_test_provider)
add_test(NAME sdk.salts3.cmeta_ace_c11 COMMAND salts3_consumer_c11)

add_executable(salts3_consumer_cpp17 consumer.cpp)
target_compile_features(salts3_consumer_cpp17 PRIVATE cxx_std_17)
target_link_libraries(salts3_consumer_cpp17 PRIVATE
  Salts::ServiceHost Salts::Plugin Salts::Core Salts::Platform)
add_test(NAME sdk.salts3.cmeta_ace_cpp17 COMMAND salts3_consumer_cpp17)

add_executable(salts3_cflow_c11 cflow_consumer.c)
target_compile_features(salts3_cflow_c11 PRIVATE c_std_11)
target_link_libraries(salts3_cflow_c11 PRIVATE
  Salts::CFlow Salts::Core Salts::NativeIO Salts::Platform)
add_test(NAME sdk.salts3.cflow_c11 COMMAND salts3_cflow_c11)

add_executable(salts3_cflow_cpp17 cflow_consumer.cpp)
target_compile_features(salts3_cflow_cpp17 PRIVATE cxx_std_17)
target_link_libraries(salts3_cflow_cpp17 PRIVATE
  Salts::CFlow Salts::Core Salts::NativeIO Salts::Platform)
add_test(NAME sdk.salts3.cflow_cpp17 COMMAND salts3_cflow_cpp17)
