# A public one-row TypeFunction must compile as *strict ISO C11* on
# AppleClang as well as GNU/Clang; no omitted-variadic C23 extension or
# warning downgrade is accepted.
add_executable(cmeta_pp_iso_c11_test cmeta_pp_iso_c11_test.c)
target_link_libraries(cmeta_pp_iso_c11_test PRIVATE CMeta::CMeta)
set_target_properties(cmeta_pp_iso_c11_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_pp_iso_c11_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME cmeta_pp_iso_c11_test COMMAND cmeta_pp_iso_c11_test)
set_target_properties(cmeta_pp_iso_c11_test PROPERTIES
  FOLDER "cmeta/tests")

cmake_add_test(cmeta_core_data_test
  SOURCES cmeta_core_data_test.c
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")

cmake_add_test(cmeta_data_reflect_test
  SOURCES cmeta_data_reflect_test.c cmeta_data_reflect_peer.c
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_data_reflect_cpp_test
  SOURCES cmeta_data_reflect_cpp_test.cpp cmeta_data_reflect_peer.c
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")

cmake_add_test(cmeta_fastpath_pattern_test
  SOURCES cmeta_pattern_test.c
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_fastpath_pattern_cpp_test
  SOURCES cmeta_pattern_cpp_test.cpp
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")

# Post-freeze POSA2 CMeta composition: typed locking policy, synchronized
# public Interface and real Monitor Object with bounded wait/close semantics.
cmake_add_test(cmeta_ace_sync_test
  SOURCES cmeta_ace_sync_test.c
  LIBS CMeta::CMeta Salts::Platform Salts::TinyTest
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_ace_sync_cpp_test
  SOURCES cmeta_ace_sync_cpp_test.cpp
  LIBS CMeta::CMeta Salts::Platform Salts::TinyTest
  FOLDER "cmeta/tests")
set_target_properties(cmeta_ace_sync_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_ace_sync_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set_tests_properties(cmeta_ace_sync_test cmeta_ace_sync_cpp_test PROPERTIES TIMEOUT 60)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_ace_sync_cpp_test PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Werror=old-style-cast>")
endif()


# #1064: CMeta describes actual CPU event callback/borrow/role type contracts;
# Platform retains all Leader/Followers mutex, condition and thread authority.
cmake_add_test(cmeta_ace_leader_followers_contract_test
  SOURCES cmeta_ace_leader_followers_contract_test.c
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_ace_leader_followers_contract_cpp_test
  SOURCES cmeta_ace_leader_followers_contract_cpp_test.cpp
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")
set_target_properties(cmeta_ace_leader_followers_contract_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_ace_leader_followers_contract_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set_tests_properties(cmeta_ace_leader_followers_contract_test
  cmeta_ace_leader_followers_contract_cpp_test PROPERTIES TIMEOUT 60)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_ace_leader_followers_contract_cpp_test PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Werror=old-style-cast>")
endif()

cmake_add_test(cmeta_fastpath_component_test
  SOURCES cmeta_component_test.c
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_fastpath_component_cpp_test
  SOURCES cmeta_component_cpp_test.cpp
  LIBS CMeta::CMeta Salts::TinyTest
  FOLDER "cmeta/tests")
set_target_properties(cmeta_fastpath_pattern_test cmeta_fastpath_component_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_fastpath_pattern_cpp_test cmeta_fastpath_component_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_fastpath_pattern_cpp_test PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Werror=old-style-cast>")
  target_compile_options(cmeta_fastpath_component_cpp_test PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Werror=old-style-cast>")
endif()

cmake_add_test(cmeta_trace_test
  SOURCES cmeta_trace_test.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Platform
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_trace_cpp_test
  SOURCES cmeta_trace_cpp_test.cpp cmeta_trace_peer.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Platform
  FOLDER "cmeta/tests")
set_target_properties(cmeta_trace_test cmeta_trace_cpp_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_trace_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set_tests_properties(cmeta_trace_test PROPERTIES TIMEOUT 60)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_trace_cpp_test PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Werror=old-style-cast>")
endif()

cmake_add_test(cmeta_fastpath_test
  SOURCES cmeta_fastpath_test.c cmeta_fastpath_peer.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Platform
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_fastpath_cpp_test
  SOURCES cmeta_fastpath_cpp_test.cpp cmeta_fastpath_peer.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Platform
  FOLDER "cmeta/tests")
cmake_add_test(cmeta_fastpath_coroutine_test
  SOURCES cmeta_fastpath_coroutine_test.c cmeta_fastpath_peer.c
  LIBS CMeta::CMeta Salts::TinyTest Salts::Coroutine Salts::Platform
  FOLDER "cmeta/tests")
set_target_properties(cmeta_fastpath_test cmeta_fastpath_cpp_test cmeta_fastpath_coroutine_test
  PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(cmeta_fastpath_cpp_test
  PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_fastpath_cpp_test PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Werror=old-style-cast>")
endif()

# #1244: malformed canonical FunctionDesc / FunctionAbi must fail closed.
add_executable(cmeta_function_desc_contract_test
  cmeta_function_desc_contract_test.c)
target_link_libraries(cmeta_function_desc_contract_test PRIVATE CMeta::CMeta)
set_target_properties(cmeta_function_desc_contract_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  FOLDER "cmeta/tests")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_function_desc_contract_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME cmeta_function_desc_contract_test
  COMMAND cmeta_function_desc_contract_test)

# #1244: exact field-type proofs must agree between strict C11 _Generic and
# C++17 decltype, including qualifiers, array extent, pointers and callbacks.
add_executable(cmeta_field_type_contract_test
  cmeta_field_type_contract_test.c)
target_link_libraries(cmeta_field_type_contract_test PRIVATE CMeta::CMeta)
set_target_properties(cmeta_field_type_contract_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  FOLDER "cmeta/tests")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_field_type_contract_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME cmeta_field_type_contract_test
  COMMAND cmeta_field_type_contract_test)

add_executable(cmeta_field_type_contract_cpp_test
  cmeta_field_type_contract_cpp_test.cpp)
target_link_libraries(cmeta_field_type_contract_cpp_test PRIVATE CMeta::CMeta)
set_target_properties(cmeta_field_type_contract_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  FOLDER "cmeta/tests")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_field_type_contract_cpp_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME cmeta_field_type_contract_cpp_test
  COMMAND cmeta_field_type_contract_cpp_test)

# #1244: Enum() metadata is TU-local; compare canonical contents, never address identity.
add_executable(cmeta_enum_tu_contract_test
  cmeta_enum_tu_contract_test.c
  cmeta_enum_tu_contract_peer.c)
target_link_libraries(cmeta_enum_tu_contract_test PRIVATE CMeta::CMeta)
set_target_properties(cmeta_enum_tu_contract_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  FOLDER "cmeta/tests")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cmeta_enum_tu_contract_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME cmeta_enum_tu_contract_test
  COMMAND cmeta_enum_tu_contract_test)
