cmake_minimum_required(VERSION 3.20)
project(SaltsCryptoInstalledConsumer LANGUAGES C CXX)

if(NOT DEFINED SALTS_TEST_VERSION OR SALTS_TEST_VERSION STREQUAL "")
  message(FATAL_ERROR "SALTS_TEST_VERSION is required")
endif()

find_package(Salts ${SALTS_TEST_VERSION} EXACT CONFIG REQUIRED)

enable_testing()

add_executable(crypto_consumer_c main.c)
target_link_libraries(crypto_consumer_c PRIVATE Salts::Core)
set_target_properties(crypto_consumer_c PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME crypto_consumer_c COMMAND crypto_consumer_c)

add_executable(crypto_consumer_cpp main.cpp)
target_link_libraries(crypto_consumer_cpp PRIVATE Salts::Core)
set_target_properties(crypto_consumer_cpp PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
add_test(NAME crypto_consumer_cpp COMMAND crypto_consumer_cpp)
