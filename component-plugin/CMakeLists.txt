# One ABI-only CMeta Plugin export contract for Component providers.
# The implementation and all generation ownership belong to ServiceHost.
add_library(salts_component_provider_abi INTERFACE)
cmake_config_target(salts_component_provider_abi
  ALIAS Salts::ComponentProviderABI
  FOLDER "component-provider"
  EXPORT_NAME ComponentProviderABI)
target_compile_features(salts_component_provider_abi INTERFACE c_std_11)
target_include_directories(salts_component_provider_abi INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(salts_component_provider_abi INTERFACE
  Salts::ComponentABI Salts::PluginABI)
install(TARGETS salts_component_provider_abi EXPORT SaltsTargets)
install(FILES include/salts/component_provider_abi.h DESTINATION include/salts)
# No Salts::ComponentPlugin or Salts::ComponentPluginABI compatibility alias.
