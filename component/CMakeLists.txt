# Canonical CMeta Component declaration and ProviderBinding ABI only.
# Configurator is the only installed graph/lifecycle implementation.
add_library(salts_component_abi INTERFACE)
cmake_config_target(salts_component_abi
  ALIAS Salts::ComponentABI
  FOLDER "component"
  EXPORT_NAME ComponentABI)
target_compile_features(salts_component_abi INTERFACE c_std_11)
target_include_directories(salts_component_abi INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_component_abi INTERFACE Salts::CMeta)
install(TARGETS salts_component_abi EXPORT SaltsTargets)
install(FILES include/salts/component_abi.h DESTINATION include/salts)
# No Salts::Component runtime, alias, archive, headers or parallel owner.
