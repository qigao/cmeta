cmake_minimum_required(VERSION 3.25)
project(SaltsCstlNativeInstalledConsumer LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Darwin)$")
  message(FATAL_ERROR "CSTL native DSO consumer requires Windows or macOS")
endif()
if(NOT DEFINED EXACT_SALTS_PREFIX OR
   NOT IS_DIRECTORY "${EXACT_SALTS_PREFIX}")
  message(FATAL_ERROR "A specific relocated installed Salts SDK is required")
endif()
if(NOT DEFINED EXACT_CORE_LIBRARY OR
   NOT EXISTS "${EXACT_CORE_LIBRARY}")
  message(FATAL_ERROR "The exact installed native Salts::Core library is required")
endif()
if(NOT EXACT_CSTL_MODE STREQUAL "hidden" AND
   NOT EXACT_CSTL_MODE STREQUAL "shared")
  message(FATAL_ERROR "EXACT_CSTL_MODE must be hidden or shared")
endif()
file(REAL_PATH "${EXACT_SALTS_PREFIX}" _prefix)
find_package(Salts CONFIG REQUIRED PATHS "${_prefix}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::CSTL OR NOT TARGET Salts::CMeta)
  message(FATAL_ERROR "Installed SDK does not export public CSTL/CMeta targets")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

foreach(target IN ITEMS cstl_provider_a cstl_provider_b)
  add_library(${target} SHARED provider.c)
  target_link_libraries(${target} PRIVATE Salts::CSTL Salts::CMeta)
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${_prefix}/lib"
    INSTALL_RPATH "${_prefix}/lib")
endforeach()

add_executable(cstl_native_loader loader.c)
# The loader intentionally does not LINK Salts::Core/CMeta: it obtains its
# code via LoadLibrary/dlopen at runtime. Headers must still come exclusively
# from the relocated, exact installed SDK, never the in-tree source checkout.
target_include_directories(cstl_native_loader PRIVATE "${_prefix}/include")
target_link_libraries(cstl_native_loader PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(cstl_native_loader PROPERTIES
  BUILD_RPATH "${_prefix}/lib")

add_executable(cstl_cpp17_consumer consumer_cpp17.cpp)
target_link_libraries(cstl_cpp17_consumer PRIVATE Salts::CSTL Salts::CMeta)
set_target_properties(cstl_cpp17_consumer PROPERTIES
  BUILD_RPATH "${_prefix}/lib")

enable_testing()
add_test(NAME cstl_native_cross_provider
  COMMAND "$<TARGET_FILE:cstl_native_loader>"
    "${EXACT_CORE_LIBRARY}"
    "$<TARGET_FILE:cstl_provider_a>"
    "$<TARGET_FILE:cstl_provider_b>"
    "${EXACT_CSTL_MODE}")
add_test(NAME cstl_cpp17_consumer COMMAND "$<TARGET_FILE:cstl_cpp17_consumer>")
set_tests_properties(cstl_native_cross_provider cstl_cpp17_consumer PROPERTIES
  TIMEOUT 60)
