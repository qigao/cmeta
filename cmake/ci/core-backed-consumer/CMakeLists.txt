cmake_minimum_required(VERSION 3.20)
project(SaltsCoreBackedIndependentSDK LANGUAGES C CXX)
include(CTest)

# Real SaltsConfig/SaltsTargets, not the separate SaltsM6 test-only package.
# Salts 2.3 must never be admitted as the 3.0 ABI implementation.
find_package(Salts 3.0 CONFIG REQUIRED)
if(NOT Salts_VERSION VERSION_EQUAL "3.0.0")
  message(FATAL_ERROR "Wrong Salts ABI epoch: ${Salts_VERSION}")
endif()
foreach(_target IN ITEMS CMeta ComponentABI PluginABI Plugin
                          ComponentProviderABI Configurator ServiceHost CFlow)
  if(NOT TARGET "Salts::${_target}")
    message(FATAL_ERROR "Real Salts 3.0 target missing: ${_target}")
  endif()
endforeach()
foreach(_legacy IN ITEMS Core Platform Concurrency Coroutine NativeIO CNet)
  if(TARGET "Salts::${_legacy}")
    message(FATAL_ERROR "Old binary owner present in Salts 3.0: ${_legacy}")
  endif()
endforeach()
foreach(_native IN ITEMS Core Platform Concurrency Coroutine NativeIO)
  if(NOT TARGET "SaltsCore::${_native}")
    message(FATAL_ERROR "Missing separately installed Core native owner ${_native}")
  endif()
  get_target_property(_owner_type "SaltsCore::${_native}" TYPE)
  if(NOT _owner_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "Standalone ${_native} must be SHARED")
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
  Salts::ServiceHost Salts::Plugin SaltsCore::Core SaltsCore::Platform)
add_dependencies(salts3_consumer_c11 salts3_test_provider)
add_test(NAME sdk.salts3.cmeta_ace_c11 COMMAND salts3_consumer_c11)

add_executable(salts3_consumer_cpp17 consumer.cpp)
target_compile_features(salts3_consumer_cpp17 PRIVATE cxx_std_17)
target_link_libraries(salts3_consumer_cpp17 PRIVATE
  Salts::ServiceHost Salts::Plugin SaltsCore::Core SaltsCore::Platform)
add_test(NAME sdk.salts3.cmeta_ace_cpp17 COMMAND salts3_consumer_cpp17)

add_executable(salts3_cflow_c11 cflow_consumer.c)
target_compile_features(salts3_cflow_c11 PRIVATE c_std_11)
target_link_libraries(salts3_cflow_c11 PRIVATE
  Salts::CFlow SaltsCore::Core SaltsCore::NativeIO SaltsCore::Platform)
add_test(NAME sdk.salts3.cflow_c11 COMMAND salts3_cflow_c11)

add_executable(salts3_cflow_cpp17 cflow_consumer.cpp)
target_compile_features(salts3_cflow_cpp17 PRIVATE cxx_std_17)
target_link_libraries(salts3_cflow_cpp17 PRIVATE
  Salts::CFlow SaltsCore::Core SaltsCore::NativeIO SaltsCore::Platform)
add_test(NAME sdk.salts3.cflow_cpp17 COMMAND salts3_cflow_cpp17)
