cmake_add_test(native_io_test
  SOURCES native_io_test.c
  LIBS Salts::NativeIO Salts::Coroutine Salts::TinyTest Salts::Concurrency
  FOLDER "native-io/tests")
set_target_properties(native_io_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(native_io_accept_escrow_test
  SOURCES native_io_accept_escrow_test.c
  LIBS Salts::TinyTest Salts::NativeIO
  FOLDER "native-io/tests")
set_target_properties(native_io_accept_escrow_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_dependencies(native_io_test native_io_accept_escrow_test)

cmake_add_test(native_io_internal_accept_test
  SOURCES native_io_internal_accept_test.c
  LIBS Salts::TinyTest Salts::NativeIO
  FOLDER "native-io/tests")
target_include_directories(native_io_internal_accept_test
  PRIVATE "${PROJECT_SOURCE_DIR}/native-io/src")
set_target_properties(native_io_internal_accept_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_dependencies(native_io_test native_io_internal_accept_test)

cmake_add_test(native_io_sharded_test
  SOURCES native_io_sharded_test.c
  LIBS Salts::NativeIO Salts::TinyTest Salts::Platform
  FOLDER "native-io/tests")
set_target_properties(native_io_sharded_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

# Bounded Owner-only NativeIO backend host lease; no unsafe raw SG borrow.
cmake_add_test(native_io_sharded_host_lease_test
  SOURCES native_io_sharded_host_lease_test.c
  LIBS Salts::NativeIO Salts::TinyTest Salts::Platform
  FOLDER "native-io/tests")
set_target_properties(native_io_sharded_host_lease_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_dependencies(native_io_test native_io_sharded_host_lease_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  cmake_add_test(native_io_readiness_lifetime_test
    SOURCES native_io_readiness_lifetime_test.c
    LIBS Salts::NativeIO Salts::TinyTest Salts::Platform
    FOLDER "native-io/tests")
  set_target_properties(native_io_readiness_lifetime_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_dependencies(native_io_test native_io_readiness_lifetime_test)

  add_executable(native_io_uring_batch_test native_io_uring_batch_test.c)
  target_link_libraries(native_io_uring_batch_test PRIVATE Salts::NativeIO Salts::TinyTest)
  set_target_properties(native_io_uring_batch_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "native-io/tests")
  add_test(NAME native_io_uring_batch_test COMMAND native_io_uring_batch_test)
  add_dependencies(native_io_test native_io_uring_batch_test)

  cmake_add_test(native_io_uring_accept_test
    SOURCES native_io_uring_accept_test.c
    LIBS Salts::TinyTest Salts::NativeIO
    FOLDER "native-io/tests")
  target_include_directories(native_io_uring_accept_test
    PRIVATE "${PROJECT_SOURCE_DIR}/native-io/src")
  set_target_properties(native_io_uring_accept_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  set_tests_properties(native_io_uring_accept_test PROPERTIES SKIP_RETURN_CODE 77)
  add_dependencies(native_io_test native_io_uring_accept_test)
endif()

if(WIN32)
  target_link_libraries(native_io_test PRIVATE ws2_32)

  cmake_add_test(native_io_iocp_accept_test
    SOURCES native_io_iocp_accept_test.c
    LIBS Salts::TinyTest Salts::NativeIO ws2_32
    FOLDER "native-io/tests")
  target_include_directories(native_io_iocp_accept_test
    PRIVATE "${PROJECT_SOURCE_DIR}/native-io/src")
  set_target_properties(native_io_iocp_accept_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_dependencies(native_io_test native_io_iocp_accept_test)
endif()

if(NOT WIN32)
  cmake_add_test(native_io_readiness_accept_test
    SOURCES native_io_readiness_accept_test.c
    LIBS Salts::TinyTest Salts::NativeIO
    FOLDER "native-io/tests")
  target_include_directories(native_io_readiness_accept_test
    PRIVATE "${PROJECT_SOURCE_DIR}/native-io/src")
  set_target_properties(native_io_readiness_accept_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_dependencies(native_io_test native_io_readiness_accept_test)
endif()


# #1168: a SEPARATE first-party static backend built from the actual NativeIO
# sources, with a private post-kernel-dispatch hook. It is never installed,
# never linked into Salts::NativeIO and cannot change production ABI/behavior.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
  set(_native_io_dispatch_sources
    "${PROJECT_SOURCE_DIR}/native-io/src/native_io.c"
    "${PROJECT_SOURCE_DIR}/native-io/src/native_io_accept_escrow.c"
    "${PROJECT_SOURCE_DIR}/native-io/src/native_io_sharded.c"
    "${PROJECT_SOURCE_DIR}/native-io/src/native_ipc.c"
    "${PROJECT_SOURCE_DIR}/native-io/src/native_io_readiness.c"
    "${PROJECT_SOURCE_DIR}/native-io/src/native_ipc_posix.c")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _native_io_dispatch_sources
      "${PROJECT_SOURCE_DIR}/native-io/src/native_io_linux.c"
      "${PROJECT_SOURCE_DIR}/native-io/src/native_io_epoll.c"
      "${PROJECT_SOURCE_DIR}/native-io/src/native_io_io_uring.c")
  else()
    list(APPEND _native_io_dispatch_sources
      "${PROJECT_SOURCE_DIR}/native-io/src/native_io_bsd.c"
      "${PROJECT_SOURCE_DIR}/native-io/src/native_io_kqueue.c")
  endif()
  add_library(native_io_test_dispatch_impl STATIC
    ${_native_io_dispatch_sources})
  target_compile_features(native_io_test_dispatch_impl PUBLIC c_std_11)
  target_compile_definitions(native_io_test_dispatch_impl
    PRIVATE SALTS_NATIVE_IO_TEST_DISPATCH=1)
  if(APPLE)
    target_compile_definitions(native_io_test_dispatch_impl
      PRIVATE _DARWIN_C_SOURCE=1)
  endif()
  target_include_directories(native_io_test_dispatch_impl
    PUBLIC "${PROJECT_SOURCE_DIR}/native-io/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/native-io/src")
  target_link_libraries(native_io_test_dispatch_impl
    PUBLIC Salts::Platform
    PRIVATE Salts::Coroutine)
  set_target_properties(native_io_test_dispatch_impl
    PROPERTIES FOLDER "native-io/tests")

  cmake_add_test(native_io_dispatch_cancel_test
    SOURCES native_io_dispatch_cancel_test.c
    LIBS native_io_test_dispatch_impl Salts::TinyTest
         Salts::Coroutine Salts::Concurrency
    FOLDER "native-io/tests")
  target_include_directories(native_io_dispatch_cancel_test PRIVATE
    "${PROJECT_SOURCE_DIR}/native-io/src")
  target_compile_definitions(native_io_dispatch_cancel_test
    PRIVATE SALTS_NATIVE_IO_TEST_DISPATCH=1)
  set_target_properties(native_io_dispatch_cancel_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_dependencies(native_io_test native_io_dispatch_cancel_test)
endif()

cmake_add_test(native_io_header_cpp_test
  SOURCES native_io_header_cpp_test.cpp
  LIBS Salts::TinyTest Salts::NativeIO
  FOLDER "native-io/tests")
set_target_properties(native_io_header_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

cmake_add_test(native_ipc_test
  SOURCES native_ipc_test.c
  LIBS Salts::NativeIO Salts::TinyTest Salts::Platform
  FOLDER "native-io/tests")
set_target_properties(native_ipc_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(native_ipc_header_cpp_test
  SOURCES native_ipc_header_cpp_test.cpp
  LIBS Salts::TinyTest Salts::NativeIO
  FOLDER "native-io/tests")
set_target_properties(native_ipc_header_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)


