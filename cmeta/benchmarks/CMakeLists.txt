cmake_add_benchmark(cmeta_data_reflect_benchmark
  SOURCES cmeta_data_reflect_benchmark.c
  LIBS CMeta::CMeta Salts::TinyTest FOLDER "cmeta/benchmarks")
if(BUILD_TESTS)
  add_test(NAME cmeta_data_reflect_benchmark COMMAND cmeta_data_reflect_benchmark)
  set_tests_properties(cmeta_data_reflect_benchmark PROPERTIES LABELS "benchmark")
endif()

cmake_add_benchmark(cmeta_binding_benchmark
  SOURCES cmeta_binding_benchmark.c cmeta_binding_benchmark_target.c
  LIBS CMeta::CMeta Salts::TinyTest FOLDER "cmeta/benchmarks")
set_target_properties(cmeta_binding_benchmark PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(BUILD_TESTS)
  add_test(NAME cmeta_binding_benchmark COMMAND cmeta_binding_benchmark)
endif()

cmake_add_benchmark(cmeta_fastpath_benchmark
  SOURCES cmeta_fastpath_benchmark.c cmeta_fastpath_benchmark_target.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Platform
  FOLDER "cmeta/benchmarks")
set_target_properties(cmeta_fastpath_benchmark PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(BUILD_TESTS)
  add_test(NAME cmeta_fastpath_benchmark COMMAND cmeta_fastpath_benchmark)
endif()

cmake_add_benchmark(cmeta_trace_benchmark
  SOURCES cmeta_trace_benchmark.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Platform
  FOLDER "cmeta/benchmarks")
set_target_properties(cmeta_trace_benchmark PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(BUILD_TESTS)
  add_test(NAME cmeta_trace_benchmark COMMAND cmeta_trace_benchmark)
endif()
