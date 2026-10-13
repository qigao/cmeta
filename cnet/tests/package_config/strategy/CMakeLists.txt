cmake_minimum_required(VERSION 3.20)
project(cnet_installed_sdk_consumer LANGUAGES C CXX)
if(NOT DEFINED SALTS_TEST_PREFIX)
  message(FATAL_ERROR "installed Salts SDK prefix required")
endif()
# Use only the installed SDK export: no build-tree links and no EXACT pins.
find_package(Salts CONFIG REQUIRED
  PATHS "${SALTS_TEST_PREFIX}" NO_DEFAULT_PATH)
if(NOT TARGET Salts::CNet OR NOT TARGET Salts::NativeIO)
  message(FATAL_ERROR "installed SDK missing canonical CNet/NativeIO targets")
endif()
if(TARGET Salts::CNetManager)
  message(FATAL_ERROR "legacy separate Manager DSO is forbidden")
endif()
get_target_property(_cnet_type Salts::CNet TYPE)
if(NOT _cnet_type STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR "Salts::CNet must be one shared target, got ${_cnet_type}")
endif()
add_executable(cnet_installed_c11 main.c)
add_executable(cnet_installed_cpp17 main.cpp)
target_compile_features(cnet_installed_c11 PRIVATE c_std_11)
target_compile_features(cnet_installed_cpp17 PRIVATE cxx_std_17)
set_target_properties(cnet_installed_c11 PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cnet_installed_cpp17 PROPERTIES
  CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
foreach(_t IN ITEMS cnet_installed_c11 cnet_installed_cpp17)
  target_link_libraries(${_t} PRIVATE Salts::CNet Salts::NativeIO)
endforeach()
enable_testing()
add_test(NAME cnet_installed_c11 COMMAND cnet_installed_c11)
add_test(NAME cnet_installed_cpp17 COMMAND cnet_installed_cpp17)

# #1196: consume only the installed Salts C11 headers and CNet DSO, with no
# provider-private GmSSL headers or direct build-tree linkage. Copy public
# test-only X.509 data into the independent consumer build directory.
add_executable(cnet_installed_p384_tls p384_tls.c)
target_compile_features(cnet_installed_p384_tls PRIVATE c_std_11)
set_target_properties(cnet_installed_p384_tls PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(cnet_installed_p384_tls PRIVATE Salts::CNet Salts::NativeIO)
set(_p384_fixtures
  p384-test-root-cert.pem
  p384-test-server-cert.pem
  p384-test-server-key.pem
  p384-test-wrong-root-cert.pem)
foreach(_fixture IN LISTS _p384_fixtures)
  configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../fixtures/${_fixture}"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/${_fixture}" COPYONLY)
endforeach()
add_test(NAME cnet_installed_p384_tls
  COMMAND cnet_installed_p384_tls
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-root-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-server-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-server-key.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-wrong-root-cert.pem")
set_tests_properties(cnet_installed_p384_tls PROPERTIES TIMEOUT 90)

# Unlike header-only consumers this TLS executable pulls in Salts::Core through
# the installed CNet DSO. ELF DT_RUNPATH does not resolve indirect DSO deps;
# expose ONLY the just-installed SDK lib directory in this test subprocess.
# Do not inherit a caller LD_LIBRARY_PATH: an empty trailing search entry
# could resolve untrusted libraries from the test working directory.
if(UNIX AND NOT APPLE)
  set_tests_properties(cnet_installed_p384_tls PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${SALTS_TEST_PREFIX}/lib")
endif()
