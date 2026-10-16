cmake_minimum_required(VERSION 3.20)
project(SaltsComponentRuntimeInstalledABIProbe LANGUAGES C CXX)
enable_testing()
if(NOT DEFINED SALTS_INSTALLED_ROOT OR NOT IS_DIRECTORY "${SALTS_INSTALLED_ROOT}")
  message(FATAL_ERROR "Set SALTS_INSTALLED_ROOT to one installed native SDK")
endif()
find_package(Salts CONFIG REQUIRED PATHS "${SALTS_INSTALLED_ROOT}" NO_DEFAULT_PATH)
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
# Windows DLLs come only from the exact installed SDK selected by the test.
# Put that one dependency closure beside consumers for native Windows loader
# resolution; never search PATH or a second/stale SDK at execution time.
if(WIN32)
  file(GLOB salts_installed_dlls "${SALTS_INSTALLED_ROOT}/bin/*.dll")
  foreach(consumer IN ITEMS salts_abi_consumer_c salts_abi_consumer_cpp
                            salts_configurator_only_consumer)
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
