if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR
   NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  message(FATAL_ERROR "CMeta native thunks require x86-64 Windows or Linux")
endif()
add_library(cmeta_native STATIC thunk.c code_memory.c object.c)
cmake_config_target(cmeta_native VERSION "${PROJECT_VERSION}" SOVERSION "${PROJECT_VERSION_MAJOR}"
  ALIAS CMeta::CMetaNative EXPORT_NAME CMetaNative FOLDER "cmeta/native")
target_link_libraries(cmeta_native PUBLIC CMeta::CMeta)
target_compile_features(cmeta_native PUBLIC c_std_11)
install(TARGETS cmeta_native EXPORT CMetaTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(BUILD_TESTS)
  cmake_add_test(cmeta_native_failure_test SOURCES thunk.c ../tests/cmeta_native_failure_test.c
    ../tests/cmeta_native_targets.c LIBS CMeta::CMeta Salts::TinyTest FOLDER "cmeta/tests")
  cmake_add_test(cmeta_native_thunk_test SOURCES ../tests/cmeta_native_thunk_test.c
    ../tests/cmeta_native_targets.c LIBS CMeta::CMetaNative Salts::TinyTest FOLDER "cmeta/tests")
  set_target_properties(cmeta_native_thunk_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  cmake_add_test(cmeta_native_thunk_cpp_test SOURCES ../tests/cmeta_native_thunk_cpp_test.cpp
    LIBS CMeta::CMetaNative Salts::Platform Salts::TinyTest FOLDER "cmeta/tests")
  set_target_properties(cmeta_native_thunk_cpp_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  cmake_add_test(cmeta_native_object_test SOURCES ../tests/cmeta_native_object_test.c
    ../tests/cmeta_native_object_fixture.c
    LIBS CMeta::CMetaNative Salts::TinyTest FOLDER "cmeta/tests")
  cmake_add_test(cmeta_native_static_call_test SOURCES ../tests/cmeta_native_static_call_test.c
    ../tests/cmeta_native_targets.c
    LIBS CMeta::CMetaNative Salts::Platform Salts::TinyTest FOLDER "cmeta/tests")
  set_target_properties(cmeta_native_object_test cmeta_native_static_call_test
    PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
if(BUILD_BENCHMARKS OR CMETA_BUILD_BENCHMARKS)
  cmake_add_benchmark(cmeta_native_benchmark SOURCES ../benchmarks/cmeta_native_benchmark.c
    ../tests/cmeta_native_targets.c ../tests/cmeta_native_object_fixture.c
    LIBS CMeta::CMetaNative Salts::Platform Salts::TinyTest FOLDER "cmeta/benchmarks")
  if(BUILD_TESTS)
    add_test(NAME cmeta_native_benchmark COMMAND cmeta_native_benchmark)
  endif()
endif()
