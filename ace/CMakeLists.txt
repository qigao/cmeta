# Canonical ABI contracts live beside their only runtime implementations.
add_library(salts_configurator_abi INTERFACE)
cmake_config_target(salts_configurator_abi
  ALIAS CMeta::ConfiguratorABI FOLDER "ace"
  EXPORT_NAME ConfiguratorABI)
target_compile_features(salts_configurator_abi INTERFACE c_std_11)
target_include_directories(salts_configurator_abi INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_configurator_abi INTERFACE CMeta::CMeta)

add_library(salts_service_host_provider_abi INTERFACE)
cmake_config_target(salts_service_host_provider_abi
  ALIAS CMeta::ServiceHostProviderABI FOLDER "ace"
  EXPORT_NAME ServiceHostProviderABI)
target_compile_features(salts_service_host_provider_abi INTERFACE c_std_11)
target_include_directories(salts_service_host_provider_abi INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_service_host_provider_abi INTERFACE
  CMeta::ConfiguratorABI CMeta::PluginABI)

# The only installed lifecycle graph and Plugin generation owner.
# ServiceHost composes Configurator, CMeta ProviderABI and the existing Plugin
# registry; it does not load modules or retain callbacks through a second path.
add_library(salts_configurator STATIC
  src/configurator.c src/configurator_abi.c)
cmake_config_target(salts_configurator
  ALIAS CMeta::Configurator
  FOLDER "ace"
  EXPORT_NAME Configurator
  OUTPUT_NAME salts_configurator)
target_compile_features(salts_configurator PUBLIC c_std_11)
target_include_directories(salts_configurator PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_configurator PUBLIC CMeta::ConfiguratorABI)

add_library(salts_service_host STATIC
  src/service_host.c src/service_host_runtime.c src/service_host_abi.c)
cmake_config_target(salts_service_host
  ALIAS CMeta::ServiceHost
  FOLDER "ace"
  EXPORT_NAME ServiceHost
  OUTPUT_NAME salts_service_host)
target_compile_features(salts_service_host PUBLIC c_std_11)
target_include_directories(salts_service_host PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_service_host
  PUBLIC CMeta::Configurator CMeta::ServiceHostProviderABI
  PRIVATE CMeta::Plugin Salts::Platform)

install(TARGETS salts_configurator_abi salts_service_host_provider_abi
  salts_configurator salts_service_host
  EXPORT SaltsTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  include/cmeta/configurator_abi.h
  include/cmeta/service_host_provider_abi.h
  include/cmeta/configurator.h
  include/cmeta/service_host.h
  include/cmeta/service_host_abi.h
  include/cmeta/native_io_ace_token.h
  DESTINATION include/cmeta)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
