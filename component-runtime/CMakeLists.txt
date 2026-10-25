# The only installed Component lifecycle graph and Plugin generation owner.
# ServiceHost composes Configurator, CMeta ProviderABI and the existing Plugin
# registry; it does not load modules or retain callbacks through a second path.
add_library(salts_configurator STATIC
  src/configurator.c src/configurator_abi.c)
cmake_config_target(salts_configurator
  ALIAS Salts::Configurator
  FOLDER "component-runtime"
  EXPORT_NAME Configurator
  OUTPUT_NAME salts_configurator)
target_compile_features(salts_configurator PUBLIC c_std_11)
target_include_directories(salts_configurator PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_configurator PUBLIC Salts::ComponentABI)

add_library(salts_service_host STATIC
  src/service_host.c src/service_host_runtime.c src/service_host_abi.c)
cmake_config_target(salts_service_host
  ALIAS Salts::ServiceHost
  FOLDER "component-runtime"
  EXPORT_NAME ServiceHost
  OUTPUT_NAME salts_service_host)
target_compile_features(salts_service_host PUBLIC c_std_11)
target_include_directories(salts_service_host PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
if(SALTS_PLUGIN_USE_SALTSCORE)
  if(TARGET Salts::Platform)
    message(FATAL_ERROR "ServiceHost migration refuses duplicated Platform")
  endif()
  target_link_libraries(salts_service_host
    PUBLIC Salts::Configurator Salts::ComponentProviderABI
    PRIVATE Salts::Plugin SaltsCore::Platform)
else()
  target_link_libraries(salts_service_host
    PUBLIC Salts::Configurator Salts::ComponentProviderABI
    PRIVATE Salts::Plugin Salts::Platform)
endif()

install(TARGETS salts_configurator salts_service_host
  EXPORT SaltsTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  include/salts/configurator.h
  include/salts/service_host.h
  include/salts/component_runtime_abi.h
  DESTINATION include/salts)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
