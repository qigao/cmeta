cmake_minimum_required(VERSION 3.20)
project(component_candidate_sdk_isolation LANGUAGES C CXX)

if(NOT DEFINED SALTS_TEST_PREFIX OR NOT DEFINED SALTS_TEST_VERSION)
  message(FATAL_ERROR "Expected installed Salts SDK prefix and exact version")
endif()

# The imported package is a real install, never a build-tree candidate alias.
find_package(Salts ${SALTS_TEST_VERSION} EXACT CONFIG REQUIRED
  PATHS "${SALTS_TEST_PREFIX}" NO_DEFAULT_PATH)

foreach(old_target IN ITEMS
    Salts::ComponentABI Salts::Component
    Salts::ComponentPluginABI Salts::ComponentPlugin
    Salts::PluginABI Salts::Plugin)
  if(NOT TARGET ${old_target})
    message(FATAL_ERROR "Existing SDK target disappeared: ${old_target}")
  endif()
endforeach()

foreach(forbidden IN ITEMS
    Salts::Configurator Salts::ServiceHost Salts::ComponentProviderABI
    Salts::ConfiguratorCandidate Salts::ServiceHostCandidate
    salts_configurator_candidate salts_service_host_candidate)
  if(TARGET ${forbidden})
    message(FATAL_ERROR "Unqualified candidate target leaked into SDK: ${forbidden}")
  endif()
endforeach()
foreach(header IN ITEMS configurator.h service_host.h)
  if(EXISTS "${SALTS_TEST_PREFIX}/include/salts/${header}")
    message(FATAL_ERROR "Unqualified candidate header installed: ${header}")
  endif()
endforeach()
file(GLOB_RECURSE installed_entries
  LIST_DIRECTORIES FALSE RELATIVE "${SALTS_TEST_PREFIX}"
  "${SALTS_TEST_PREFIX}/*")
foreach(entry IN LISTS installed_entries)
  if("${entry}" MATCHES "(^|/)(lib)?salts_(configurator|service_host)_candidate")
    message(FATAL_ERROR "Unqualified candidate binary installed: ${entry}")
  endif()
endforeach()

get_target_property(component_abi_type Salts::ComponentABI TYPE)
if(NOT component_abi_type STREQUAL "INTERFACE_LIBRARY")
  message(FATAL_ERROR "Original ComponentABI must remain header/CMeta-only")
endif()
get_target_property(component_links Salts::Component INTERFACE_LINK_LIBRARIES)
if("${component_links}" MATCHES "Salts::Plugin")
  message(FATAL_ERROR "Static Component gained an unexpected Plugin dependency")
endif()

# Execute the SAME original C11 installed-consumer sources with the SDK that
# was installed from the candidate-ON build graph.
set(legacy_sources
  "${CMAKE_CURRENT_LIST_DIR}/../../../../component/tests/package_config/component")
foreach(file IN ITEMS main.c scope.c)
  if(NOT EXISTS "${legacy_sources}/${file}")
    message(FATAL_ERROR "Missing original installed-consumer fixture: ${file}")
  endif()
endforeach()
add_executable(component_sdk_legacy_c "${legacy_sources}/main.c")
target_link_libraries(component_sdk_legacy_c PRIVATE Salts::Component)
target_compile_features(component_sdk_legacy_c PRIVATE c_std_11)
set_target_properties(component_sdk_legacy_c PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_executable(component_sdk_legacy_scope "${legacy_sources}/scope.c")
target_link_libraries(component_sdk_legacy_scope PRIVATE Salts::ComponentPlugin)
target_compile_features(component_sdk_legacy_scope PRIVATE c_std_11)
set_target_properties(component_sdk_legacy_scope PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(component_sdk_legacy_cpp sdk_legacy_cpp.cpp)
target_link_libraries(component_sdk_legacy_cpp
  PRIVATE Salts::Component Salts::ComponentPlugin)
target_compile_features(component_sdk_legacy_cpp PRIVATE cxx_std_17)
set_target_properties(component_sdk_legacy_cpp PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME component_sdk_legacy_c COMMAND component_sdk_legacy_c)
add_test(NAME component_sdk_legacy_scope COMMAND component_sdk_legacy_scope)
add_test(NAME component_sdk_legacy_cpp COMMAND component_sdk_legacy_cpp)
