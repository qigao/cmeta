cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

project(
  Salts
  VERSION 2.3.0
  LANGUAGES C CXX)

# CI cache preflight computes the exact vcpkg ABI graph using this release
# preset, but must not configure production targets or build dependencies.
if(SALTS_CACHE_PREFLIGHT)
  if(NOT "--dry-run" IN_LIST VCPKG_INSTALL_OPTIONS)
    message(FATAL_ERROR "SALTS_CACHE_PREFLIGHT requires vcpkg --dry-run")
  endif()
  return()
endif()

# The 2.3 SDK includes an explicit native ABI transition from released 2.2.
# Distinct SONAME (2.3) prevents old 2.2 binaries loading incompatible 2.3
# Component/Plugin records merely because the SemVer major remains 2.
set(SALTS_NATIVE_SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "common library for C/C++")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/salts")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # Preserve the supported C11 atomics flag for MSVC builds.
  add_compile_options("$<$<COMPILE_LANGUAGE:C>:-experimental:c11atomics>")
endif()
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

include(CMakeOptions.cmake)
include(Sanitizers)
include(CmakeUtils)

if(BUILD_TESTS)
  find_program(SALTS_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
  if(SALTS_POWERSHELL_EXECUTABLE)
    add_test(
      NAME benchmark_policy_test
      COMMAND
        ${SALTS_POWERSHELL_EXECUTABLE} -NoProfile -File
        ${PROJECT_SOURCE_DIR}/cmake/ci/verify-benchmark-policy.ps1 -ProjectRoot
        ${PROJECT_SOURCE_DIR})
  endif()
endif()

find_package(Threads REQUIRED)

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(tinytest)
add_subdirectory(platform)
add_subdirectory(concurrency)
add_subdirectory(coroutine)
add_subdirectory(native-io)
add_subdirectory(uri)
add_subdirectory(cmeta)
add_subdirectory(component)
add_subdirectory(plugin)
add_subdirectory(component-plugin)
# One installed Configurator/ServiceHost runtime with native ABI epoch 1.
# Component/ComponentPlugin legacy implementations are not built or exported.
add_subdirectory(component-runtime)
add_subdirectory(simd)
add_subdirectory(tinymock)
add_subdirectory(cflow)
add_subdirectory(cstl)
add_subdirectory(cnet)
add_subdirectory(cserde)
add_subdirectory(asn1)
add_subdirectory(utils)
# Unicode depends only on Salts::Core; this migration does not change CNet DNS.
add_subdirectory(unicode)
add_subdirectory(idna)

if(WIN32)
  enable_language(RC)
  include(SaltsVersion)
endif()

# Package Configuration
include(CMakePackageConfigHelpers)

# Generate the config file
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/SaltsConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/SaltsConfig.cmake" INSTALL_DESTINATION "lib/cmake/Salts")

# Generate the version file
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/SaltsConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)

# Install the config files
install(FILES "${CMAKE_BINARY_DIR}/SaltsConfig.cmake"
              "${CMAKE_BINARY_DIR}/SaltsConfigVersion.cmake"
        DESTINATION "lib/cmake/Salts")

# Install the export set
export(
  EXPORT SaltsTargets
  FILE "${CMAKE_BINARY_DIR}/SaltsTargets.cmake"
  NAMESPACE Salts::)

install(
  EXPORT SaltsTargets
  FILE SaltsTargets.cmake
  NAMESPACE Salts::
  DESTINATION "lib/cmake/Salts")
