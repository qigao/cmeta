cmake_minimum_required(VERSION 3.20)
project(cmeta_ace_patterns_installed LANGUAGES C CXX)
if(NOT DEFINED SALTS_TEST_PREFIX OR NOT DEFINED SALTS_TEST_VERSION)
  message(FATAL_ERROR "exact installed Salts prefix/version required")
endif()
find_package(CMeta ${SALTS_TEST_VERSION} EXACT CONFIG REQUIRED
  PATHS "${SALTS_TEST_PREFIX}" NO_DEFAULT_PATH)
foreach(target IN ITEMS CMeta::CMeta Salts::NativeIO Salts::Platform)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "installed SDK missing ${target}")
  endif()
endforeach()

add_executable(ace_patterns_installed_c consumer.c)
target_link_libraries(ace_patterns_installed_c PRIVATE
  CMeta::CMeta Salts::NativeIO Salts::Platform)
set_target_properties(ace_patterns_installed_c PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  target_compile_options(ace_patterns_installed_c PRIVATE
    -Werror=implicit-function-declaration)
endif()

add_executable(ace_patterns_installed_cpp consumer.cpp)
target_link_libraries(ace_patterns_installed_cpp PRIVATE
  CMeta::CMeta Salts::NativeIO Salts::Platform)
set_target_properties(ace_patterns_installed_cpp PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  target_compile_options(ace_patterns_installed_cpp PRIVATE
    -Werror=old-style-cast)
endif()

enable_testing()
add_test(NAME ace_patterns_installed_c COMMAND ace_patterns_installed_c)
add_test(NAME ace_patterns_installed_cpp COMMAND ace_patterns_installed_cpp)
