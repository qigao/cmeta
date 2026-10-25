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

# #1196/#1222: installed-SDK consumer for the public C11 CNet TLS API.
# The test uses only installed Salts targets and public headers; it must not
# depend on private GmSSL headers, build-tree libraries, or provider fallback.
add_executable(cnet_installed_p384_tls p384_tls.c)
target_compile_features(cnet_installed_p384_tls PRIVATE c_std_11)
set_target_properties(cnet_installed_p384_tls PROPERTIES
  C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(cnet_installed_p384_tls PRIVATE Salts::CNet Salts::NativeIO)
set(_x509_fixtures
  p384-test-root-cert.pem
  p384-test-server-cert.pem
  p384-test-server-key.pem
  p384-test-wrong-root-cert.pem
  p384-test-expired-cert.pem
  p384-test-tampered-cert.pem
  p256-test-server-key.pem
  p384-chain-root-cert.pem
  p384-chain-intermediate-cert.pem
  p384-chain-server-cert.pem
  p384-chain-server-key.pem
  invalid-ca-root-cert.pem
  invalid-ca-issuer-cert.pem
  invalid-ca-server-cert.pem
  invalid-ca-server-key.pem
  unsupported-ed25519-server-cert.pem
  unsupported-ed25519-server-key.pem
  rsa3072-test-root-cert.pem
  rsa3072-test-server-cert.pem
  rsa3072-test-server-key.pem
  rsa3072-test-server-pss-cert.pem)
foreach(_fixture IN LISTS _x509_fixtures)
  configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../fixtures/${_fixture}"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/${_fixture}" COPYONLY)
endforeach()
file(READ
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-server-cert.pem"
  _p384_chain_leaf)
file(READ
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-intermediate-cert.pem"
  _p384_chain_intermediate)
file(WRITE
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-server-fullchain.pem"
  "${_p384_chain_leaf}${_p384_chain_intermediate}")
file(READ
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/invalid-ca-server-cert.pem"
  _invalid_ca_leaf)
file(READ
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/invalid-ca-issuer-cert.pem"
  _invalid_ca_issuer)
file(WRITE
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/invalid-ca-server-fullchain.pem"
  "${_invalid_ca_leaf}${_invalid_ca_issuer}")
file(WRITE
  "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/malformed-server-cert.pem"
  "-----BEGIN CERTIFICATE-----\nMALFORMED-DER\n-----END CERTIFICATE-----\n")
unset(_p384_chain_leaf)
unset(_p384_chain_intermediate)
unset(_invalid_ca_leaf)
unset(_invalid_ca_issuer)
add_test(NAME cnet_installed_p384_tls
  COMMAND cnet_installed_p384_tls
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-root-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-server-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-server-key.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-wrong-root-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-expired-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-test-tampered-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-root-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-server-fullchain.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-server-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p384-chain-server-key.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/malformed-server-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/p256-test-server-key.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/unsupported-ed25519-server-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/unsupported-ed25519-server-key.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/rsa3072-test-root-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/rsa3072-test-server-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/rsa3072-test-server-key.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/rsa3072-test-server-pss-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/invalid-ca-root-cert.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/invalid-ca-server-fullchain.pem"
    "${CMAKE_CURRENT_BINARY_DIR}/p384-fixtures/invalid-ca-server-key.pem")
set_tests_properties(cnet_installed_p384_tls PROPERTIES TIMEOUT 90)

# ELF DT_RUNPATH does not resolve indirect shared-library dependencies.
# Scope the test subprocess to the SDK's own lib directory, replacing rather
# than appending any caller-supplied LD_LIBRARY_PATH.
if(UNIX AND NOT APPLE)
  set_tests_properties(cnet_installed_p384_tls PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${SALTS_TEST_PREFIX}/lib")
endif()
