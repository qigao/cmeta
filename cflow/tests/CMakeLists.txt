set(CFLOW_TEST_SUPPORT
  cflow_test_ops.c
  cflow_test_ops.h)

cmake_add_test(cflow_graph_test
  SOURCES
    cflow_graph_test.c
    ${CFLOW_TEST_SUPPORT}
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_dense_successor_index_test
  SOURCES cflow_dense_successor_index_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_branching_views_test
  SOURCES
    cflow_branching_views_test.c
    ../benchmarks/cflow_branching_views.c
  LIBS CMeta::CFlow Salts::CSTL Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks
  FOLDER "cflow/tests")

cmake_add_test(cflow_pipeline_test
  SOURCES
    cflow_pipeline_test.c
    ${CFLOW_TEST_SUPPORT}
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

# ACE Pipes/Filters: canonical reflected publisher/subscriber + CMeta filter/map
# over the real demand-driven CFlow Graph and bounded Channel implementation.
cmake_add_test(cflow_ace_pipes_filters_test
  SOURCES cflow_ace_pipes_filters_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")
set_target_properties(cflow_ace_pipes_filters_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

cmake_add_test(cflow_plan_managed_test
  SOURCES cflow_plan_managed_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_stream_slice_test
  SOURCES cflow_stream_slice_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_stream_count_test
  SOURCES cflow_stream_count_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_stream_execution_test
  SOURCES cflow_stream_execution_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_adapter_lifecycle_test
  SOURCES cflow_adapter_lifecycle_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_stream_terminal_test
  SOURCES cflow_stream_terminal_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_status_test
  SOURCES cflow_status_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_backend_test
  SOURCES cflow_backend_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_reactive_test
  SOURCES
    cflow_reactive_test.c
    ${CFLOW_TEST_SUPPORT}
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_interface_reflection_test
  SOURCES cflow_interface_reflection_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_function_projection_test
  SOURCES cflow_function_projection_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_tinymock_interface_test
  SOURCES cflow_tinymock_interface_test.c
  LIBS CMeta::CFlow Salts::TinyMock
  FOLDER "cflow/tests")

cmake_add_test(cflow_readiness_test
  SOURCES
    cflow_readiness_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/tests/readiness_fake_backend.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/tests
  FOLDER "cflow/tests")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   SALTS_ENABLE_EPOLL_READINESS)
  target_compile_definitions(cflow_readiness_test PRIVATE
    CFLOW_TEST_EPOLL_READINESS=1)
endif()

cmake_add_test(cflow_event_mailbox_test
  SOURCES cflow_event_mailbox_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_machine_test
  SOURCES cflow_machine_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_machine_hierarchy_test
  SOURCES cflow_machine_hierarchy_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_statechart_test
  SOURCES cflow_statechart_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_statechart_instance_test
  SOURCES cflow_statechart_instance_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_statechart_instance_adapter_test
  SOURCES cflow_statechart_instance_adapter_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_statechart_hierarchy_adapter_test
  SOURCES cflow_statechart_hierarchy_adapter_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_machine_instance_test
  SOURCES cflow_machine_instance_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_actor_test
  SOURCES cflow_actor_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

# ACE #1025 real TCP -> bounded CFlow Channel -> Subscription -> Actor.
# Keep its full production API conformance independently from the retired
# CFlowCNet adapter target. Both libraries have separate authoritative owners.
cmake_add_test(cflow_ace_half_sync_async_test
  SOURCES cflow_ace_half_sync_async_test.c
  LIBS Salts::CNet CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")
set_target_properties(cflow_ace_half_sync_async_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(WIN32)
  target_link_libraries(cflow_ace_half_sync_async_test PRIVATE ws2_32)
endif()

# Real installed-SDK test; use the shared CMake consumer harness without
# linking against in-tree CMeta/CFlow targets or a duplicate test wrapper.
add_test(NAME cflow_ace_active_object_installed_test
  COMMAND "${CMAKE_COMMAND}"
    "-DSALTS_BINARY_DIR=${CMAKE_BINARY_DIR}"

    "-DSALTS_TEST_CONFIG=$<CONFIG>"
    "-DSALTS_GENERATOR=${CMAKE_GENERATOR}"
    "-DSALTS_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DSALTS_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DSALTS_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_config/ace_actor"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-ace-actor"
    "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
    -P "${PROJECT_SOURCE_DIR}/cmake/ci/verify-installed-consumer.cmake")
set_tests_properties(cflow_ace_active_object_installed_test PROPERTIES
  TIMEOUT 300 RUN_SERIAL TRUE)

cmake_add_test(cflow_statechart_actor_test
  SOURCES cflow_statechart_actor_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_io_actor_test
  SOURCES cflow_io_actor_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_io_publisher_test
  SOURCES cflow_io_publisher_test.c cflow_io_publisher_init_failure_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_io_native_test
  SOURCES cflow_io_native_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")
if(WIN32)
  target_link_libraries(cflow_io_native_test PRIVATE ws2_32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       SALTS_ENABLE_EPOLL_READINESS)
  target_compile_definitions(cflow_io_native_test PRIVATE
    CFLOW_TEST_NATIVE_EPOLL=1)
endif()

cmake_add_test(cflow_io_native_adapter_test
  SOURCES cflow_io_native_adapter_test.c
  LIBS CMeta::CFlow Salts::NativeIO Salts::Concurrency Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")



cmake_add_test(cflow_io_native_sharded_adapter_test
  SOURCES cflow_io_native_sharded_adapter_test.c
  LIBS CMeta::CFlow Salts::NativeIO Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")
if(WIN32)
  target_link_libraries(cflow_io_native_sharded_adapter_test PRIVATE ws2_32)
endif()

cmake_add_test(cflow_io_file_test
  SOURCES cflow_io_file_test.c cflow_io_file_init_failure_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_io_pipe_test
  SOURCES cflow_io_pipe_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_timer_event_test
  SOURCES cflow_timer_event_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_temporal_test
  SOURCES cflow_temporal_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_calculus_conformance_test
  SOURCES
    cflow_calculus_conformance_test.c
    ${CFLOW_TEST_SUPPORT}
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_direct_test
  SOURCES cflow_direct_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_parallel_reduce_test
  SOURCES cflow_parallel_reduce_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_seeded_reduce_test
  SOURCES cflow_seeded_reduce_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_certificate_test
  SOURCES cflow_certificate_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_execution_test
  SOURCES cflow_execution_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

cmake_add_test(cflow_owner_executor_test
  SOURCES cflow_owner_executor_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_owner_scheduler_test
  SOURCES cflow_owner_scheduler_test.c
  LIBS CMeta::CFlow Salts::Platform Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER "cflow/tests")

if(WIN32 AND NOT BUILD_SHARED_LIBS)
  add_library(cflow_executor_dso_fixture SHARED
    cflow_executor_dso_fixture.c
    cflow_executor_dso_fixture.h)
  target_link_libraries(cflow_executor_dso_fixture
    PRIVATE CMeta::CFlow)
  target_compile_definitions(cflow_executor_dso_fixture
    PRIVATE CFLOW_EXECUTOR_DSO_FIXTURE_BUILD=1)
  cmake_config_target(cflow_executor_dso_fixture FOLDER "cflow/tests"
    VERSION "${PROJECT_VERSION}" SOVERSION "${PROJECT_VERSION_MAJOR}")

  cmake_add_test(cflow_executor_dso_test
    SOURCES cflow_executor_dso_test.c
    LIBS cflow_executor_dso_fixture CMeta::CFlow Salts::TinyTest
    FOLDER "cflow/tests")
endif()

cmake_add_test(cflow_scheduler_compat_test
  SOURCES cflow_scheduler_compat_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_header_cpp_test
  SOURCES cflow_header_cpp_test.cpp
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

cmake_add_test(cflow_operator_policy_test
  SOURCES cflow_operator_policy_test.c
  LIBS CMeta::CFlow Salts::TinyTest
  FOLDER "cflow/tests")

set_target_properties(
  cflow_graph_test
  cflow_dense_successor_index_test
  cflow_branching_views_test
  cflow_pipeline_test
  cflow_stream_slice_test
  cflow_stream_count_test
  cflow_stream_execution_test
  cflow_adapter_lifecycle_test
  cflow_stream_terminal_test
  cflow_status_test
  cflow_backend_test
  cflow_reactive_test
  cflow_interface_reflection_test
  cflow_function_projection_test
  cflow_tinymock_interface_test
  cflow_readiness_test
  cflow_event_mailbox_test
  cflow_machine_test
  cflow_machine_hierarchy_test
  cflow_statechart_test
  cflow_statechart_instance_test
  cflow_statechart_instance_adapter_test
  cflow_statechart_hierarchy_adapter_test
  cflow_machine_instance_test
  cflow_actor_test
  cflow_owner_executor_test
  cflow_owner_scheduler_test
  cflow_statechart_actor_test
  cflow_io_actor_test
  cflow_io_publisher_test
  cflow_io_native_test
  cflow_io_native_adapter_test
  cflow_io_native_sharded_adapter_test
  cflow_io_file_test
  cflow_io_pipe_test
  cflow_timer_event_test
  cflow_temporal_test
  cflow_calculus_conformance_test
  cflow_direct_test
  cflow_parallel_reduce_test
  cflow_seeded_reduce_test
  cflow_certificate_test
  cflow_execution_test
  cflow_owner_executor_test
  cflow_owner_scheduler_test
  cflow_scheduler_compat_test
  cflow_operator_policy_test
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

set_target_properties(cflow_header_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
