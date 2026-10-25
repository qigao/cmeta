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
