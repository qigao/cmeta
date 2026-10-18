cmake_minimum_required(VERSION 3.20)
project(salts_turbo_build_platform_graph VERSION 2.3.0 LANGUAGES C)

if(NOT DEFINED SALTS_SOURCE_DIR OR NOT IS_DIRECTORY "${SALTS_SOURCE_DIR}")
  message(FATAL_ERROR "SALTS_SOURCE_DIR must point to the Salts checkout")
endif()

include(GNUInstallDirs)
include("${SALTS_SOURCE_DIR}/cmake/CmakeUtils.cmake")
find_package(Threads REQUIRED)

set(SALTS_NATIVE_SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(BUILD_TESTS OFF CACHE BOOL "Disable platform test subdirectory in pilot" FORCE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory("${SALTS_SOURCE_DIR}/platform"
                 "${CMAKE_BINARY_DIR}/salts-platform")
