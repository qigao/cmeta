# Provider declarations carry the canonical CMeta and Plugin contracts.
# They do not link the ACE construction/publication runtime.
add_library(salts_provider INTERFACE)
cmake_config_target(salts_provider
  ALIAS Salts::Provider FOLDER "ace"
  EXPORT_NAME Provider)
target_compile_features(salts_provider INTERFACE c_std_11)
target_include_directories(salts_provider INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_provider INTERFACE Salts::CMeta Salts::PluginABI)

# One static owner for Configurator construction and ServiceHost generations.
# Plugin remains the independent loader, Registry and lease/unload authority.
add_library(salts_ace STATIC
  src/configurator.c src/configurator_abi.c
  src/service_host.c src/service_host_runtime.c src/service_host_abi.c)
cmake_config_target(salts_ace
  ALIAS Salts::ACE FOLDER "ace"
  EXPORT_NAME ACE OUTPUT_NAME salts_ace)
target_compile_features(salts_ace PUBLIC c_std_11)
target_include_directories(salts_ace PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_ace
  PUBLIC Salts::Provider
  PRIVATE Salts::Plugin Salts::Platform)

install(TARGETS salts_provider salts_ace
  EXPORT SaltsTargets
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
