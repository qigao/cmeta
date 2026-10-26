cmake_minimum_required(VERSION 3.20)
project(SaltsDisruptorRetirementRegression LANGUAGES C)

include(CTest)
find_package(Threads REQUIRED)

# Isolated source-instrumented regression; no vcpkg and no SDK installation.
get_filename_component(SALTS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SALTS_RETIREMENT_SANITIZER "none" CACHE STRING "none, tsan, or asan-ubsan")
set_property(CACHE SALTS_RETIREMENT_SANITIZER PROPERTY STRINGS none tsan asan-ubsan)

if(SALTS_RETIREMENT_SANITIZER STREQUAL "tsan")
    set(_retirement_sanitizer_flags -fsanitize=thread)
elseif(SALTS_RETIREMENT_SANITIZER STREQUAL "asan-ubsan")
    set(_retirement_sanitizer_flags -fsanitize=address,undefined)
elseif(NOT SALTS_RETIREMENT_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unsupported sanitizer profile: ${SALTS_RETIREMENT_SANITIZER}")
endif()

function(add_retirement_regression target source)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/${source}"
        "${SALTS_REPO_ROOT}/concurrency/src/disruptor.c"
        "${SALTS_REPO_ROOT}/platform/src/thread_posix.c")
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(${target} PRIVATE
        "${SALTS_REPO_ROOT}/concurrency/include"
        "${SALTS_REPO_ROOT}/utils/include"
        "${SALTS_REPO_ROOT}/platform/include")
    target_compile_definitions(${target} PRIVATE
        _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(_retirement_sanitizer_flags)
        target_compile_options(${target} PRIVATE -O1 -g -fno-omit-frame-pointer ${_retirement_sanitizer_flags})
        target_link_options(${target} PRIVATE ${_retirement_sanitizer_flags})
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endfunction()

add_retirement_regression(disruptor_retirement_fifo disruptor_retirement_fifo_test.c)
add_retirement_regression(disruptor_retirement_mpmc disruptor_retirement_mpmc_test.c)
