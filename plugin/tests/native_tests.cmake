if(NOT TARGET CMeta::CMetaNative)
  return()
endif()
add_library(cmeta_plugin_native_fixture SHARED fixture_plugin_native.c
  ../../cmeta/tests/cmeta_native_targets.c)
target_link_libraries(cmeta_plugin_native_fixture PRIVATE CMeta::PluginABI CMeta::CMetaNative)
set_target_properties(cmeta_plugin_native_fixture PROPERTIES
  FOLDER "plugin/tests/fixtures" C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
cmake_add_test(cmeta_plugin_native_test SOURCES plugin_native_test.c
  LIBS CMeta::Plugin CMeta::CMetaNative Salts::TinyTest
  DEFS "NATIVE_PLUGIN_PATH=\"$<TARGET_FILE:cmeta_plugin_native_fixture>\"" FOLDER "plugin/tests")
add_dependencies(cmeta_plugin_native_test cmeta_plugin_native_fixture)
if(BUILD_BENCHMARKS OR CMETA_BUILD_BENCHMARKS)
  cmake_add_benchmark(cmeta_plugin_native_benchmark SOURCES plugin_native_test.c
    LIBS CMeta::Plugin CMeta::CMetaNative Salts::TinyTest
    DEFS CMETA_NATIVE_PLUGIN_BENCHMARK=1
      "NATIVE_PLUGIN_PATH=\"$<TARGET_FILE:cmeta_plugin_native_fixture>\"" FOLDER "plugin/benchmarks")
  add_dependencies(cmeta_plugin_native_benchmark cmeta_plugin_native_fixture)
  add_test(NAME cmeta_plugin_native_benchmark COMMAND cmeta_plugin_native_benchmark)
endif()
