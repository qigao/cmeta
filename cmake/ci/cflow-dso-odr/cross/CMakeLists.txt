cmake_minimum_required(VERSION 3.27)
project(SaltsCFlowInstalledCrossDsoConsumer LANGUAGES C)
if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "Cross-platform CFlow consumer requires Windows or macOS")
endif()
if(NOT DEFINED EXACT_SALTS_PREFIX OR NOT IS_DIRECTORY "${EXACT_SALTS_PREFIX}")
  message(FATAL_ERROR "EXACT_SALTS_PREFIX must be a real installed Salts SDK")
endif()
file(REAL_PATH "${EXACT_SALTS_PREFIX}" _exact_prefix)
if(NOT EXISTS "${_exact_prefix}/lib/cmake/Salts/SaltsConfig.cmake")
  message(FATAL_ERROR "exact installed Salts package config missing")
endif()
find_package(Salts CONFIG REQUIRED
  PATHS "${_exact_prefix}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::CFlow)
  message(FATAL_ERROR "installed Salts::CFlow target missing")
endif()
if(NOT CROSS_CFLOW_EXPECTED_IDENTITY MATCHES "^(isolated|shared)$")
  message(FATAL_ERROR "CROSS_CFLOW_EXPECTED_IDENTITY must be isolated|shared")
endif()
enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Windows explicitly exports only our three defined test APIs, NOT any
# embedded static CFlow descriptor. Shared-single imports remain producer
# responsibility and must not be silently resolved by a consumer workaround.
if(WIN32)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()
foreach(role IN ITEMS client server)
  add_library(cflow_cross_${role} SHARED
    "../consumer/provider.c")
  target_link_libraries(cflow_cross_${role} PRIVATE Salts::CFlow)
  target_include_directories(cflow_cross_${role} PRIVATE "../consumer")
  set_target_properties(cflow_cross_${role} PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  if(APPLE)
    set_target_properties(cflow_cross_${role} PROPERTIES
      BUILD_RPATH "${_exact_prefix}/lib")
  endif()
endforeach()
add_executable(cflow_cross_loader portable_loader.c)
target_include_directories(cflow_cross_loader PRIVATE "../consumer")
if(APPLE)
  set_target_properties(cflow_cross_loader PROPERTIES
    BUILD_RPATH "${_exact_prefix}/lib")
endif()
add_test(NAME cflow_cross_client_first
  COMMAND cflow_cross_loader
    "$<TARGET_FILE:cflow_cross_client>"
    "$<TARGET_FILE:cflow_cross_server>"
    "${CROSS_CFLOW_EXPECTED_IDENTITY}")
add_test(NAME cflow_cross_server_first
  COMMAND cflow_cross_loader
    "$<TARGET_FILE:cflow_cross_server>"
    "$<TARGET_FILE:cflow_cross_client>"
    "${CROSS_CFLOW_EXPECTED_IDENTITY}")
set_tests_properties(cflow_cross_client_first cflow_cross_server_first
  PROPERTIES TIMEOUT 90)
