cmake_minimum_required(VERSION 3.25)
project(SaltsCstlDsoOdrConsumer LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The ASan CSTL DSO comparison currently requires Linux")
endif()
if(NOT DEFINED EXACT_SALTS_PREFIX OR
   NOT IS_DIRECTORY "${EXACT_SALTS_PREFIX}")
  message(FATAL_ERROR "EXACT_SALTS_PREFIX must name a fresh installed Salts SDK")
endif()
file(REAL_PATH "${EXACT_SALTS_PREFIX}" _prefix)
if(NOT EXISTS "${_prefix}/lib/cmake/Salts/SaltsConfig.cmake")
  message(FATAL_ERROR "An exact installed Salts package config is required")
endif()
find_package(Salts CONFIG REQUIRED
  PATHS "${_prefix}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::CSTL)
  message(FATAL_ERROR "The exact SDK does not export Salts::CSTL")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(_sanitizer -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)

add_library(cstl_probe_provider SHARED provider.c)
target_link_libraries(cstl_probe_provider PRIVATE Salts::CSTL)
target_compile_options(cstl_probe_provider PRIVATE ${_sanitizer})
target_link_options(cstl_probe_provider PRIVATE -fsanitize=address,undefined
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/provider.map")
set_target_properties(cstl_probe_provider PROPERTIES
  BUILD_RPATH "${_prefix}/lib")

add_executable(cstl_probe_loader loader.c)
target_link_libraries(cstl_probe_loader PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(cstl_probe_loader PRIVATE ${_sanitizer})
target_link_options(cstl_probe_loader PRIVATE -fsanitize=address,undefined
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/loader.map")
set_target_properties(cstl_probe_loader PROPERTIES BUILD_RPATH "${_prefix}/lib")
