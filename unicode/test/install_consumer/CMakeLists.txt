cmake_minimum_required(VERSION 3.25)
project(SaltsUnicodeInstalledConsumers LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{SALTS_ROOT} OR NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT must name the installed SDK under test")
endif()
unset(Salts_DIR CACHE)
unset(Salts_DIR)
find_package(Salts CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::Unicode)
  message(FATAL_ERROR "Installed Salts SDK does not export Salts::Unicode")
endif()

set_property(DIRECTORY APPEND PROPERTY COMPILE_OPTIONS "${SALTS_UNICODE_TEST_COMPILE_OPTIONS}")
set_property(DIRECTORY APPEND PROPERTY LINK_OPTIONS "${SALTS_UNICODE_TEST_LINK_OPTIONS}")

add_executable(salts_unicode_installed_c11 main.c)
# The Unicode ABI test must explicitly resolve Unicode's own public export,
# not pass only because Salts::IDNA happens to depend on that target.
target_link_libraries(salts_unicode_installed_c11 PRIVATE Salts::Unicode Salts::IDNA)
add_executable(salts_unicode_installed_cpp17 main.cpp)
target_link_libraries(salts_unicode_installed_cpp17 PRIVATE Salts::Unicode Salts::IDNA)

enable_testing()
add_test(NAME salts_unicode_installed_c11 COMMAND salts_unicode_installed_c11)
add_test(NAME salts_unicode_installed_cpp17 COMMAND salts_unicode_installed_cpp17)
