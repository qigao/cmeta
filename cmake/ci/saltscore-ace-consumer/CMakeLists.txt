cmake_minimum_required(VERSION 3.20)
project(SaltsM6IndependentInstalledSDK LANGUAGES C CXX)
include(CTest)
find_package(SaltsM6 CONFIG REQUIRED)
foreach(_target IN ITEMS CMeta ComponentABI PluginABI Plugin
                         ComponentProviderABI Configurator ServiceHost CFlow)
  if(NOT TARGET "Salts::${_target}")
    message(FATAL_ERROR "Missing original installed Salts::${_target}")
  endif()
endforeach()
foreach(_legacy IN ITEMS Core Platform Concurrency Coroutine NativeIO CNet)
  if(TARGET "Salts::${_legacy}")
    message(FATAL_ERROR "Unexpected legacy Salts owner Salts::${_legacy}")
  endif()
endforeach()
if(NOT TARGET SaltsCore::Core OR NOT TARGET SaltsCore::Platform)
  message(FATAL_ERROR "Missing real imported SaltsCore shared owners")
endif()
# Resolve the installed provider path from SaltsM6_DIR, not source/build paths.
get_filename_component(_m6_installed_prefix "${SaltsM6_DIR}/../../.." ABSOLUTE)
if(WIN32)
  set(_m6_provider "${_m6_installed_prefix}/bin/salts_m6_host_primary.dll")
else()
  set(_m6_provider "${_m6_installed_prefix}/lib/libsalts_m6_host_primary${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
if(NOT EXISTS "${_m6_provider}")
  message(FATAL_ERROR "Missing truly installed M6 Provider DSO: ${_m6_provider}")
endif()
file(TO_CMAKE_PATH "${_m6_provider}" _m6_provider)

add_executable(m6_installed_c consumer.c)
target_compile_definitions(m6_installed_c PRIVATE
  "SALTS_M6_INSTALLED_PROVIDER=\"${_m6_provider}\"")
target_compile_features(m6_installed_c PRIVATE c_std_11)
target_link_libraries(m6_installed_c PRIVATE
  Salts::ServiceHost Salts::Plugin SaltsCore::Core SaltsCore::Platform)
add_test(NAME sdk.m6.service_host_c11 COMMAND m6_installed_c)
add_executable(m6_installed_cpp consumer.cpp)
target_compile_features(m6_installed_cpp PRIVATE cxx_std_17)
target_link_libraries(m6_installed_cpp PRIVATE
  Salts::ServiceHost Salts::Plugin SaltsCore::Core SaltsCore::Platform)
add_test(NAME sdk.m6.service_host_cpp17 COMMAND m6_installed_cpp)

# A separate installed CFlow native consumer must import the same
# NativeIO/Platform/Concurrency owner as the real ServiceHost.
foreach(_foundation IN ITEMS NativeIO Concurrency)
  if(NOT TARGET "SaltsCore::${_foundation}")
    message(FATAL_ERROR "Installed SaltsM6 CFlow lacks ${_foundation}")
  endif()
  get_target_property(_foundation_type "SaltsCore::${_foundation}" TYPE)
  if(NOT _foundation_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "CFlow cannot embed a static ${_foundation} owner")
  endif()
endforeach()

add_executable(m6_installed_cflow_c cflow_consumer.c)
target_compile_features(m6_installed_cflow_c PRIVATE c_std_11)
target_link_libraries(m6_installed_cflow_c PRIVATE
  Salts::CFlow SaltsCore::Core SaltsCore::NativeIO SaltsCore::Platform)
add_test(NAME sdk.m6.cflow_c11 COMMAND m6_installed_cflow_c)

add_executable(m6_installed_cflow_cpp cflow_consumer.cpp)
target_compile_features(m6_installed_cflow_cpp PRIVATE cxx_std_17)
target_link_libraries(m6_installed_cflow_cpp PRIVATE
  Salts::CFlow SaltsCore::Core SaltsCore::NativeIO SaltsCore::Platform)
add_test(NAME sdk.m6.cflow_cpp17 COMMAND m6_installed_cflow_cpp)
