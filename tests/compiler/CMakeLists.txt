# Build independently of the full Salts/vcpkg graph, or from the Salts root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(SaltsCompilerConformance LANGUAGES C)
  include(CTest)
endif()

find_package(Threads REQUIRED)

function(salts_add_atomic_conformance target file)
  add_executable(${target} "${file}")
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
