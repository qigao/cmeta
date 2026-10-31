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

# C11/C++17 independent SDK consumption of the five new ACE pattern facades.
# Use the existing installed-consumer helper, not another workflow.
if(NOT WIN32)
  add_test(NAME cmeta_ace_patterns_installed_test
    COMMAND "${CMAKE_COMMAND}"
      "-DSALTS_BINARY_DIR=${CMAKE_BINARY_DIR}"
      "-DSALTS_TEST_VERSION=${PROJECT_VERSION}"
      "-DSALTS_TEST_CONFIG=$<CONFIG>"
      "-DSALTS_GENERATOR=${CMAKE_GENERATOR}"
      "-DSALTS_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DSALTS_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DSALTS_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_config/ace_patterns"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-ace-patterns"
      "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
      -P "${PROJECT_SOURCE_DIR}/cmake/ci/verify-installed-consumer.cmake")
  set_tests_properties(cmeta_ace_patterns_installed_test PROPERTIES
    TIMEOUT 180 RUN_SERIAL TRUE)
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

# ACE feature-freeze compile admission: the canonical Interface method
# signature must compile under C11 and C++17; an incompatible native
# return-type assertion must fail. FunctionAbi metadata is covered by the
# existing C11/C++17 pattern runtime suites.
# This runs at configure time only on native hosts (mobile cross builds retain
# the ordinary public-header compile tests).
if(NOT CMAKE_CROSSCOMPILING)
  include(CheckCSourceCompiles)
  include(CheckCXXSourceCompiles)

  set(_cmeta_ace_previous_required_includes "${CMAKE_REQUIRED_INCLUDES}")
  set(_cmeta_ace_previous_try_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_REQUIRED_INCLUDES "${PROJECT_SOURCE_DIR}/cmeta/include")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

  # Native signature admission uses an existing, widely exercised R0
  # CMeta Interface declaration. Compare its canonical generated vtable member
  # to the exact declared type; the bad case must reject void return. Reflected
  # FunctionAbi result-carrier runtime facts are checked by cmeta_pattern_test.
  # No custom pattern facade or duplicate signature ABI is introduced.
  set(_cmeta_ace_good_interface_source [=[
#include <cmeta/function.h>
#include <cmeta/object.h>
#include <cmeta/object_interface.h>
#define ACE_SCORE_METHODS(X,I) X(I,R0,int,get,_)
CMETA_INTERFACE(ace_freeze_score, ACE_SCORE_METHODS);
CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(
    CMETA_IFACE_MEMBER_ADDRESS_(ace_freeze_score,get),
    int (**)(void *)), "ACE exact CMeta Strategy method");
int main(void) { return 0; }
]=])
  set(_cmeta_ace_bad_interface_source [=[
#include <cmeta/function.h>
#include <cmeta/object.h>
#include <cmeta/object_interface.h>
#define ACE_SCORE_METHODS(X,I) X(I,R0,int,get,_)
CMETA_INTERFACE(ace_freeze_score, ACE_SCORE_METHODS);
CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(
    CMETA_IFACE_MEMBER_ADDRESS_(ace_freeze_score,get),
    void (**)(void *)), "ACE must reject wrong native return signature");
int main(void) { return 0; }
]=])

  # Re-evaluate on configuration: a compiler/SDK change must not silently
  # inherit a stale cached negative result from an earlier branch revision.
  foreach(_ace_check IN ITEMS
      SALTS_ACE_GOOD_C11_COMPILES SALTS_ACE_BAD_C11_COMPILES
      SALTS_ACE_GOOD_CPP17_COMPILES SALTS_ACE_BAD_CPP17_COMPILES)
    unset("${_ace_check}" CACHE)
  endforeach()

  check_c_source_compiles("${_cmeta_ace_good_interface_source}"
    SALTS_ACE_GOOD_C11_COMPILES)
  check_c_source_compiles("${_cmeta_ace_bad_interface_source}"
    SALTS_ACE_BAD_C11_COMPILES)
  check_cxx_source_compiles("${_cmeta_ace_good_interface_source}"
    SALTS_ACE_GOOD_CPP17_COMPILES)
  check_cxx_source_compiles("${_cmeta_ace_bad_interface_source}"
    SALTS_ACE_BAD_CPP17_COMPILES)

  # No erased call ABI: reject native callback constness mismatches at configure
  # time in both C11 and C++17 before creating any interceptor chain.
  set(_cmeta_ace_intercept_good [=[
#include <cmeta/ace_interceptor.h>
CMETA_INTERCEPTOR_TYPE(ace_intercept_probe, int, int);
cmeta_status exact_target(void *, const int *, int *);
CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&exact_target, ace_intercept_probe_target_fn),
                    "exact Interceptor signature");
int main(void) { return 0; }
]=])
  set(_cmeta_ace_intercept_bad [=[
#include <cmeta/ace_interceptor.h>
CMETA_INTERCEPTOR_TYPE(ace_intercept_probe, int, int);
cmeta_status mutable_target(void *, int *, int *);
CMETA_STATIC_ASSERT(CMETA_TYPE_MATCHES(&mutable_target, ace_intercept_probe_target_fn),
                    "mutable request cannot satisfy borrowed const request");
int main(void) { return 0; }
]=])
  foreach(_check IN ITEMS
      SALTS_ACE_INTERCEPT_GOOD_C11 SALTS_ACE_INTERCEPT_BAD_C11
      SALTS_ACE_INTERCEPT_GOOD_CPP17 SALTS_ACE_INTERCEPT_BAD_CPP17)
    unset("${_check}" CACHE)
  endforeach()
  check_c_source_compiles("${_cmeta_ace_intercept_good}" SALTS_ACE_INTERCEPT_GOOD_C11)
  check_c_source_compiles("${_cmeta_ace_intercept_bad}" SALTS_ACE_INTERCEPT_BAD_C11)
  check_cxx_source_compiles("${_cmeta_ace_intercept_good}" SALTS_ACE_INTERCEPT_GOOD_CPP17)
  check_cxx_source_compiles("${_cmeta_ace_intercept_bad}" SALTS_ACE_INTERCEPT_BAD_CPP17)
  if(NOT SALTS_ACE_INTERCEPT_GOOD_C11 OR SALTS_ACE_INTERCEPT_BAD_C11 OR
     NOT SALTS_ACE_INTERCEPT_GOOD_CPP17 OR SALTS_ACE_INTERCEPT_BAD_CPP17)
    message(FATAL_ERROR
      "ACE typed Interceptor C11/C++17 signature admission failed: "
      "good-C=${SALTS_ACE_INTERCEPT_GOOD_C11}, bad-C=${SALTS_ACE_INTERCEPT_BAD_C11}, "
      "good-C++=${SALTS_ACE_INTERCEPT_GOOD_CPP17}, bad-C++=${SALTS_ACE_INTERCEPT_BAD_CPP17}")
  endif()

  set(CMAKE_REQUIRED_INCLUDES "${_cmeta_ace_previous_required_includes}")
  if("${_cmeta_ace_previous_try_type}" STREQUAL "")
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
  else()
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_cmeta_ace_previous_try_type}")
  endif()

  if(NOT SALTS_ACE_GOOD_C11_COMPILES OR SALTS_ACE_BAD_C11_COMPILES OR
     NOT SALTS_ACE_GOOD_CPP17_COMPILES OR SALTS_ACE_BAD_CPP17_COMPILES)
    # Make a false-negative positive sample actionable on all hosts.
    set(_ace_compiler_log "${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml")
    if(NOT EXISTS "${_ace_compiler_log}")
      set(_ace_compiler_log "${CMAKE_BINARY_DIR}/CMakeFiles/CMakeError.log")
    endif()
    if(EXISTS "${_ace_compiler_log}")
      file(READ "${_ace_compiler_log}" _ace_diagnostics)
      string(FIND "${_ace_diagnostics}" "SALTS_ACE_GOOD_C11_COMPILES" _ace_start)
      if(_ace_start GREATER_EQUAL 0)
        string(LENGTH "${_ace_diagnostics}" _ace_log_size)
        math(EXPR _ace_remain "${_ace_log_size} - ${_ace_start}")
        if(_ace_remain GREATER 5500)
          set(_ace_remain 5500)
        endif()
        string(SUBSTRING "${_ace_diagnostics}" ${_ace_start} ${_ace_remain} _ace_excerpt)
        message(WARNING "ACE positive compile probe diagnostic:\n${_ace_excerpt}")
      endif()
    endif()
    message(FATAL_ERROR
      "ACE CMeta Interface native signature compile admission failed: "
      "valid C11=${SALTS_ACE_GOOD_C11_COMPILES}, invalid C11=${SALTS_ACE_BAD_C11_COMPILES}, "
      "valid C++17=${SALTS_ACE_GOOD_CPP17_COMPILES}, invalid C++17=${SALTS_ACE_BAD_CPP17_COMPILES}")
  endif()
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
