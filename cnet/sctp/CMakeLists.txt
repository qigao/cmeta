cmake_minimum_required(VERSION 3.20)
project(CNetSctpPrototype VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# An isolated, C-only research target, not the production Salts::CNet library.
add_library(cnet_sctp_wire STATIC src/sctp_wire.c)
add_library(CNetSctpPrototype::Wire ALIAS cnet_sctp_wire)
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
endif()
