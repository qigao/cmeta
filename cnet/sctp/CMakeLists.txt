cmake_minimum_required(VERSION 3.20)
project(CNetSctpPrototype VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# An isolated, C-only research target, not the production Salts::CNet library.
add_library(cnet_sctp_wire STATIC src/sctp_wire.c)
add_library(CNetSctpPrototype::Wire ALIAS cnet_sctp_wire)

# M2 research only: not part of installed C11 ABI or Salts targets.
add_library(cnet_sctp_assoc_proto STATIC
  src/sctp_assoc.c src/sctp_init_params.c src/sctp_collision.c)

# M2/M3 DATA sub-engine: independent static C11 target, not installed/exported.
add_library(cnet_sctp_data_proto STATIC src/sctp_data.c)

# RFC 8832 DCEP codec is private until PR-SCTP, complete stream reset and full
# peer interoperability qualify a complete public WebRTC DataChannel API.
add_library(cnet_sctp_dcep_proto STATIC src/sctp_dcep.c)

# Stateless RFC8831 PPID 51/53/56/57 adapter, no external transport or SDK.
add_library(cnet_sctp_webrtc_proto STATIC src/sctp_webrtc.c)
target_include_directories(cnet_sctp_webrtc_proto PRIVATE src)
set_target_properties(cnet_sctp_webrtc_proto PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(cnet_sctp_webrtc_proto PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_webrtc_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_include_directories(cnet_sctp_dcep_proto PRIVATE src)
set_target_properties(cnet_sctp_dcep_proto PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(cnet_sctp_dcep_proto PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_dcep_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_link_libraries(cnet_sctp_data_proto PUBLIC cnet_sctp_wire)
# Existing private Association now admits DATA/SACK only after Cookie ACK.
target_link_libraries(cnet_sctp_assoc_proto PUBLIC cnet_sctp_data_proto)
target_include_directories(cnet_sctp_data_proto PRIVATE src)
set_target_properties(cnet_sctp_data_proto PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(cnet_sctp_data_proto PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_data_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_link_libraries(cnet_sctp_assoc_proto PUBLIC cnet_sctp_wire)
target_include_directories(cnet_sctp_assoc_proto PRIVATE src)
set_target_properties(cnet_sctp_assoc_proto PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(cnet_sctp_assoc_proto PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_assoc_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
set_target_properties(cnet_sctp_wire PROPERTIES
  EXPORT_NAME Wire
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(cnet_sctp_wire PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(MSVC)
  target_compile_options(cnet_sctp_wire PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_wire PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS cnet_sctp_wire EXPORT CNetSctpPrototypeTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/cnet/sctp_wire.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cnet)
configure_package_config_file(
  cmake/CNetSctpPrototypeConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CNetSctpPrototypeConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CNetSctpPrototype)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CNetSctpPrototypeConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CNetSctpPrototypeConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CNetSctpPrototypeConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CNetSctpPrototype)
install(EXPORT CNetSctpPrototypeTargets
  NAMESPACE CNetSctpPrototype::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CNetSctpPrototype)

if(BUILD_TESTING)
  add_executable(cnet_sctp_wire_test tests/wire_test.c)
  target_link_libraries(cnet_sctp_wire_test PRIVATE CNetSctpPrototype::Wire)
  set_target_properties(cnet_sctp_wire_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_wire_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_wire_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_wire_test COMMAND cnet_sctp_wire_test)

  add_executable(cnet_sctp_assoc_test tests/assoc_test.c)
  target_link_libraries(cnet_sctp_assoc_test PRIVATE cnet_sctp_assoc_proto)
  target_include_directories(cnet_sctp_assoc_test PRIVATE src)
  set_target_properties(cnet_sctp_assoc_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_assoc_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_assoc_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_assoc_test COMMAND cnet_sctp_assoc_test)

  add_executable(cnet_sctp_init_params_test tests/init_params_test.c)
  target_link_libraries(cnet_sctp_init_params_test PRIVATE cnet_sctp_assoc_proto)
  target_include_directories(cnet_sctp_init_params_test PRIVATE src)
  set_target_properties(cnet_sctp_init_params_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_init_params_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_init_params_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_init_params_test COMMAND cnet_sctp_init_params_test)

  # Table 12 classifiers are decision-only: no side effects or production cutover.
  add_executable(cnet_sctp_collision_test tests/collision_test.c)
  target_link_libraries(cnet_sctp_collision_test PRIVATE cnet_sctp_assoc_proto)
  target_include_directories(cnet_sctp_collision_test PRIVATE src)
  set_target_properties(cnet_sctp_collision_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_collision_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_collision_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_collision_test COMMAND cnet_sctp_collision_test)

  add_executable(cnet_sctp_data_test tests/data_test.c)
  target_link_libraries(cnet_sctp_data_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_data_test PRIVATE src)
  set_target_properties(cnet_sctp_data_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_data_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_data_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_data_test COMMAND cnet_sctp_data_test)

  add_executable(cnet_sctp_sack_recovery_test tests/sack_recovery_test.c)
  target_link_libraries(cnet_sctp_sack_recovery_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_sack_recovery_test PRIVATE src)
  set_target_properties(cnet_sctp_sack_recovery_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_sack_recovery_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_sack_recovery_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_sack_recovery_test COMMAND cnet_sctp_sack_recovery_test)

  add_executable(cnet_sctp_fast_recovery_test tests/fast_recovery_test.c)
  target_link_libraries(cnet_sctp_fast_recovery_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_fast_recovery_test PRIVATE src)
  set_target_properties(cnet_sctp_fast_recovery_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_fast_recovery_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_fast_recovery_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_fast_recovery_test COMMAND cnet_sctp_fast_recovery_test)

  add_executable(cnet_sctp_congestion_test tests/congestion_test.c)
  target_link_libraries(cnet_sctp_congestion_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_congestion_test PRIVATE src)
  set_target_properties(cnet_sctp_congestion_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_congestion_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_congestion_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_congestion_test COMMAND cnet_sctp_congestion_test)

  add_executable(cnet_sctp_zero_window_test tests/zero_window_test.c)
  target_link_libraries(cnet_sctp_zero_window_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_zero_window_test PRIVATE src)
  set_target_properties(cnet_sctp_zero_window_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_zero_window_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_zero_window_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_zero_window_test COMMAND cnet_sctp_zero_window_test)

  add_executable(cnet_sctp_pr_sctp_test tests/pr_sctp_test.c)
  target_link_libraries(cnet_sctp_pr_sctp_test PRIVATE
    cnet_sctp_data_proto cnet_sctp_wire)
  target_include_directories(cnet_sctp_pr_sctp_test PRIVATE src)
  set_target_properties(cnet_sctp_pr_sctp_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_pr_sctp_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_pr_sctp_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_pr_sctp_test COMMAND cnet_sctp_pr_sctp_test)

  add_executable(cnet_sctp_bundle_test tests/bundle_test.c)
  target_link_libraries(cnet_sctp_bundle_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_bundle_test PRIVATE src)
  set_target_properties(cnet_sctp_bundle_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_bundle_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_bundle_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_bundle_test COMMAND cnet_sctp_bundle_test)

  add_executable(cnet_sctp_fragment_test tests/fragment_test.c)
  target_link_libraries(cnet_sctp_fragment_test PRIVATE cnet_sctp_data_proto)
  target_include_directories(cnet_sctp_fragment_test PRIVATE src)
  set_target_properties(cnet_sctp_fragment_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_fragment_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_fragment_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_fragment_test COMMAND cnet_sctp_fragment_test)

  add_executable(cnet_sctp_assoc_data_test tests/assoc_data_test.c)
  target_link_libraries(cnet_sctp_assoc_data_test PRIVATE
    cnet_sctp_assoc_proto cnet_sctp_dcep_proto cnet_sctp_webrtc_proto)
  target_include_directories(cnet_sctp_assoc_data_test PRIVATE src)
  set_target_properties(cnet_sctp_assoc_data_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_assoc_data_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_assoc_data_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_assoc_data_test COMMAND cnet_sctp_assoc_data_test)

  add_executable(cnet_sctp_dcep_test tests/dcep_test.c)
  target_link_libraries(cnet_sctp_dcep_test PRIVATE cnet_sctp_dcep_proto)
  target_include_directories(cnet_sctp_dcep_test PRIVATE src)
  set_target_properties(cnet_sctp_dcep_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_dcep_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_dcep_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_dcep_test COMMAND cnet_sctp_dcep_test)


endif()

# M5: actual POSIX-thread handoff test with external single-Owner lease.
# Opt-in only. This test does not authorize unsynchronized concurrent API
# calls and never enters the installed Wire-only SDK.
option(CNET_SCTP_TEST_OWNER_THREADS "Build private POSIX Owner-handoff test" OFF)
if(CNET_SCTP_TEST_OWNER_THREADS)
  if(WIN32)
    message(FATAL_ERROR "Private Owner thread gate needs POSIX Threads")
  endif()
  if(NOT BUILD_TESTING)
    message(FATAL_ERROR "Owner thread gate requires BUILD_TESTING=ON")
  endif()
  find_package(Threads REQUIRED)
  add_executable(cnet_sctp_owner_thread_test tests/owner_thread_test.c)
  target_include_directories(cnet_sctp_owner_thread_test PRIVATE src)
  target_link_libraries(cnet_sctp_owner_thread_test PRIVATE
    cnet_sctp_assoc_proto Threads::Threads)
  set_target_properties(cnet_sctp_owner_thread_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(cnet_sctp_owner_thread_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME cnet_sctp_owner_thread_test
    COMMAND cnet_sctp_owner_thread_test)
  # Actual dlopen/dlclose validation is POSIX only and never installed.
  # Keep the provider and host as independently compiled C11 code: a future
  # DSO unload must wait until the last borrowed callback has settled.
  add_library(cnet_sctp_test_provider_dso MODULE
    tests/provider_dso_fixture.c)
  target_include_directories(cnet_sctp_test_provider_dso PRIVATE tests)
  target_link_libraries(cnet_sctp_test_provider_dso PRIVATE Threads::Threads)
  set_target_properties(cnet_sctp_test_provider_dso PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(cnet_sctp_test_provider_dso PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

  add_executable(cnet_sctp_provider_dso_test tests/provider_dso_test.c)
  target_include_directories(cnet_sctp_provider_dso_test PRIVATE src tests)
  target_link_libraries(cnet_sctp_provider_dso_test PRIVATE
    cnet_sctp_assoc_proto Threads::Threads ${CMAKE_DL_LIBS})
  set_target_properties(cnet_sctp_provider_dso_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(cnet_sctp_provider_dso_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_dependencies(cnet_sctp_provider_dso_test
    cnet_sctp_test_provider_dso)
  add_test(NAME cnet_sctp_provider_dso_test
    COMMAND cnet_sctp_provider_dso_test
      $<TARGET_FILE:cnet_sctp_test_provider_dso>)
endif()

# Optional cryptographic research gate. Its private C11 sources and GmSSL
# dependency MUST NOT enter the installed CNetSctpPrototype::Wire package.
# Standalone Wire/Association CI does not require GmSSL or a C++ runtime.
option(CNET_SCTP_TEST_GMSSL "Build separate GmSSL-backed M2 cookie MAC tests" OFF)
if(CNET_SCTP_TEST_GMSSL)
  if(NOT BUILD_TESTING)
    message(FATAL_ERROR "CNET_SCTP_TEST_GMSSL requires BUILD_TESTING=ON")
  endif()
  find_package(GmSSL CONFIG REQUIRED)
  add_library(cnet_sctp_gmssl_mac_proto STATIC src/sctp_gmssl_mac.c)
  target_include_directories(cnet_sctp_gmssl_mac_proto PRIVATE src)
  target_link_libraries(cnet_sctp_gmssl_mac_proto PRIVATE cnet_sctp_wire GmSSL::GmSSL)
  set_target_properties(cnet_sctp_gmssl_mac_proto PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_executable(cnet_sctp_gmssl_mac_test tests/assoc_gmssl_test.c)
  target_include_directories(cnet_sctp_gmssl_mac_test PRIVATE src)
  target_link_libraries(cnet_sctp_gmssl_mac_test PRIVATE
    cnet_sctp_assoc_proto cnet_sctp_gmssl_mac_proto GmSSL::GmSSL)
  set_target_properties(cnet_sctp_gmssl_mac_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(cnet_sctp_gmssl_mac_proto PRIVATE /W4 /WX)
    target_compile_options(cnet_sctp_gmssl_mac_test PRIVATE /W4 /WX)
  else()
    target_compile_options(cnet_sctp_gmssl_mac_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(cnet_sctp_gmssl_mac_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cnet_sctp_gmssl_mac_test COMMAND cnet_sctp_gmssl_mac_test)
endif()
