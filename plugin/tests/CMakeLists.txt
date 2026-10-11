include("${CMAKE_CURRENT_LIST_DIR}/native_tests.cmake")

if(WIN32)
  add_library(cmeta_plugin_windows_dependency SHARED windows_dependency.c)
  target_compile_definitions(cmeta_plugin_windows_dependency PRIVATE
    PLUGIN_WINDOWS_DEPENDENCY_BUILD)

  add_library(cmeta_plugin_windows_fixture SHARED windows_fixture.c)
  add_library(cmeta_plugin_windows_registered_fixture SHARED windows_fixture.c)
  foreach(fixture IN ITEMS cmeta_plugin_windows_fixture cmeta_plugin_windows_registered_fixture)
    target_link_libraries(${fixture} PRIVATE
      Salts::PluginABI cmeta_plugin_windows_dependency)
  endforeach()

  set_target_properties(cmeta_plugin_windows_dependency cmeta_plugin_windows_fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/private-plugin/$<CONFIG>"
    FOLDER "plugin/tests/fixtures")
  set_target_properties(cmeta_plugin_windows_registered_fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/registered-plugin/$<CONFIG>"
    FOLDER "plugin/tests/fixtures")

  cmake_add_test(cmeta_plugin_windows_search_test
    SOURCES windows_search_test.c
    LIBS Salts::Plugin Salts::TinyTest
    DEFS
      "PRIVATE_PLUGIN_PATH=\"$<TARGET_FILE:cmeta_plugin_windows_fixture>\""
      "RELATIVE_PLUGIN_PATH=\"private-plugin/$<CONFIG>/$<TARGET_FILE_NAME:cmeta_plugin_windows_fixture>\""
      "REGISTERED_PLUGIN_PATH=\"$<TARGET_FILE:cmeta_plugin_windows_registered_fixture>\""
      "REGISTERED_DEPENDENCY_DIR=L\"$<TARGET_FILE_DIR:cmeta_plugin_windows_dependency>\""
    FOLDER "plugin/tests")
  add_dependencies(cmeta_plugin_windows_search_test
    cmeta_plugin_windows_fixture cmeta_plugin_windows_registered_fixture)
  set_tests_properties(cmeta_plugin_windows_search_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
