cmake_minimum_required(VERSION 3.20)
project(SaltsCoreSaltsM6Downstream VERSION 2.3.0 LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)
if(NOT DEFINED SALTS_SOURCE_ROOT OR
   NOT EXISTS "${SALTS_SOURCE_ROOT}/cmeta/CMakeLists.txt" OR
   NOT EXISTS "${SALTS_SOURCE_ROOT}/component-runtime/src/configurator.c")
  message(FATAL_ERROR "An original qigao/salts source tree is mandatory")
endif()
if(NOT IS_ABSOLUTE "${SALTS_SOURCE_ROOT}")
  message(FATAL_ERROR "SALTS_SOURCE_ROOT must be absolute")
endif()

set(BUILD_TESTS OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_BENCHMARKS OFF)
set(CMETA_BUILD_NATIVE_THUNKS OFF)
set(SALTS_NATIVE_SOVERSION "2.3")
include("${SALTS_SOURCE_ROOT}/cmake/CmakeUtils.cmake")
find_package(SaltsCore 0.1 CONFIG REQUIRED)
foreach(_owner IN ITEMS Core Platform)
  if(NOT TARGET "SaltsCore::${_owner}")
    message(FATAL_ERROR "Missing real SaltsCore::${_owner}")
  endif()
  get_target_property(_type "SaltsCore::${_owner}" TYPE)
  if(NOT _type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "SaltsCore::${_owner} must be a shared binary owner")
  endif()
endforeach()

# Exact production CMeta and ComponentABI targets; not compatibility aliases.
add_subdirectory("${SALTS_SOURCE_ROOT}/cmeta" "${PROJECT_BINARY_DIR}/cmeta")
add_subdirectory("${SALTS_SOURCE_ROOT}/component" "${PROJECT_BINARY_DIR}/component")

# Original production Configurator C11 implementation. ServiceHost/Plugin
# Registry remain a later coordinated cutover.
add_library(salts_m6_configurator STATIC
  "${SALTS_SOURCE_ROOT}/component-runtime/src/configurator.c"
  "${SALTS_SOURCE_ROOT}/component-runtime/src/configurator_abi.c")
target_include_directories(salts_m6_configurator PUBLIC
  "${SALTS_SOURCE_ROOT}/component-runtime/include")
target_compile_features(salts_m6_configurator PUBLIC c_std_11)
target_link_libraries(salts_m6_configurator PUBLIC Salts::ComponentABI)

add_library(salts_m6_ace_provider SHARED ace_provider.c)
target_compile_features(salts_m6_ace_provider PRIVATE c_std_11)
target_link_libraries(salts_m6_ace_provider PRIVATE
  Salts::ComponentABI SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts_m6_ace_provider PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(salts_m6_consumer_c consumer.c)
target_compile_features(salts_m6_consumer_c PRIVATE c_std_11)
target_link_libraries(salts_m6_consumer_c PRIVATE
  salts_m6_configurator salts_m6_ace_provider
  SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts_m6_consumer_c PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(salts_m6_consumer_cpp consumer.cpp)
target_compile_features(salts_m6_consumer_cpp PRIVATE cxx_std_17)
target_link_libraries(salts_m6_consumer_cpp PRIVATE
  salts_m6_configurator salts_m6_ace_provider
  SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts_m6_consumer_cpp PROPERTIES
  CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

foreach(_legacy IN ITEMS Core Platform Concurrency Coroutine NativeIO CNet)
  if(TARGET "Salts::${_legacy}")
    message(FATAL_ERROR "Forbidden legacy runtime owner: Salts::${_legacy}")
  endif()
endforeach()
foreach(_consumer IN ITEMS salts_m6_consumer_c salts_m6_consumer_cpp
                           salts_m6_ace_provider)
  get_target_property(_direct ${_consumer} LINK_LIBRARIES)
  foreach(_item IN LISTS _direct)
    if(_item MATCHES "^(Salts::(Core|Platform|Concurrency|Coroutine|NativeIO|CNet)|cmeta_platform|cmeta_concurrency|cmeta_native_io|cnet)$")
      message(FATAL_ERROR "Legacy implementation unexpectedly linked: ${_item}")
    endif()
  endforeach()
endforeach()

enable_testing()
add_test(NAME salts.m6.cmeta_ace_c11 COMMAND salts_m6_consumer_c)
add_test(NAME salts.m6.cmeta_ace_cpp17 COMMAND salts_m6_consumer_cpp)
