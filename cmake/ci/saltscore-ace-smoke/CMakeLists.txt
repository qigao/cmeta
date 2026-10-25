cmake_minimum_required(VERSION 3.20)
project(SaltsCoreSaltsM6Downstream VERSION 2.3.0 LANGUAGES C CXX)
# Match the real Salts project contract: static CMeta/Plugin descriptors are
# embedded into multiple native Provider DSOs, including under ASan/UBSan.
# Never rely on non-PIC archives and never disable instrumented tests.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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
set(SALTS_PLUGIN_USE_SALTSCORE ON)
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

# Exact original production CMake targets (not copies or shims).
add_subdirectory("${SALTS_SOURCE_ROOT}/tinytest" "${PROJECT_BINARY_DIR}/tinytest")
add_subdirectory("${SALTS_SOURCE_ROOT}/cmeta" "${PROJECT_BINARY_DIR}/cmeta")
add_subdirectory("${SALTS_SOURCE_ROOT}/component" "${PROJECT_BINARY_DIR}/component")
add_subdirectory("${SALTS_SOURCE_ROOT}/plugin" "${PROJECT_BINARY_DIR}/plugin")
add_subdirectory("${SALTS_SOURCE_ROOT}/component-plugin" "${PROJECT_BINARY_DIR}/component-plugin")
add_subdirectory("${SALTS_SOURCE_ROOT}/component-runtime" "${PROJECT_BINARY_DIR}/component-runtime")

add_library(salts_m6_ace_provider SHARED ace_provider.c)
target_compile_features(salts_m6_ace_provider PRIVATE c_std_11)
target_link_libraries(salts_m6_ace_provider PRIVATE
  Salts::ComponentABI SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts_m6_ace_provider PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(salts_m6_consumer_c consumer.c)
# Reuse Salts' actual canonical two-provider graph fixture. No synthetic
# Provider description: logger -> configured application dependency.
target_include_directories(salts_m6_consumer_c PRIVATE
  "${SALTS_SOURCE_ROOT}/component-runtime/tests")
target_compile_features(salts_m6_consumer_c PRIVATE c_std_11)
target_link_libraries(salts_m6_consumer_c PRIVATE
  Salts::Configurator salts_m6_ace_provider
  SaltsCore::Core SaltsCore::Platform)
set_target_properties(salts_m6_consumer_c PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(salts_m6_consumer_cpp consumer.cpp)
target_compile_features(salts_m6_consumer_cpp PRIVATE cxx_std_17)
target_link_libraries(salts_m6_consumer_cpp PRIVATE
  Salts::Configurator salts_m6_ace_provider
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

# Real original Provider DSO fixtures, loaded through PluginRegistry.
add_library(salts_m6_host_primary SHARED
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/fixture_component_provider.c")
add_library(salts_m6_host_secondary SHARED
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/fixture_component_provider_secondary.c")
foreach(_fixture IN ITEMS salts_m6_host_primary salts_m6_host_secondary)
  target_compile_features(${_fixture} PRIVATE c_std_11)
  target_link_libraries(${_fixture} PRIVATE
    Salts::ComponentProviderABI Salts::PluginABI)
  set_target_properties(${_fixture} PROPERTIES
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endforeach()

add_executable(salts_m6_service_host_generation
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/service_host_generation_test.c")
target_include_directories(salts_m6_service_host_generation PRIVATE
  "${SALTS_SOURCE_ROOT}/component-runtime/tests")
target_compile_features(salts_m6_service_host_generation PRIVATE c_std_11)
target_link_libraries(salts_m6_service_host_generation PRIVATE
  Salts::ServiceHost Salts::TinyTest)
set_target_properties(salts_m6_service_host_generation PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(salts_m6_service_host_generation PRIVATE
  "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_m6_host_primary>\""
  "COMPONENT_PROVIDER_SECONDARY_PLUGIN_PATH=\"$<TARGET_FILE:salts_m6_host_secondary>\"")
add_dependencies(salts_m6_service_host_generation
  salts_m6_host_primary salts_m6_host_secondary)
add_test(NAME salts.m6.service_host_generation COMMAND salts_m6_service_host_generation)
set_tests_properties(salts.m6.service_host_generation PROPERTIES TIMEOUT 65)

# The unchanged original ServiceHost concurrent publication/drain test:
# four live readers, 32 bounded publish/race rounds and DSO lease fencing.
add_executable(salts_m6_service_host_concurrency
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/service_host_concurrency_test.c")
target_compile_features(salts_m6_service_host_concurrency PRIVATE c_std_11)
target_include_directories(salts_m6_service_host_concurrency PRIVATE
  "${SALTS_SOURCE_ROOT}/component-runtime/tests")
target_link_libraries(salts_m6_service_host_concurrency PRIVATE
  Salts::ServiceHost Salts::TinyTest SaltsCore::Platform)
target_compile_definitions(salts_m6_service_host_concurrency PRIVATE
  "COMPONENT_PROVIDER_PLUGIN_PATH=\"$<TARGET_FILE:salts_m6_host_primary>\"")
add_dependencies(salts_m6_service_host_concurrency salts_m6_host_primary)
add_test(NAME salts.m6.service_host_concurrency
  COMMAND salts_m6_service_host_concurrency)
set_tests_properties(salts.m6.service_host_concurrency PROPERTIES TIMEOUT 100)

add_executable(salts_m6_service_host_cpp
  "${SALTS_SOURCE_ROOT}/component-runtime/tests/service_host_scope_cpp_test.cpp")
target_compile_features(salts_m6_service_host_cpp PRIVATE cxx_std_17)
target_link_libraries(salts_m6_service_host_cpp PRIVATE Salts::ServiceHost)
add_test(NAME salts.m6.service_host_cpp COMMAND salts_m6_service_host_cpp)

add_executable(salts_m6_plugin_path utf8_plugin_path_test.c)
target_compile_features(salts_m6_plugin_path PRIVATE c_std_11)
target_link_libraries(salts_m6_plugin_path PRIVATE Salts::Plugin SaltsCore::Core)
add_test(NAME salts.m6.plugin_utf8_path COMMAND salts_m6_plugin_path)

# Test-only installable extracted ACE package; never pretend this is the
# existing Salts 2.3 SDK or production SaltsConfig.cmake.
# Install actual dynamic Plugin Provider fixtures; the independent
# clean-prefix consumer must load the installed copies, not build-tree files.
install(TARGETS salts_m6_host_primary salts_m6_host_secondary
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CMakePackageConfigHelpers)
set(_m6_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/SaltsM6")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/SaltsM6Config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/SaltsM6Config.cmake"
  INSTALL_DESTINATION "${_m6_package_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SaltsM6Config.cmake"
  DESTINATION "${_m6_package_dir}")
install(EXPORT SaltsTargets
  FILE SaltsM6Targets.cmake
  NAMESPACE Salts::
  DESTINATION "${_m6_package_dir}")
