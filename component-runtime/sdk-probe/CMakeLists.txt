cmake_minimum_required(VERSION 3.20)
project(SaltsComponentRuntimeInstalledABIProbe LANGUAGES C CXX)
enable_testing()
if(NOT DEFINED SALTS_INSTALLED_ROOT OR NOT IS_DIRECTORY "${SALTS_INSTALLED_ROOT}")
  message(FATAL_ERROR "Set SALTS_INSTALLED_ROOT to one installed native SDK")
endif()
# Android/iOS toolchains may re-root absolute find paths into their sysroot.
# Require the exact selected staged SDK package and do not substitute host or
# sysroot packages when resolving the explicitly provided native SDK prefix.
set(_salts_config "${SALTS_INSTALLED_ROOT}/lib/cmake/Salts/SaltsConfig.cmake")
if(NOT EXISTS "${_salts_config}")
  message(FATAL_ERROR "Selected installed Salts SDK lacks its Config: ${_salts_config}")
endif()
find_package(Salts CONFIG REQUIRED
  PATHS "${SALTS_INSTALLED_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
# Resolve the actual file identities rather than comparing spelling of
# Windows path separators or the case of the drive letter. On Unix the same
# realpath guard continues to enforce one exact SDK; no host/toolchain fallback.
file(REAL_PATH "${_salts_config}" _salts_expected_config_real)
file(REAL_PATH "${Salts_DIR}/SaltsConfig.cmake" _salts_imported_config_real)
if(WIN32)
  string(TOLOWER "${_salts_expected_config_real}" _salts_expected_config_real)
  string(TOLOWER "${_salts_imported_config_real}" _salts_imported_config_real)
endif()
if(NOT _salts_imported_config_real STREQUAL _salts_expected_config_real)
  message(FATAL_ERROR
    "Salts was not resolved from the exact selected installed SDK: ${Salts_DIR}")
endif()
foreach(name IN ITEMS Salts::Configurator Salts::ServiceHost Salts::ComponentABI
    Salts::ComponentPluginABI Salts::Plugin Salts::Platform)
  if(NOT TARGET ${name})
    message(FATAL_ERROR "Missing installed transitive ABI target: ${name}")
  endif()
endforeach()
if(TARGET Salts::ConfiguratorCandidate OR TARGET Salts::ServiceHostCandidate)
  message(FATAL_ERROR "Draft target names must not leak into exported SDK")
endif()
get_target_property(config_libraries Salts::Configurator INTERFACE_LINK_LIBRARIES)
if("${config_libraries}" MATCHES "Salts::Plugin|Salts::ServiceHost")
  message(FATAL_ERROR "Static Configurator unexpectedly owns Plugin/ServiceHost")
endif()
foreach(lang IN ITEMS c cpp)
  if(lang STREQUAL "c")
    add_executable(salts_abi_consumer_c consumer.c)
    set_target_properties(salts_abi_consumer_c PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    set(consumer salts_abi_consumer_c)
  else()
    add_executable(salts_abi_consumer_cpp consumer.cpp)
    set_target_properties(salts_abi_consumer_cpp PROPERTIES
      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    set(consumer salts_abi_consumer_cpp)
  endif()
  target_link_libraries(${consumer} PRIVATE Salts::ServiceHost)
  add_test(NAME ${consumer} COMMAND ${consumer})
endforeach()
# A static-only Configurator consumer must resolve with no Plugin/ServiceHost
# archive or module loader dependency in the link closure.
add_executable(salts_configurator_only_consumer consumer_configurator.c)
set_target_properties(salts_configurator_only_consumer PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(salts_configurator_only_consumer PRIVATE Salts::Configurator)
add_test(NAME salts_configurator_only_consumer COMMAND salts_configurator_only_consumer)
# Native hosts additionally compile two genuinely distinct Provider DSOs using
# only installed Salts SDK imports; this is not a private staged runtime.
# Device cross-compile jobs continue to test the installed consumer's C11 and
# C++17 link contracts without pretending to execute a Provider on-device.
if(NOT CMAKE_CROSSCOMPILING)
  foreach(index IN ITEMS 1 2)
    set(provider salts_installed_provider_${index})
    add_library(${provider} SHARED installed_provider.c)
    set_target_properties(${provider} PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(${provider} PRIVATE INSTALLED_PROVIDER_INDEX=${index})
    target_link_libraries(${provider} PRIVATE Salts::ComponentPluginABI Salts::PluginABI)
  endforeach()
  foreach(lang IN ITEMS c cpp)
    if(lang STREQUAL "c")
      set(consumer salts_installed_dso_consumer_c)
      add_executable(${consumer} installed_dso_consumer.c)
      set_target_properties(${consumer} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    else()
      set(consumer salts_installed_dso_consumer_cpp)
      add_executable(${consumer} installed_dso_consumer.cpp)
      set_target_properties(${consumer} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
    target_link_libraries(${consumer} PRIVATE Salts::ServiceHost)
    add_dependencies(${consumer} salts_installed_provider_1 salts_installed_provider_2)
    add_test(NAME ${consumer} COMMAND ${consumer}
      "$<TARGET_FILE:salts_installed_provider_1>"
      "$<TARGET_FILE:salts_installed_provider_2>")
  endforeach()
endif()
# Windows DLLs come only from the exact installed SDK selected by the test.
# Put that one dependency closure beside consumers for native Windows loader
# resolution; never search PATH or a second/stale SDK at execution time.
if(WIN32)
  file(GLOB salts_installed_dlls "${SALTS_INSTALLED_ROOT}/bin/*.dll")
  set(installed_sdk_run_targets
      salts_abi_consumer_c salts_abi_consumer_cpp salts_configurator_only_consumer)
  if(TARGET salts_installed_dso_consumer_c)
    list(APPEND installed_sdk_run_targets
         salts_installed_dso_consumer_c salts_installed_dso_consumer_cpp
         salts_installed_provider_1 salts_installed_provider_2)
  endif()
  foreach(consumer IN LISTS installed_sdk_run_targets)
    foreach(dll IN LISTS salts_installed_dlls)
      add_custom_command(TARGET ${consumer} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}"
                "$<TARGET_FILE_DIR:${consumer}>"
        VERBATIM)
    endforeach()
  endforeach()
endif()
# The consumer project references no Salts source-tree paths, private archives,
# or build-interface candidate targets. Imported target linkage is mandatory.
