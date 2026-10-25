# Unreleased Salts 3.0 Core-backed CMeta/ACE/CFlow profile.
# Included only by root CMakeLists.txt with SALTS_CORE_BACKED_PROFILE=ON.
# The historical 2.3 target graph is not configured, *not* used as fallback.
include_guard(GLOBAL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT SALTS_CORE_BACKED_PROFILE OR
   NOT PROJECT_NAME STREQUAL "Salts" OR
   NOT PROJECT_VERSION STREQUAL "3.0.0")
  message(FATAL_ERROR
    "Core-backed profile requires explicit unreleased Salts 3.0 opt-in")
endif()

# Source-tree tests in the old Salts repository can transitively import
# Salts::Core and duplicate state owners. Only independent installed tests
# are admitted in this phase.
if(BUILD_TESTS OR BUILD_EXAMPLES OR BUILD_BENCHMARKS OR
   CFLOW_BUILD_BENCHMARKS OR CMETA_BUILD_NATIVE_THUNKS OR
   CFLOW_ENABLE_MINICORO OR SALTS_ENABLE_EPOLL_READINESS OR
   BUILD_SHARED_LIBS)
  message(FATAL_ERROR
    "Unreleased Core-backed SDK requires BUILD_TESTS/BUILD_EXAMPLES/"
    "BUILD_BENCHMARKS/CFLOW_BUILD_BENCHMARKS/"
    "CMETA_BUILD_NATIVE_THUNKS/CFLOW_ENABLE_MINICORO/"
    "SALTS_ENABLE_EPOLL_READINESS/BUILD_SHARED_LIBS=OFF; "
    "no silent fallback to the legacy native graph")
endif()
if(DEFINED SALTS_PLUGIN_USE_SALTSCORE AND
   NOT SALTS_PLUGIN_USE_SALTSCORE)
  message(FATAL_ERROR "Cannot disable standalone Core in the ACE profile")
endif()
if(DEFINED SALTS_CFLOW_USE_SALTSCORE AND
   NOT SALTS_CFLOW_USE_SALTSCORE)
  message(FATAL_ERROR "Cannot disable standalone Core in the CFlow profile")
endif()
set(SALTS_PLUGIN_USE_SALTSCORE ON)
set(SALTS_CFLOW_USE_SALTSCORE ON)

find_package(SaltsCore 0.1 CONFIG REQUIRED)
foreach(_component IN ITEMS Core Platform Concurrency Coroutine NativeIO)
  if(NOT TARGET "SaltsCore::${_component}")
    message(FATAL_ERROR "Missing installed SaltsCore::${_component}")
  endif()
  get_target_property(_kind "SaltsCore::${_component}" TYPE)
  get_target_property(_imported "SaltsCore::${_component}" IMPORTED)
  if(NOT _kind STREQUAL "SHARED_LIBRARY" OR NOT _imported)
    message(FATAL_ERROR "SaltsCore::${_component} must be an imported "
      "shared installed native owner, not a static/archive fallback")
  endif()
endforeach()

# Real original production target sources and export set. Deliberately do
# NOT add platform/, concurrency/, coroutine/, native-io/, cnet/ or utils/.
# No old Salts::Core, Platform, NativeIO, Concurrency or CNet can appear.
add_subdirectory(tinytest)
add_subdirectory(cmeta)
add_subdirectory(component)
add_subdirectory(plugin)
add_subdirectory(component-plugin)
add_subdirectory(component-runtime)
add_subdirectory(cflow)

foreach(_legacy IN ITEMS Core Platform Concurrency Coroutine NativeIO CNet)
  if(TARGET "Salts::${_legacy}")
    message(FATAL_ERROR "Core-backed SDK accidentally defined "
      "old Salts::${_legacy}")
  endif()
endforeach()
foreach(_retained IN ITEMS CMeta ComponentABI PluginABI Plugin
    ComponentProviderABI Configurator ServiceHost CFlow)
  if(NOT TARGET "Salts::${_retained}")
    message(FATAL_ERROR "Required actual Salts 3.0 target missing: ${_retained}")
  endif()
endforeach()
foreach(_implementation IN ITEMS cmeta_plugin salts_service_host cmeta_cflow)
  get_target_property(_direct "${_implementation}" LINK_LIBRARIES)
  foreach(_linked IN LISTS _direct)
    if(_linked MATCHES "^Salts::(Core|Platform|Concurrency|Coroutine|NativeIO|CNet)$")
      message(FATAL_ERROR "Legacy native owner in ${_implementation}: ${_linked}")
    endif()
  endforeach()
endforeach()

# A real SaltsConfig/SaltsTargets candidate rather than a second M6 shim.
# The independent ABI epoch (3.0) explicitly prevents Salts 2.3 consumers
# from interpreting this subset as a backward-compatible 2.3 SDK.
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/SaltsConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/SaltsConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Salts")
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/SaltsConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion)
install(FILES
  "${CMAKE_BINARY_DIR}/SaltsConfig.cmake"
  "${CMAKE_BINARY_DIR}/SaltsConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Salts")
export(EXPORT SaltsTargets
  FILE "${CMAKE_BINARY_DIR}/SaltsTargets.cmake"
  NAMESPACE Salts::)
install(EXPORT SaltsTargets
  FILE SaltsTargets.cmake NAMESPACE Salts::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Salts")

message(STATUS
  "SALTS_CORE_BACKED_PROFILE: unreleased Salts 3.0 ACE/CFlow candidate; "
  "native state exclusively from installed SaltsCore 0.1")
