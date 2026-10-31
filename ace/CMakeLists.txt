# Provider declarations carry the canonical CMeta and Plugin contracts.
# They do not link the ACE construction/publication runtime.
add_library(cmeta_provider INTERFACE)
cmake_config_target(cmeta_provider
  ALIAS CMeta::Provider FOLDER "ace"
  EXPORT_NAME Provider)
target_compile_features(cmeta_provider INTERFACE c_std_11)
target_include_directories(cmeta_provider INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(cmeta_provider INTERFACE CMeta::CMeta CMeta::PluginABI)

# One static owner for Configurator construction and ServiceHost generations.
# Plugin remains the independent loader, Registry and lease/unload authority.
add_library(cmeta_ace STATIC
  src/configurator.c src/configurator_abi.c
  src/service_host.c src/service_host_runtime.c src/service_host_abi.c)
cmake_config_target(cmeta_ace
  ALIAS CMeta::ACE FOLDER "ace"
  EXPORT_NAME ACE OUTPUT_NAME cmeta_ace)
target_compile_features(cmeta_ace PUBLIC c_std_11)
target_include_directories(cmeta_ace PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(cmeta_ace
  PUBLIC CMeta::Provider
  PRIVATE CMeta::Plugin Salts::Platform)

install(TARGETS cmeta_provider cmeta_ace
  EXPORT CMetaTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  include/salts/configurator_abi.h
  include/salts/service_host_provider_abi.h
  include/salts/configurator.h
  include/salts/service_host.h
  include/salts/service_host_abi.h
  include/salts/native_io_ace_token.h
  DESTINATION include/salts)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
