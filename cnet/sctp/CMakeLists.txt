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
