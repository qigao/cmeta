cmake_add_benchmark(cflow_direct_benchmark
  SOURCES
    cflow_direct_benchmark.c
    cflow_direct_benchmark_consume.c
    cflow_range_benchmark.c
    cflow_graph_path_benchmark.c
    cflow_branching_views.c
    cflow_branching_csr_benchmark.c
  LIBS CMeta::CFlow CMeta::CSTL Salts::TinyTest
  FOLDER "cflow/benchmarks")

cmake_add_benchmark(cflow_parallel_reduce_benchmark
  SOURCES cflow_parallel_reduce_benchmark.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/benchmarks")

cmake_add_benchmark(cflow_machine_hierarchy_benchmark
  SOURCES cflow_machine_hierarchy_benchmark.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/benchmarks")

cmake_add_benchmark(cflow_reactive_benchmark
  SOURCES cflow_reactive_benchmark.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/benchmarks")

cmake_add_benchmark(cflow_actor_mailbox_benchmark
  SOURCES cflow_actor_mailbox_benchmark.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/benchmarks")
add_test(NAME cflow_actor_mailbox_benchmark COMMAND cflow_actor_mailbox_benchmark)
set_tests_properties(cflow_actor_mailbox_benchmark PROPERTIES LABELS benchmark TIMEOUT 60)
set_target_properties(cflow_actor_mailbox_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_benchmark(cflow_ace_throughput_benchmark
  SOURCES cflow_ace_throughput_benchmark.c
  LIBS CMeta::CFlow Salts::Core Salts::Platform Salts::TinyTest
  FOLDER "cflow/benchmarks")
add_test(NAME cflow_ace_throughput_benchmark COMMAND cflow_ace_throughput_benchmark)
set_tests_properties(cflow_ace_throughput_benchmark PROPERTIES LABELS benchmark TIMEOUT 600)
set_target_properties(cflow_ace_throughput_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

set_target_properties(cflow_direct_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

target_include_directories(cflow_direct_benchmark PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set_target_properties(cflow_parallel_reduce_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

set_target_properties(cflow_machine_hierarchy_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

set_target_properties(cflow_reactive_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)


cmake_add_benchmark(cflow_control_path_benchmark
  SOURCES cflow_control_path_benchmark.c
  LIBS CMeta::CFlow Salts::NativeIO Salts::Platform
  FOLDER "cflow/benchmarks")

set_target_properties(cflow_control_path_benchmark PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
