cmake_minimum_required(VERSION 3.20)
project(salts_turbo_build_cmake_graph LANGUAGES C)

# Reuse the actual Salts target declaration, while keeping this pilot graph
# intentionally narrow so unrelated SDK dependencies do not enter the test.
if(NOT DEFINED SALTS_SOURCE_DIR OR NOT IS_DIRECTORY "${SALTS_SOURCE_DIR}")
  message(FATAL_ERROR "SALTS_SOURCE_DIR must point to the Salts checkout")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory("${SALTS_SOURCE_DIR}/tools/wsparser"
                 "${CMAKE_BINARY_DIR}/salts-wsparser")
