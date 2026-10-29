cmake_minimum_required(VERSION 3.20)
project(Salts VERSION 3.0.0 LANGUAGES C CXX)
# This is the sole CMeta/ACE/CFlow package epoch. No old Salts 2.3,
# native-ownership profile or compatibility/fallback switch exists.
set(SALTS_NATIVE_SOVERSION "3.0")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
# Only one package graph. Legacy CMake options must never select a second
# native source owner, even if passed explicitly by a downstream build.
if(BUILD_TESTS OR BUILD_EXAMPLES OR BUILD_BENCHMARKS OR
   CFLOW_BUILD_BENCHMARKS OR CMETA_BUILD_NATIVE_THUNKS OR
   CFLOW_ENABLE_MINICORO OR BUILD_SHARED_LIBS)
  message(FATAL_ERROR "The Salts 3 package has no legacy native source modes")
endif()
include(CmakeUtils)
find_package(Threads REQUIRED)
find_package(SaltsCore 0.1 CONFIG REQUIRED)
foreach(_owner IN ITEMS Core Platform Concurrency Coroutine NativeIO)
  if(NOT TARGET "Salts::${_owner}")
    message(FATAL_ERROR "Missing installed Salts::${_owner}")
  endif()
  get_target_property(_kind "Salts::${_owner}" TYPE)
  get_target_property(_imported "Salts::${_owner}" IMPORTED)
  if(NOT _imported OR NOT _kind STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "Salts::${_owner} must be the imported SHARED Core owner")
  endif()
endforeach()

# Numeric execution and lane shapes belong to the installed static Core module.
if(NOT TARGET Salts::SIMD)
  message(FATAL_ERROR "Missing installed static Salts::SIMD from SaltsCore")
endif()
get_target_property(_simd_kind Salts::SIMD TYPE)
get_target_property(_simd_imported Salts::SIMD IMPORTED)
if(NOT _simd_imported OR NOT _simd_kind STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "Salts::SIMD must be the imported STATIC Core module")
endif()

# Only reflected CMeta, ACE/Component, Configurator/ServiceHost and CFlow.
# All native CNet, FS, Process, Unicode, IDNA, CSerde, ASN1 and Crypto
# implementations must belong to their appropriate Core/Crypto repositories.
add_subdirectory(cmeta)
add_subdirectory(component)
add_subdirectory(plugin)
add_subdirectory(component-plugin)
add_subdirectory(component-runtime)
add_subdirectory(cflow)
foreach(_retained IN ITEMS CMeta ComponentABI PluginABI Plugin
    ComponentProviderABI Configurator ServiceHost CFlow)
  if(NOT TARGET "Salts::${_retained}")
    message(FATAL_ERROR "Missing CMeta/ACE target Salts::${_retained}")
  endif()
endforeach()
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/SaltsConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/SaltsConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Salts")
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/SaltsConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
  "${CMAKE_BINARY_DIR}/SaltsConfig.cmake"
  "${CMAKE_BINARY_DIR}/SaltsConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Salts")
export(EXPORT SaltsTargets
  FILE "${CMAKE_BINARY_DIR}/SaltsTargets.cmake" NAMESPACE Salts::)
install(EXPORT SaltsTargets
  FILE SaltsTargets.cmake NAMESPACE Salts::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Salts")
