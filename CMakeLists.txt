cmake_minimum_required(VERSION 3.20)
project(CMeta VERSION 0.1.3 LANGUAGES C CXX)
# The current split-package release is tagged v0.1.003. CMake and NuGet use
# 0.1.3; the already-published v0.1.000 through v0.1.002 identities are immutable.
set(CMETA_NATIVE_SOVERSION "0.1")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
# Keep Windows DLL identities stable across MSVC-ABI Clang and llvm-mingw.
# MinGW's Unix-style `lib` prefix is not part of the published Windows ABI.
if(WIN32)
  set(CMAKE_SHARED_LIBRARY_PREFIX "")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
# Only one package graph. Legacy CMake options must never select a second
# native source owner, even if passed explicitly by a downstream build.
if(BUILD_TESTS OR BUILD_EXAMPLES OR BUILD_BENCHMARKS OR
   CFLOW_BUILD_BENCHMARKS OR CMETA_BUILD_NATIVE_THUNKS OR
   CFLOW_ENABLE_MINICORO OR BUILD_SHARED_LIBS)
  message(FATAL_ERROR "The CMeta 0.1 package has no legacy native source modes")
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

# Only reflected CMeta, ACE/Component, Configurator/ServiceHost and CFlow.
# All native CNet, FS, Process, Unicode, IDNA, CSerde, ASN1 and Crypto
# implementations must belong to their appropriate Core/Crypto repositories.
add_subdirectory(cmeta)
add_subdirectory(plugin)
add_subdirectory(ace)
add_subdirectory(cflow)
add_subdirectory(cstl)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
foreach(_retained IN ITEMS CMeta ConfiguratorABI PluginABI Plugin
    ServiceHostProviderABI Configurator ServiceHost CFlow CSTL CSTLStream)
  if(NOT TARGET "CMeta::${_retained}")
    message(FATAL_ERROR "Missing repository target CMeta::${_retained}")
  endif()
endforeach()
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/CMetaConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/CMetaConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CMeta")
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/CMetaConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
  "${CMAKE_BINARY_DIR}/CMetaConfig.cmake"
  "${CMAKE_BINARY_DIR}/CMetaConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CMeta")
export(EXPORT CMetaTargets
  FILE "${CMAKE_BINARY_DIR}/CMetaTargets.cmake" NAMESPACE CMeta::)
install(EXPORT CMetaTargets
  FILE CMetaTargets.cmake NAMESPACE CMeta::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CMeta")
