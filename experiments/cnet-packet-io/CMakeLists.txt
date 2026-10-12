cmake_minimum_required(VERSION 3.20)
project(cnet_packet_io_research VERSION 0.1 LANGUAGES C)
include(CTest)
find_package(Threads REQUIRED)

add_library(packet_io_research STATIC packet_io.c)
target_compile_features(packet_io_research PUBLIC c_std_11)
set_target_properties(packet_io_research PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(packet_io_research PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(packet_io_research PUBLIC Threads::Threads)

# Independent, bounded, experimental reliable-message engine: no public ABI.
add_library(packet_reliable_research STATIC reliable.c)
target_compile_features(packet_reliable_research PUBLIC c_std_11)
set_target_properties(packet_reliable_research PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(packet_reliable_research PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(packet_reliable_research PUBLIC Threads::Threads)

# Shared bounded Owner/Generation lease: the only MAC lifecycle authority for
# the GmSSL source baseline and installed Salts::Core ABI adapter.
add_library(packet_mac_lease STATIC mac_lease.c)
target_compile_features(packet_mac_lease PUBLIC c_std_11)
target_link_libraries(packet_mac_lease PUBLIC packet_reliable_research)

if(BUILD_TESTING)
  # Testing-only real HMAC-SHA256 provider. Production Salts/CNet acquires NO
  # OpenSSL dependency; the protocol engine accepts an explicit MAC provider.
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  add_library(packet_test_hmac STATIC tests/test_mac.c)
  target_link_libraries(packet_test_hmac PUBLIC OpenSSL::Crypto)
  target_compile_features(packet_test_hmac PUBLIC c_std_11)
  add_executable(packet_reliable_test tests/reliable_test.c)
  target_link_libraries(packet_reliable_test PRIVATE packet_reliable_research packet_test_hmac)
  set_target_properties(packet_reliable_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME packet_reliable_conformance COMMAND packet_reliable_test)
  set_tests_properties(packet_reliable_conformance PROPERTIES TIMEOUT 30)

  add_executable(packet_reliable_packet_io_test tests/reliable_packet_io_test.c)
  target_link_libraries(packet_reliable_packet_io_test PRIVATE
    packet_reliable_research packet_io_research packet_test_hmac)
  set_target_properties(packet_reliable_packet_io_test
    PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME packet_reliable_packet_io COMMAND packet_reliable_packet_io_test)
  set_tests_properties(packet_reliable_packet_io PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
  add_executable(packet_io_fake_test tests/fake_packet_io_test.c)
  target_link_libraries(packet_io_fake_test PRIVATE packet_io_research)
  set_target_properties(packet_io_fake_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME packet_io_fake_owner COMMAND packet_io_fake_test)
  set_tests_properties(packet_io_fake_owner PROPERTIES TIMEOUT 20)
endif()

option(PACKET_IO_ENABLE_DPDK "Build optional Linux DPDK net_ring smoke" OFF)
if(PACKET_IO_ENABLE_DPDK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "DPDK Packet IO is experimental and Linux-only")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
  # libdpdk.pc describes core libraries; ring PMD helper symbols reside in the
  # separate rte_net_ring driver library on distro shared-DPDK packages.
  find_library(PACKET_IO_RING_PMD_LIBRARY NAMES rte_net_ring REQUIRED)
  message(STATUS "Experimental DPDK ${DPDK_VERSION}, ring PMD ${PACKET_IO_RING_PMD_LIBRARY}")
  add_executable(packet_io_dpdk_ring_test tests/dpdk_ring_test.c)
  target_link_libraries(packet_io_dpdk_ring_test PRIVATE packet_io_research
    PkgConfig::DPDK "${PACKET_IO_RING_PMD_LIBRARY}")
  target_compile_features(packet_io_dpdk_ring_test PRIVATE c_std_11)
  if(BUILD_TESTING)
    add_test(NAME packet_io_dpdk_ring COMMAND packet_io_dpdk_ring_test)
    set_tests_properties(packet_io_dpdk_ring PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
  endif()
endif()

# Standalone *explicitly selected* native GmSSL R2 provider research.
# Never link the provider or its system package into the installed Salts::CNet
# target, NativeIO, or the default Packet I/O conformance build.
option(PACKET_IO_ENABLE_GMSSL "Build opt-in pinned GmSSL R2 MAC conformance" OFF)
if(PACKET_IO_ENABLE_GMSSL)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Research GmSSL R2 MAC qualification is Linux-only")
  endif()
  if(NOT BUILD_TESTING)
    message(FATAL_ERROR "Research GmSSL R2 MAC requires BUILD_TESTING=ON")
  endif()
  find_package(GmSSL CONFIG REQUIRED)
  if(NOT TARGET GmSSL::GmSSL)
    message(FATAL_ERROR "GmSSL package lacks the required GmSSL::GmSSL target")
  endif()

  add_library(packet_gmssl_mac_research STATIC gmssl_mac.c)
  target_compile_features(packet_gmssl_mac_research PRIVATE c_std_11)
  target_link_libraries(packet_gmssl_mac_research PUBLIC packet_mac_lease
    PRIVATE GmSSL::GmSSL)
  set_target_properties(packet_gmssl_mac_research PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

  add_executable(packet_gmssl_mac_test tests/mac_provider_test.c)
  target_compile_features(packet_gmssl_mac_test PRIVATE c_std_11)
  target_link_libraries(packet_gmssl_mac_test PRIVATE
    packet_gmssl_mac_research GmSSL::GmSSL packet_test_hmac)
  set_target_properties(packet_gmssl_mac_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME packet_gmssl_mac_conformance COMMAND packet_gmssl_mac_test)
  set_tests_properties(packet_gmssl_mac_conformance PROPERTIES TIMEOUT 30)
endif()

# Explicit exact-SDK consumer. This target uses Salts::Core (GmSSL stays
# private inside the released SDK) and never reuses the direct-GmSSL target.
option(PACKET_IO_ENABLE_CMETA_SDK "Build opt-in exact Salts.Native 2.3 SDK MAC consumer" OFF)
if(PACKET_IO_ENABLE_CMETA_SDK)
  if(NOT BUILD_TESTING)
    message(FATAL_ERROR "Exact Salts.Native SDK consumer requires BUILD_TESTING=ON")
  endif()
  find_package(Salts 2.3.0 EXACT CONFIG REQUIRED)
  if(NOT TARGET Salts::Core)
    message(FATAL_ERROR "Exact Salts 2.3 SDK does not export Salts::Core")
  endif()
  add_library(packet_cmeta_mac_research STATIC cmeta_mac.c)
  target_compile_features(packet_cmeta_mac_research PUBLIC c_std_11)
  target_link_libraries(packet_cmeta_mac_research PUBLIC packet_mac_lease
    PRIVATE Salts::Core)
  set_target_properties(packet_cmeta_mac_research PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_executable(packet_cmeta_sdk_test tests/mac_provider_test.c)
  target_compile_definitions(packet_cmeta_sdk_test PRIVATE RL_TEST_CMETA_SDK=1)
  target_link_libraries(packet_cmeta_sdk_test PRIVATE
    packet_cmeta_mac_research Salts::Core packet_test_hmac)
  set_target_properties(packet_cmeta_sdk_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME packet_cmeta_sdk_conformance COMMAND packet_cmeta_sdk_test)
  set_tests_properties(packet_cmeta_sdk_conformance PROPERTIES TIMEOUT 30)
  # The exact same exported C API must also be consumable by C++17.
  enable_language(CXX)
  add_executable(packet_cmeta_sdk_cpp_test tests/cmeta_sdk_cpp.cpp)
  target_link_libraries(packet_cmeta_sdk_cpp_test PRIVATE Salts::Core)
  set_target_properties(packet_cmeta_sdk_cpp_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME packet_cmeta_sdk_cpp COMMAND packet_cmeta_sdk_cpp_test)
  set_tests_properties(packet_cmeta_sdk_cpp PROPERTIES TIMEOUT 30)
endif()
