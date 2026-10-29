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
# The native Core SDK owns its first-party ASN1 codec. SaltsTargets above must
# still export only CMeta/ACE/CFlow; it may never provide another native owner.
if(NOT TARGET Salts::ASN1)
  message(FATAL_ERROR "Core SDK is missing the native Salts::ASN1 owner")
endif()
get_target_property(_asn1_kind Salts::ASN1 TYPE)
get_target_property(_asn1_imported Salts::ASN1 IMPORTED)
if(NOT _asn1_imported OR NOT _asn1_kind STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "Salts::ASN1 must be the imported static Core codec")
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

get_target_property(_simd_kind Salts::SIMD TYPE)
get_target_property(_simd_imported Salts::SIMD IMPORTED)
if(NOT _simd_imported OR NOT _simd_kind STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "Reflection consumers require the installed static SIMD module")
endif()
foreach(_language IN ITEMS c11 cpp17)
  if(_language STREQUAL "c11")
    set(_source simd_reflection.c)
    set(_standard c_std_11)
  else()
    set(_source simd_reflection.cpp)
    set(_standard cxx_std_17)
  endif()
  add_executable(salts3_simd_reflection_${_language} ${_source})
  target_compile_features(salts3_simd_reflection_${_language} PRIVATE ${_standard})
  # CMeta must propagate its one native descriptor/execution dependency.
  target_link_libraries(salts3_simd_reflection_${_language} PRIVATE Salts::CMeta)
  if(MSVC)
    target_compile_options(salts3_simd_reflection_${_language} PRIVATE /W4 /WX)
  else()
    target_compile_options(salts3_simd_reflection_${_language} PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME sdk.salts3.simd_reflection_${_language}
    COMMAND salts3_simd_reflection_${_language})
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
