# Deliberately independent from the full Salts SDK build.
# Creates a real static archive from ACTUAL Disruptor/Platform C sources,
# plus one explicitly selected, independently assembled Atomic backend.
cmake_minimum_required(VERSION 3.20)
project(SaltsDisruptorTinyCC LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "TCC Disruptor ASM currently supports Linux only")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "TinyCC")
  message(FATAL_ERROR
    "This standalone target requires TinyCC. Native GCC/Clang use standard Salts C11")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "TinyCC Disruptor ASM requires a 64-bit target")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(_salts_asm_arch x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_salts_asm_arch aarch64)
else()
  message(FATAL_ERROR
    "No admitted Disruptor ASM Atomic for target ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# GCC/GAS compiles only .S; TinyCC compiles ALL C sources.
enable_language(ASM)
if(NOT CMAKE_ASM_COMPILER_ID MATCHES "^(GNU|Clang)$")
  message(FATAL_ERROR "External ASM requires native GNU/Clang assembler")
endif()
find_package(Threads REQUIRED)
include(CTest)

get_filename_component(SALTS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(salts_tcc_disruptor STATIC
  "${SALTS_ROOT}/concurrency/src/disruptor.c"
  "${SALTS_ROOT}/platform/src/thread_posix.c"
  "${SALTS_ROOT}/concurrency/src/asm-atomic/asm_atomic_${_salts_asm_arch}.S")
target_include_directories(salts_tcc_disruptor
  PUBLIC "${SALTS_ROOT}/concurrency/include"
  PRIVATE "${SALTS_ROOT}/utils/include"
          "${SALTS_ROOT}/platform/include")
target_compile_options(salts_tcc_disruptor PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-std=c11>")
target_compile_definitions(salts_tcc_disruptor PRIVATE
  SALTS_DISRUPTOR_BACKEND_ASM=1
  _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700
  SALTS_THREAD_LOCAL=_Thread_local)
target_link_libraries(salts_tcc_disruptor PUBLIC Threads::Threads)

# Deliberately do not install/export this as the full Salts::Concurrency SDK.
# Two tests are the existing master/production retirement regressions, NOT
# duplicated versions that could diverge after a bug fix is merged.
if(BUILD_TESTING)
  add_executable(salts_tcc_disruptor_retirement_fifo
    "${SALTS_ROOT}/concurrency/tests/retirement/disruptor_retirement_fifo_test.c")
  add_executable(salts_tcc_disruptor_retirement_mpmc
    "${SALTS_ROOT}/concurrency/tests/retirement/disruptor_retirement_mpmc_test.c")
  add_executable(salts_tcc_disruptor_parked_worker
    "${CMAKE_CURRENT_LIST_DIR}/tests/disruptor_parked_worker_test.c")
  foreach(_target
    salts_tcc_disruptor_retirement_fifo
    salts_tcc_disruptor_retirement_mpmc
    salts_tcc_disruptor_parked_worker)
    target_compile_options(${_target} PRIVATE -std=c11)
    target_compile_definitions(${_target} PRIVATE
      _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
    target_link_libraries(${_target} PRIVATE salts_tcc_disruptor Threads::Threads)
    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
