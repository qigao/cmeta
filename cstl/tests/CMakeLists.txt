cmake_add_test(cstl_header_test
  SOURCES cstl_header_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_header_cpp_test
  SOURCES cstl_header_cpp_test.cpp
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_header_typed_test
  SOURCES cstl_header_typed_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

add_library(cstl_imported_metadata_fixture SHARED cstl_imported_metadata_fixture.c)
target_link_libraries(cstl_imported_metadata_fixture PUBLIC Salts::Core Salts::CSTL)
target_compile_definitions(cstl_imported_metadata_fixture PRIVATE CSTL_METADATA_PRODUCER)
cmake_config_target(cstl_imported_metadata_fixture FOLDER "cstl/tests")
cmake_add_test(cstl_imported_metadata_test
  SOURCES cstl_imported_metadata_test.c
  LIBS cstl_imported_metadata_fixture Salts::TinyTest
  FOLDER "cstl/tests")
cmake_add_test(cstl_imported_metadata_cpp_test
  SOURCES cstl_imported_metadata_cpp_test.cpp
  LIBS cstl_imported_metadata_fixture Salts::TinyTest
  FOLDER "cstl/tests")
set_target_properties(cstl_imported_metadata_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if(WIN32)
  add_executable(cstl_imported_tstr_map_smoke
    cstl_imported_tstr_map_smoke.c)
  target_link_libraries(cstl_imported_tstr_map_smoke
    PRIVATE Salts::Core Salts::CSTL Salts::TinyTest)
  set_target_properties(cstl_imported_tstr_map_smoke PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    FOLDER "cstl/tests")
  add_test(NAME cstl_imported_tstr_map_smoke
    COMMAND cstl_imported_tstr_map_smoke)
endif()

cmake_add_test(cstl_tinytest_test
  SOURCES cstl_tinytest_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_header_typed_cpp_test
  SOURCES cstl_header_typed_cpp_test.cpp
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_typed_decl_cpp_test
  SOURCES cstl_typed_decl_c.c cstl_typed_decl_cpp.cpp
  LIBS Salts::TinyTest Salts::CSTL
  FOLDER "cstl/tests")

cmake_add_test(cstl_stack_type_collision_test
  SOURCES cstl_stack_type_collision_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_generic_identity_test
  SOURCES cstl_generic_identity_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_construction_binding_test
  SOURCES cstl_construction_binding_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_sequence_test
  SOURCES cstl_sequence_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_ownership_test
  SOURCES cstl_ownership_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_hash_test
  SOURCES cstl_hash_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_typed_test
  SOURCES cstl_typed_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_tree_test
  SOURCES cstl_tree_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_list_test
  SOURCES cstl_list_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_map_test
  SOURCES cstl_map_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  foreach(tgt cstl_list_test cstl_map_test)
    target_compile_options(${tgt} PRIVATE -Werror=misleading-indentation)
  endforeach()
  target_compile_options(cstl_map_test PRIVATE
    -Werror=missing-field-initializers)
endif()

cmake_add_test(cstl_rbtree_test
  SOURCES cstl_rbtree_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_entry_test
  SOURCES cstl_entry_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_sort_test
  SOURCES cstl_sort_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_stream_test
  SOURCES cstl_stream_test.c
  LIBS Salts::CSTLStream Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_stream_cpp_test
  SOURCES cstl_stream_cpp_test.cpp
  LIBS Salts::CSTLStream Salts::TinyTest
  FOLDER "cstl/tests")

cmake_add_test(cstl_managed_stream_test
  SOURCES cstl_managed_stream_test.c
  LIBS Salts::CSTLStream Salts::TinyTest
  FOLDER "cstl/tests")

set_target_properties(
  cstl_header_test
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

set_target_properties(cstl_stream_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

set_target_properties(
  cstl_header_cpp_test
  PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

set_target_properties(
  cstl_header_typed_test
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

set_target_properties(
  cstl_header_typed_cpp_test
  PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

set_target_properties(
  cstl_typed_decl_cpp_test
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  foreach(target IN ITEMS cstl_header_typed_test cstl_header_typed_cpp_test
                          cstl_typed_decl_cpp_test)
    target_compile_options(${target} PRIVATE -Werror=missing-field-initializers)
  endforeach()
endif()

set_target_properties(
  cstl_stack_type_collision_test
  cstl_generic_identity_test
  cstl_construction_binding_test
  cstl_sequence_test
  cstl_ownership_test
  cstl_hash_test
  cstl_typed_test
  cstl_tinytest_test
  cstl_tree_test
  cstl_list_test
  cstl_map_test
  cstl_rbtree_test
  cstl_entry_test
  cstl_sort_test
  cstl_stream_test
  cstl_managed_stream_test
  PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

cmake_add_test(cstl_vec_alloc_test
  SOURCES cstl_vec_alloc_test.c
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")
cmake_add_test(cstl_vec_alloc_cpp_test
  SOURCES cstl_vec_alloc_cpp_test.cpp
  LIBS Salts::CSTL Salts::TinyTest
  FOLDER "cstl/tests")
set_target_properties(cstl_vec_alloc_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cstl_vec_alloc_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
