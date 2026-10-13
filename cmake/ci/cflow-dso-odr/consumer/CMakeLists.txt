cmake_minimum_required(VERSION 3.27)
project(SaltsCFlowDsoOdrConsumer LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "CFlow cross-DSO ASan gate requires Linux")
endif()
if(NOT DEFINED EXACT_SALTS_PREFIX OR
   NOT IS_DIRECTORY "${EXACT_SALTS_PREFIX}")
  message(FATAL_ERROR "EXACT_SALTS_PREFIX must name installed source SDK")
endif()
file(REAL_PATH "${EXACT_SALTS_PREFIX}" _prefix)
find_package(Salts CONFIG REQUIRED
  PATHS "${_prefix}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::CFlow)
  message(FATAL_ERROR "exact SDK missing Salts::CFlow")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(_sanitizer -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)

foreach(role IN ITEMS client server)
  add_library(cflow_probe_${role} SHARED provider.c)
  target_link_libraries(cflow_probe_${role} PRIVATE Salts::CFlow)
  target_compile_options(cflow_probe_${role} PRIVATE ${_sanitizer})
  target_link_options(cflow_probe_${role} PRIVATE -fsanitize=address,undefined
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${role}.map")
  set_target_properties(cflow_probe_${role} PROPERTIES
    BUILD_RPATH "${_prefix}/lib")
endforeach()

add_executable(cflow_probe_loader loader.c)
target_link_libraries(cflow_probe_loader PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(cflow_probe_loader PRIVATE ${_sanitizer})
target_link_options(cflow_probe_loader PRIVATE -fsanitize=address,undefined
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/loader.map")
set_target_properties(cflow_probe_loader PROPERTIES
  BUILD_RPATH "${_prefix}/lib")
