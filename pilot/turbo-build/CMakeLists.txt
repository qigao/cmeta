cmake_minimum_required(VERSION 3.20)
project(salts_turbo_build_probe LANGUAGES C)

add_executable(salts-turbo-build-probe probe.c)
target_compile_features(salts-turbo-build-probe PRIVATE c_std_11)
set_target_properties(salts-turbo-build-probe PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
)
target_compile_options(salts-turbo-build-probe PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
)
