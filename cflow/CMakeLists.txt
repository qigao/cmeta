set(TARGET_NAME cmeta_cflow)
set(CFLOW_LIBRARY_VERSION 5.0.0)
set(CFLOW_ABI_VERSION 5)

set(CFLOW_MACHINE_MAX_STATES 65536 CACHE STRING
    "Maximum state declarations accepted by one CFlow Machine")
set(CFLOW_MACHINE_MAX_EVENTS 65536 CACHE STRING
    "Maximum Event declarations accepted by one CFlow Machine")
set(CFLOW_MACHINE_MAX_GUARDS 65536 CACHE STRING
    "Maximum guard declarations accepted by one CFlow Machine")
set(CFLOW_MACHINE_MAX_ACTIONS 65536 CACHE STRING
    "Maximum action declarations accepted by one CFlow Machine")
set(CFLOW_MACHINE_MAX_TRANSITIONS 1048576 CACHE STRING
    "Maximum transitions accepted by one CFlow Machine")
set(CFLOW_STATECHART_MAX_ACTION_REFS 1048576 CACHE STRING
    "Maximum ordered action references accepted by one CFlow Statechart")
set(CFLOW_STATECHART_MAX_INSTANCE_BYTES 67108864 CACHE STRING
    "Maximum dynamically allocated bytes accepted by one Statechart instance")
set(CFLOW_STATECHART_ERROR_CAPACITY 512 CACHE STRING
    "Bytes reserved for the first owned Statechart instance error")
foreach(machine_limit IN ITEMS
    CFLOW_MACHINE_MAX_STATES
    CFLOW_MACHINE_MAX_EVENTS
    CFLOW_MACHINE_MAX_GUARDS
    CFLOW_MACHINE_MAX_ACTIONS
    CFLOW_MACHINE_MAX_TRANSITIONS
    CFLOW_STATECHART_MAX_ACTION_REFS)
  if(NOT "${${machine_limit}}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${machine_limit} must be a positive integer")
  endif()
  if(${machine_limit} GREATER 1048576)
    message(FATAL_ERROR
      "${machine_limit} must not exceed the verified ceiling 1048576")
  endif()
endforeach()
if(NOT "${CFLOW_STATECHART_MAX_INSTANCE_BYTES}" MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
    "CFLOW_STATECHART_MAX_INSTANCE_BYTES must be a positive integer")
endif()
if(CFLOW_STATECHART_MAX_INSTANCE_BYTES GREATER 1073741824)
  message(FATAL_ERROR
    "CFLOW_STATECHART_MAX_INSTANCE_BYTES must not exceed 1073741824")
endif()
if(NOT "${CFLOW_STATECHART_ERROR_CAPACITY}" MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
    "CFLOW_STATECHART_ERROR_CAPACITY must be a positive integer")
endif()
if(CFLOW_STATECHART_ERROR_CAPACITY GREATER 1048576)
  message(FATAL_ERROR
    "CFLOW_STATECHART_ERROR_CAPACITY must not exceed 1048576")
endif()

add_library(${TARGET_NAME}
  src/admission.c
  src/adapters.c
  src/actor.c
  src/backend.c
  src/certificate.c
  src/clock.c
  src/coord.c
  src/direct.c
  src/dense_successor_index.c
  src/effect.c
  src/event.c
  src/executor.c
  src/executor_owner.c
  src/function_projection.c
  src/graph.c
  src/interfaces.c
  src/io_actor.c
  src/io_native_adapter.c
  src/io_native_sharded_adapter.c
  src/io_publisher.c
  src/io_file.c
  src/io_pipe.c
  src/io_native.c
  src/lower.c
  src/machine.c
  src/machine_hierarchy.c
  src/machine_instance.c
  src/opt.c
  src/plan_compile.c
  src/plan_exec.c
  src/plan_parallel_reduce.c
  src/property.c
  src/readiness.c
  src/relation_exec.c
  src/subscription.c
  src/scheduler.c
  src/scheduler_owner.c
  src/scheduler_worker.c
  src/publishers.c
  src/statechart.c
  src/statechart_hierarchy_adapter.c
  src/statechart_instance.c
  src/status.c
  src/stream.c
  src/stream_execution.c
  src/subflow.c
  src/temporal.c
  src/time.c
  src/timer_queue.c
  src/timer_event.c
  src/verify.c)

