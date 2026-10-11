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


