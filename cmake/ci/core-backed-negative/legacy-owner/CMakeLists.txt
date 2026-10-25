cmake_minimum_required(VERSION 3.20)
project(Salts3OldRuntimeTargetMustFail LANGUAGES C CXX)
if(NOT DEFINED SALTS_SOURCE_ROOT OR
   NOT IS_ABSOLUTE "${SALTS_SOURCE_ROOT}")
  message(FATAL_ERROR "SALTS_SOURCE_ROOT is required for negative legacy owner test")
endif()
# Inject ONE old Platform target into the parent scope of the actual
# production CMake project. The profile must reject this, not use it.
add_library(salts_m6_fake_old_platform INTERFACE)
add_library(Salts::Platform ALIAS salts_m6_fake_old_platform)
add_subdirectory("${SALTS_SOURCE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/real-salts")
