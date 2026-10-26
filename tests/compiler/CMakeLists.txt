# Build independently of the full Salts/vcpkg graph, or from the Salts root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(SaltsCompilerConformance LANGUAGES C)
  include(CTest)
endif()

find_package(Threads REQUIRED)

function(salts_add_atomic_conformance target)
  add_executable(${target} ${ARGN})
  # CMake identifies TinyCC but does not publish a c_std_11 feature table.
  # Let the compiler and the source-level C11 guards reject unsupported syntax;
  # do not claim feature support or enable an atomic fallback here.
  if(CMAKE_C_COMPILER_ID STREQUAL "TinyCC")
    target_compile_options(${target} PRIVATE -std=c11)
  else()
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  endif()
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  endif()
  if(BUILD_TESTING)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
  endif()
endfunction()

salts_add_atomic_conformance(
  salts_atomic_language_conformance atomic_language_conformance.c)
salts_add_atomic_conformance(
  salts_atomic_contract_conformance salts_atomic_contract_conformance.c)
target_include_directories(salts_atomic_contract_conformance PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")

# Real same-compiler baseline for the independent mixed GCC/TCC ABI probes.
salts_add_atomic_conformance(salts_atomic_abi_same_compiler
  atomic_abi_driver.c atomic_abi_peer.c)

# Qualify actual POSIX Platform threading primitives without the full SDK.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")

  # Explicit external Linux assembly entrypoints, NOT a C11 stdatomic shim.
  # Use the native GAS/Clang assembler on each target architecture and keep
  # the selected architecture's .S object separate from C compiler builtins.
  # The strict TinyCC C11 feature gate stays unchanged.
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "TinyCC" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      set(_salts_asm_atomic_source asm-atomic/asm_atomic_x86_64.S)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      set(_salts_asm_atomic_source asm-atomic/asm_atomic_aarch64.S)
    endif()
    if(DEFINED _salts_asm_atomic_source)
      enable_language(ASM)
      salts_add_atomic_conformance(salts_asm_atomic_conformance
        asm-atomic/asm_atomic_conformance.c
        "${CMAKE_CURRENT_LIST_DIR}/${_salts_asm_atomic_source}")
      set_tests_properties(salts_asm_atomic_conformance PROPERTIES TIMEOUT 60)
      # THREE identical source tests also run against the private assembly
      # atomic slots. Original Disruptor algorithms are #included unmodified
      # into ONE private experimental TU; consumers still use public opaque API.
      foreach(_variant real_source_conformance mpmc_real_source parked_worker_conformance)
        salts_add_atomic_conformance("salts_disruptor_asm_${_variant}"
          "disruptor_${_variant}.c"
          asm-atomic/disruptor_asm_inclusion.c
          "${CMAKE_CURRENT_LIST_DIR}/${_salts_asm_atomic_source}"
          "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
        target_include_directories("salts_disruptor_asm_${_variant}" PRIVATE
          "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include"
          "${CMAKE_CURRENT_LIST_DIR}/../../utils/include"
          "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
        target_compile_definitions("salts_disruptor_asm_${_variant}" PRIVATE
          _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
        set_tests_properties("salts_disruptor_asm_${_variant}" PROPERTIES TIMEOUT 60)
      endforeach()
      # Fair paired wall-time comparison: compile EXACT same 4x4 MPMC
      # harness, original Disruptor source and POSIX Platform source with
      # the same C compiler, C flags, workload and 64-slot capacity.
      # Only atomic storage + primitive translation unit differ.
      # Do not make an observational throughput ratio a release gate.
      add_executable(salts_disruptor_native_benchmark
        disruptor_mpmc_real_source.c
        "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/src/disruptor.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
      add_executable(salts_disruptor_asm_benchmark
        disruptor_mpmc_real_source.c
        asm-atomic/disruptor_asm_inclusion.c
        "${CMAKE_CURRENT_LIST_DIR}/${_salts_asm_atomic_source}"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
      # Separate per-message age distribution test, using exactly the same
      # source and aligned 16-byte timestamped ring payload on both backends.
      add_executable(salts_disruptor_native_latency
        disruptor_mpmc_real_source.c
        "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/src/disruptor.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
      add_executable(salts_disruptor_asm_latency
        disruptor_mpmc_real_source.c
        asm-atomic/disruptor_asm_inclusion.c
        "${CMAKE_CURRENT_LIST_DIR}/${_salts_asm_atomic_source}"
        "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
      foreach(_benchmark_target
        salts_disruptor_native_benchmark
        salts_disruptor_asm_benchmark
        salts_disruptor_native_latency
        salts_disruptor_asm_latency)
        target_compile_features(${_benchmark_target} PRIVATE c_std_11)
        set_target_properties(${_benchmark_target} PROPERTIES
          C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_include_directories(${_benchmark_target} PRIVATE
          "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include"
          "${CMAKE_CURRENT_LIST_DIR}/../../utils/include"
          "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
        target_compile_definitions(${_benchmark_target} PRIVATE
          _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
        if(_benchmark_target MATCHES "_latency$")
          target_compile_definitions(${_benchmark_target} PRIVATE
            SALTS_COMPILER_MPMC_LATENCY=1 SALTS_COMPILER_MPMC_EACH=4000)
        else()
          target_compile_definitions(${_benchmark_target} PRIVATE
            SALTS_COMPILER_MPMC_BENCHMARK=1 SALTS_COMPILER_MPMC_EACH=20000)
        endif()
        target_compile_options(${_benchmark_target} PRIVATE
          -O2 -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(${_benchmark_target} PRIVATE Threads::Threads)
      endforeach()
      unset(_salts_asm_atomic_source)
    endif()
  endif()
  salts_add_atomic_conformance(salts_native_tls_probe
    tcc_tls_probe.c)
  # Key-backed POSIX TSS is an independently tested design reference, NOT
  # a replacement for the SALTS_THREAD_LOCAL storage-class contract.
  salts_add_atomic_conformance(salts_posix_tss_semantics_probe
    posix_tss_semantics_probe.c)
  # Independent GNU __atomic intrinsic *storage* backend diagnostic only.
  # This target is not a C11 _Atomic ABI replacement and is never installed.
  salts_add_atomic_conformance(salts_intrinsic_atomic_conformance
    intrinsic_atomic_conformance.c)
  # This is Salts' original SPSC implementation + original public header.
  # _Atomic layout is public: do NOT substitute the private intrinsic storage.
  salts_add_atomic_conformance(salts_spsc_real_source_conformance
    spsc_real_source_conformance.c
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/src/spsc_ring.c")
  target_include_directories(salts_spsc_real_source_conformance PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include")
  # Unlike SPSC, Disruptor keeps its atomic state opaque. Link actual
  # production source + actual Salts::Platform POSIX implementation.
  salts_add_atomic_conformance(salts_disruptor_real_source_conformance
    disruptor_real_source_conformance.c
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/src/disruptor.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
  target_include_directories(salts_disruptor_real_source_conformance PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../utils/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
  target_compile_definitions(salts_disruptor_real_source_conformance PRIVATE
    _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
  # Full opaque worker-pool contention: 4 publishers and 4 consumers.
  salts_add_atomic_conformance(salts_disruptor_mpmc_real_source
    disruptor_mpmc_real_source.c
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/src/disruptor.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
  target_include_directories(salts_disruptor_mpmc_real_source PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../utils/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
  target_compile_definitions(salts_disruptor_mpmc_real_source PRIVATE
    _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
  # Real worker_waiters + pthread condpark path, including wake-on-publication
  # and explicit bounded cancellation; original opaque Disruptor source only.
  salts_add_atomic_conformance(salts_disruptor_parked_worker_conformance
    disruptor_parked_worker_conformance.c
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/src/disruptor.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
  target_include_directories(salts_disruptor_parked_worker_conformance PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../concurrency/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../utils/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
  target_compile_definitions(salts_disruptor_parked_worker_conformance PRIVATE
    _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
  target_include_directories(salts_intrinsic_atomic_conformance PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
  salts_add_atomic_conformance(salts_platform_posix_thread_probe
    tcc_platform_thread_probe.c
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/src/thread_posix.c")
  target_include_directories(salts_platform_posix_thread_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../platform/include")
  target_compile_definitions(salts_platform_posix_thread_probe PRIVATE
    _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
endif()
