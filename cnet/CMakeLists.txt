set(TARGET_NAME cnet)

find_package(c-ares CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(GmSSL CONFIG REQUIRED)
if(APPLE)
  find_library(CNET_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(CNET_SECURITY_FRAMEWORK Security REQUIRED)
endif()

get_target_property(cnet_cares_configurations c-ares::cares
                    IMPORTED_CONFIGURATIONS)

set(CNET_SOURCE_FILES
  src/cnet_client.c
  src/cnet_command.c
  src/cnet_datagram.c
  src/cnet_kcp.c
  src/cnet_kcp_fec.c
  src/cnet_kcp_secure.c
  src/cnet_secure_kcp.c
  src/cnet_packet_endpoint.c
  src/cnet_dispatcher.c
  src/cnet_event.c
  src/cnet_listener.c
  src/cnet_module.c
  src/cnet_owner.c
  src/cnet_owner_strategy.c
  src/cnet_destination_policy.c
  src/cnet_client_pool.c
  src/cnet_recovery_policy.c
  src/cnet_managed_dial.c
  src/cnet_sg_host.c
  src/cnet_resolver.c
  src/cnet_session.c
  src/cnet_shards.c
  src/cnet_tls.c
  src/cnet_transport.c
  src/cnet_transport_pipe.c
  src/cnet_transport_ipc.c
  src/cnet_ipc.c
  src/cnet_transport_vsock.c
  src/cnet_uri.c
  src/cnet_write_queue.c
  src/cnet_websocket.c
  $<TARGET_OBJECTS:cmeta_wsparser>)

function(cnet_configure_target target_name)
  target_compile_features(${target_name} PUBLIC c_std_11)
  target_include_directories(
    ${target_name}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tools/wsparser
      ${CMAKE_SOURCE_DIR}/native-io/src)
  target_link_libraries(
    ${target_name}
    PUBLIC Salts::NativeIO Salts::Concurrency Salts::Core
    PRIVATE Salts::UriParser Salts::Platform Salts::CSTL monocypher cmeta_reed
            ${cnet_cares_link_items} kcp::kcp c-ares::cares
            GmSSL::GmSSL
            $<$<PLATFORM_ID:Windows>:crypt32>
            "$<$<PLATFORM_ID:Darwin>:-framework CoreFoundation>"
            "$<$<PLATFORM_ID:Darwin>:-framework Security>"
            "$<$<PLATFORM_ID:iOS>:-framework CoreFoundation>"
            "$<$<PLATFORM_ID:iOS>:-framework Security>")
endfunction()

add_library(${TARGET_NAME} SHARED ${CNET_SOURCE_FILES})
# #1220 diagnostic branch ONLY: failed-drain read-only state snapshot.
# This define must never enter the official CNet shared-library build.
target_compile_definitions(${TARGET_NAME} PRIVATE CNET_VSOCK_STOP_DIAGNOSTIC=1)
# Manager and inbound admission handoff share CNet's single DSO identity.
# Do not add them to CNET_SOURCE_FILES: profile archives recompile that list.
target_sources(${TARGET_NAME} PRIVATE
  src/cnet_manager.c
  src/cnet_handoff.c)
cmake_config_target(${TARGET_NAME}
  VERSION "${PROJECT_VERSION}" SOVERSION "${SALTS_NATIVE_SOVERSION}"
  ALIAS Salts::CNet
  FOLDER "cnet"
  EXPORT_NAME CNet)
set_target_properties(${TARGET_NAME} PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON)
cnet_configure_target(${TARGET_NAME})

function(cnet_add_profile_target target_name)
  # Keep diagnostic instrumentation out of the installed shared library.
  add_library(${target_name} STATIC ${CNET_SOURCE_FILES})
  cnet_configure_target(${target_name})
  target_compile_definitions(
    ${target_name}
    PUBLIC CNET_INTERNAL_PROFILING=1 CNET_INTERNAL_MULTI_OWNER_POC=1)
  set_target_properties(${target_name} PROPERTIES FOLDER "cnet/diagnostics")
endfunction()

if(BUILD_BENCHMARKS OR CNET_BUILD_BENCHMARKS)
  cnet_add_profile_target(cnet_profile)
endif()
if(BUILD_TESTS)
  # Tests and benchmarks coexist in CI; fault injection must not affect timings.
  cnet_add_profile_target(cnet_test_profile)
  target_compile_definitions(cnet_test_profile PUBLIC CNET_INTERNAL_TESTING=1)
endif()

install(TARGETS ${TARGET_NAME} EXPORT SaltsTargets)
if(WIN32)
  install(FILES "$<TARGET_FILE:c-ares::cares>" TYPE BIN)
endif()
install(FILES "${c-ares_DIR}/copyright"
        DESTINATION share/licenses/Salts/c-ares)
install(FILES "${kcp_DIR}/copyright"
        DESTINATION share/licenses/Salts/kcp)
install(FILES "${CMAKE_SOURCE_DIR}/vendor/reed/LICENSE"
        DESTINATION share/licenses/Salts/reed)
install(
  DIRECTORY include/cnet
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS OR CNET_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
