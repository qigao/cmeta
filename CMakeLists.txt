cmake_minimum_required(VERSION 3.27)
include(CMakeOptions.cmake)
project(CMeta VERSION 0.1.7 LANGUAGES C CXX)
set(CMETA_NATIVE_SOVERSION "0.1")
if(BUILD_SHARED_LIBS OR CMETA_BUILD_NATIVE_THUNKS OR CFLOW_ENABLE_MINICORO)
  message(FATAL_ERROR "The CMeta split SDK has one static metadata/runtime composition; legacy source modes are unsupported")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(WIN32)
  set(CMAKE_SHARED_LIBRARY_PREFIX "")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(Sanitizers)
include(CmakeUtils)
if(BUILD_TESTS OR BUILD_BENCHMARK)
  enable_testing()
endif()
set(CMETA_DEPENDENCY_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
list(PREPEND CMAKE_PREFIX_PATH
  "${CMETA_DEPENDENCY_PREFIX}/$ENV{VCPKG_CONFIGURATION_SUBDIR}"
  "${CMETA_DEPENDENCY_PREFIX}")
find_package(Threads REQUIRED)
if(NOT "$ENV{SALTS_BASE_VERSION}" MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "SALTS_BASE_VERSION must select an exact SDK version without the v prefix")
endif()
if("$ENV{SALTS_BASE_VERSION}" VERSION_LESS "0.1.4")
  message(FATAL_ERROR "CFlow requires SaltsBase 0.1.4 or newer with public threadpool_is_current")
endif()
unset(SaltsBase_DIR CACHE)
find_package(SaltsBase "$ENV{SALTS_BASE_VERSION}" EXACT CONFIG REQUIRED
  PATHS "$ENV{SALTS_BASE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(BUILD_TESTS)
  # Only formal CNet composition tests depend on the network SDK.
  unset(SaltsCNet_DIR CACHE)
  find_package(SaltsCNet "$ENV{SALTS_CORE_VERSION}" EXACT CONFIG REQUIRED
    PATHS "$ENV{SALTSCORE_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
add_subdirectory(cmeta)
add_subdirectory(tinymock)
add_subdirectory(plugin)
add_subdirectory(ace)
add_subdirectory(cflow)
add_subdirectory(cstl)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CMetaConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/CMetaConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CMeta")
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/CMetaConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_BINARY_DIR}/CMetaConfig.cmake"
  "${CMAKE_BINARY_DIR}/CMetaConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CMeta")
export(EXPORT CMetaTargets FILE "${CMAKE_BINARY_DIR}/CMetaTargets.cmake" NAMESPACE CMeta::)
install(EXPORT CMetaTargets FILE CMetaTargets.cmake NAMESPACE CMeta::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CMeta")
