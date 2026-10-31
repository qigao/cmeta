# Original runtime regressions in the production build graph. No second project,
# installed consumer harness, private runtime copy or alternate test profile.
function(salts_native_case name source)
  add_executable(salts_native_${name} "${source}")
  target_compile_features(salts_native_${name} PRIVATE c_std_11 cxx_std_17)
  target_include_directories(salts_native_${name} PRIVATE
    "${PROJECT_SOURCE_DIR}/ace/tests"
    "${PROJECT_SOURCE_DIR}/cflow/src")
  target_link_libraries(salts_native_${name} PRIVATE
    Salts::TinyTest CMeta::CMeta CMeta::CFlow CMeta::ACE
    CMeta::Plugin Salts::Core Salts::Platform Salts::NativeIO Salts::Concurrency)
  if(WIN32)
    target_link_libraries(salts_native_${name} PRIVATE ws2_32)
  endif()
  add_test(NAME salts.native.${name} COMMAND salts_native_${name})
  set_tests_properties(salts.native.${name} PROPERTIES TIMEOUT 120)
endfunction()

salts_native_case(plugin_utf8_path utf8_plugin_path_test.c)
foreach(flow IN ITEMS status stream_slice ace_pipes_filters event_mailbox
    io_native_adapter io_native_sharded_adapter machine_instance)
  salts_native_case(cflow_${flow}
    "${PROJECT_SOURCE_DIR}/cflow/tests/cflow_${flow}_test.c")
endforeach()

# Preserve the former C11/C++17 owner and ABI assertions as ordinary tests.
foreach(pair IN ITEMS "owner_c11|runtime_owner_test.c"
    "owner_cpp17|runtime_owner_cpp_test.cpp"
    "cflow_owner_c11|cflow_owner_test.c"
    "cflow_owner_cpp17|cflow_owner_cpp_test.cpp")
  string(REPLACE "|" ";" row "${pair}")
  list(GET row 0 name)
  list(GET row 1 source)
  add_executable(salts_${name} "${source}")
  target_compile_features(salts_${name} PRIVATE c_std_11 cxx_std_17)
  target_link_libraries(salts_${name} PRIVATE
    CMeta::ACE CMeta::Plugin CMeta::CFlow Salts::Core
    Salts::NativeIO Salts::Platform)
  add_test(NAME salts.native.${name} COMMAND salts_${name})
endforeach()
target_compile_definitions(salts_owner_c11 PRIVATE
  "SALTS_TEST_PROVIDER=\"$<TARGET_FILE:salts_service_host_provider_fixture>\"")
add_dependencies(salts_owner_c11 salts_service_host_provider_fixture)
