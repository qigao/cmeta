# Opt-in Salts-owned migration candidate (#1154). No SDK exports/install.
# This code is a separately named copy of the existing Component and
# ComponentPlugin runtimes. Do not link old and new owners to one live graph.
add_library(salts_configurator_candidate STATIC src/configurator.c)
add_library(Salts::ConfiguratorCandidate ALIAS salts_configurator_candidate)
target_compile_features(salts_configurator_candidate PRIVATE c_std_11)
target_include_directories(salts_configurator_candidate PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(salts_configurator_candidate PUBLIC Salts::ComponentABI)
set_target_properties(salts_configurator_candidate PROPERTIES
  FOLDER "component-runtime")

add_library(salts_service_host_candidate STATIC
  src/service_host.c
  src/service_host_runtime.c)
add_library(Salts::ServiceHostCandidate ALIAS salts_service_host_candidate)
target_compile_features(salts_service_host_candidate PRIVATE c_std_11)
target_include_directories(salts_service_host_candidate PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(salts_service_host_candidate
  PUBLIC salts_configurator_candidate Salts::ComponentPluginABI
  PRIVATE Salts::Plugin Salts::Platform)
set_target_properties(salts_service_host_candidate PROPERTIES
  FOLDER "component-runtime")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Salts #1153: nonpublishable installed-ABI proof only. This flag stays OFF
# in defaults, on #1155, and in all production/published SDK configurations.
# The exported target names are the proposed final names, but original
# Component/ComponentPlugin provider ABI remains separately qualified.
if(SALTS_ENABLE_COMPONENT_RUNTIME_INSTALLED_ABI_PROBE)
  add_library(Salts::Configurator ALIAS salts_configurator_candidate)
  add_library(Salts::ServiceHost ALIAS salts_service_host_candidate)
  target_sources(salts_configurator_candidate PRIVATE src/configurator_abi.c)
  target_sources(salts_service_host_candidate PRIVATE src/service_host_abi.c)
  target_compile_features(salts_configurator_candidate PUBLIC c_std_11)
  target_compile_features(salts_service_host_candidate PUBLIC c_std_11)
  target_include_directories(salts_configurator_candidate PUBLIC
    "$<INSTALL_INTERFACE:include>")
  target_include_directories(salts_service_host_candidate PUBLIC
    "$<INSTALL_INTERFACE:include>")
  set_target_properties(salts_configurator_candidate PROPERTIES
    EXPORT_NAME Configurator OUTPUT_NAME salts_configurator)
  set_target_properties(salts_service_host_candidate PROPERTIES
    EXPORT_NAME ServiceHost OUTPUT_NAME salts_service_host)
  install(TARGETS salts_configurator_candidate salts_service_host_candidate
    EXPORT SaltsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/salts DESTINATION include
    FILES_MATCHING PATTERN "*.h")
endif()