if(WIN32)
  target_sources(${TARGET_NAME} PRIVATE src/io_native_iocp.c)
  target_compile_definitions(${TARGET_NAME} PRIVATE CFLOW_HAS_NATIVE_IOCP=1)
  target_link_libraries(${TARGET_NAME} PRIVATE ws2_32)
elseif(UNIX)
  target_sources(${TARGET_NAME} PRIVATE src/io_native_readiness.c)
  target_compile_definitions(${TARGET_NAME} PRIVATE CFLOW_HAS_NATIVE_POLL=1)
  if(APPLE)
    target_compile_definitions(${TARGET_NAME} PRIVATE CFLOW_HAS_NATIVE_KQUEUE=1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      target_sources(${TARGET_NAME} PRIVATE src/io_native_darwin_aio.c)
      target_compile_definitions(${TARGET_NAME} PRIVATE CFLOW_HAS_NATIVE_DARWIN_AIO=1)
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${TARGET_NAME} PRIVATE src/io_native_io_uring.c)
    target_compile_definitions(${TARGET_NAME} PRIVATE CFLOW_HAS_NATIVE_IO_URING=1)
    if(SALTS_ENABLE_EPOLL_READINESS)
      target_compile_definitions(${TARGET_NAME} PRIVATE CFLOW_HAS_NATIVE_EPOLL=1)
    endif()
  endif()
endif()

# A static CFlow/CSTL archive can be linked into more than one native DSO.
# Keep its reflected CMeta descriptor globals image-local, preserving public
# semantic type equality instead of making duplicate data symbols preemptible.
# This is a pure static-owner policy: explicitly requested shared libraries
# retain their existing export/link ABI and MUST be reviewed separately.
get_target_property(_salts_owner_type ${TARGET_NAME} TYPE)
if(_salts_owner_type STREQUAL "STATIC_LIBRARY")
  set_target_properties(${TARGET_NAME} PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
endif()
unset(_salts_owner_type)

cmake_config_target(
  ${TARGET_NAME}
  ALIAS Salts::CFlow
  FOLDER "cflow"
  EXPORT_NAME CFlow
  VERSION ${CFLOW_LIBRARY_VERSION}
  SOVERSION ${CFLOW_ABI_VERSION})

if(WIN32 AND BUILD_SHARED_LIBS)
  set_target_properties(${TARGET_NAME} PROPERTIES
    OUTPUT_NAME "cmeta_cflow-${CFLOW_ABI_VERSION}")
endif()

target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
target_compile_definitions(
  ${TARGET_NAME}
  PUBLIC
    CFLOW_MACHINE_MAX_STATES=${CFLOW_MACHINE_MAX_STATES}
    CFLOW_MACHINE_MAX_EVENTS=${CFLOW_MACHINE_MAX_EVENTS}
    CFLOW_MACHINE_MAX_GUARDS=${CFLOW_MACHINE_MAX_GUARDS}
    CFLOW_MACHINE_MAX_ACTIONS=${CFLOW_MACHINE_MAX_ACTIONS}
    CFLOW_MACHINE_MAX_TRANSITIONS=${CFLOW_MACHINE_MAX_TRANSITIONS}
    CFLOW_STATECHART_MAX_ACTION_REFS=${CFLOW_STATECHART_MAX_ACTION_REFS}
    CFLOW_STATECHART_MAX_INSTANCE_BYTES=${CFLOW_STATECHART_MAX_INSTANCE_BYTES}
  PRIVATE
    CFLOW_STATECHART_ERROR_CAPACITY=${CFLOW_STATECHART_ERROR_CAPACITY})
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
# CFlow is a CMeta execution layer. Real NativeIO is always Core-owned.
target_link_libraries(${TARGET_NAME}
  PUBLIC Salts::CMeta Salts::NativeIO Salts::Platform
  PRIVATE Salts::Concurrency)

install(
  TARGETS ${TARGET_NAME}
  EXPORT SaltsTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  DIRECTORY include/cflow
  DESTINATION include
  FILES_MATCHING
    PATTERN "*.h"
    PATTERN "plan_internal.h" EXCLUDE
    PATTERN "relation_exec.h" EXCLUDE)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BUILD_BENCHMARKS OR CFLOW_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
