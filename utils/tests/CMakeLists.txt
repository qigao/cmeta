# Common module tests

# Collect test files
set(TLOG_HEADER_COLLISION_C
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tlog_header_collision.c)
set(TLOG_HEADER_COLLISION_CPP
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tlog_header_collision.cpp)
set(FMT_SURFACE_CONTRACT_C
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fmt_surface_contract.c)
set(FMT_SURFACE_CONTRACT_CPP
    ${CMAKE_CURRENT_SOURCE_DIR}/test_fmt_surface_contract.cpp)
set(EXECUTION_COMPAT_C
    ${CMAKE_CURRENT_SOURCE_DIR}/test_execution_compat.c)
set(SALTS_FS_REPLACE_FAULT_C
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cmeta_fs_replace_fault.c)
cmake_add_source(TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS
                 "test_*.c" EXCLUDES ${TLOG_HEADER_COLLISION_C} ${FMT_SURFACE_CONTRACT_C}
                 ${EXECUTION_COMPAT_C} ${SALTS_FS_REPLACE_FAULT_C})
cmake_add_source(CXX_TEST_SOURCES DIRS ${CMAKE_CURRENT_SOURCE_DIR} PATTERNS
                 "test_*.cpp" EXCLUDES ${TLOG_HEADER_COLLISION_CPP} ${FMT_SURFACE_CONTRACT_CPP})

# Common libraries for all tests
set(COMMON_LIBS Salts::TinyTest Salts::Core)
if(WIN32)
  list(APPEND COMMON_LIBS psapi iphlpapi userenv ws2_32)
endif()

# Create test executables
foreach(test_source ${TEST_SOURCES} ${CXX_TEST_SOURCES})
  get_filename_component(test_name ${test_source} NAME_WE)
  cmake_add_test(${test_name}
    SOURCES ${test_source}
    FOLDER "utils/tests"
    LIBS ${COMMON_LIBS})
endforeach()

# Seed a parent-only variable so the process clean-environment contract can be
# tested without relying on PATH, which command interpreters may synthesize.
if(TARGET test_cmeta_process)
  set_tests_properties(test_cmeta_process PROPERTIES
    ENVIRONMENT "SALTS_PROCESS_PARENT_SENTINEL=parent-value")
endif()

# Core is the only production dependency of this link contract. Its legacy headers
# must pull Platform/Concurrency transitively through the exported target graph.
cmake_add_test(test_execution_compat
  SOURCES ${EXECUTION_COMPAT_C}
  FOLDER "utils/tests"
  LIBS Salts::TinyTest Salts::Core)
set_target_properties(test_execution_compat PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND TARGET test_tlog)
  target_compile_options(test_tlog PRIVATE -Werror=null-dereference)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(cmeta_fs_fsync_fault SHARED cmeta_fs_fsync_fault.c)
  target_compile_features(cmeta_fs_fsync_fault PRIVATE c_std_11)
  set_target_properties(cmeta_fs_fsync_fault PROPERTIES
                        PREFIX ""
                        FOLDER "utils/tests")

  add_executable(cmeta_fs_replace_fault_test
                 ${SALTS_FS_REPLACE_FAULT_C})
  target_compile_features(cmeta_fs_replace_fault_test PRIVATE c_std_11)
  target_link_libraries(cmeta_fs_replace_fault_test PRIVATE Salts::Core Salts::TinyTest)
  set_target_properties(cmeta_fs_replace_fault_test
                        PROPERTIES C_STANDARD_REQUIRED ON
                                   FOLDER "utils/tests")
  add_test(
    NAME test_cmeta_fs_replace_pre_publish_fault
    COMMAND ${CMAKE_COMMAND} -E env
            "SALTS_FS_TEST_FAIL_FSYNC_CALL=1"
            "LD_PRELOAD=$<TARGET_FILE:cmeta_fs_fsync_fault>"
            $<TARGET_FILE:cmeta_fs_replace_fault_test>)
  add_test(
    NAME test_cmeta_fs_replace_post_publish_fault
    COMMAND ${CMAKE_COMMAND} -E env
            "SALTS_FS_TEST_FAIL_FSYNC_CALL=2"
            "LD_PRELOAD=$<TARGET_FILE:cmeta_fs_fsync_fault>"
            $<TARGET_FILE:cmeta_fs_replace_fault_test>)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(tgt test_tlog_cpp test_cmeta_str_meta_cpp)
    if(TARGET ${tgt})
      target_compile_options(${tgt} PRIVATE -Werror=old-style-cast)
    endif()
  endforeach()
endif()

# fmt surface contracts deliberately do not link Salts::Core. They stub
# fmt_print and validate the public macro expansion contract. Because fmt.h
# reaches the Core compatibility headers directly, these standalone targets
# explicitly own both canonical execution-substrate include contracts.
cmake_add_test(test_fmt_surface_contract_c
  SOURCES ${FMT_SURFACE_CONTRACT_C}
  FOLDER "utils/tests"
  LIBS Salts::TinyTest Salts::CMeta Salts::Platform Salts::Concurrency)
target_include_directories(test_fmt_surface_contract_c PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../parser
  ${CMAKE_SOURCE_DIR}/vendor/sds)
set_target_properties(test_fmt_surface_contract_c PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
if(NOT WIN32)
  target_compile_definitions(test_fmt_surface_contract_c PRIVATE
    _POSIX_C_SOURCE=200809L)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_fmt_surface_contract_c PRIVATE
    -Werror=unused-parameter
    -Werror=unused-variable)
endif()

cmake_add_test(test_fmt_surface_contract_cpp
  SOURCES ${FMT_SURFACE_CONTRACT_CPP}
  FOLDER "utils/tests"
  LIBS Salts::TinyTest Salts::CMeta Salts::Platform Salts::Concurrency)
target_include_directories(test_fmt_surface_contract_cpp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../parser
  ${CMAKE_SOURCE_DIR}/vendor/sds)
set_target_properties(test_fmt_surface_contract_cpp PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_fmt_surface_contract_cpp PRIVATE
    -Werror=sign-conversion
    -Werror=old-style-cast
    -Werror=missing-field-initializers
    -Werror=unused-parameter
    -Werror=unused-variable)
endif()

if(TARGET test_cmeta_str_meta)
  target_sources(test_cmeta_str_meta PRIVATE cmeta_str_meta_peer.c)
endif()

foreach(tgt test_cmeta_cmeta_data test_cmeta_cmeta_data_cpp)
  if(TARGET ${tgt})
    target_sources(${tgt} PRIVATE cmeta_cmeta_data_peer.c)
  endif()
endforeach()

cmake_add_test(test_tlog_header_collision_c
  SOURCES ${TLOG_HEADER_COLLISION_C}
  FOLDER "utils/tests"
  LIBS ${COMMON_LIBS})
set_target_properties(test_tlog_header_collision_c PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

cmake_add_test(test_tlog_header_collision_cpp
  SOURCES ${TLOG_HEADER_COLLISION_CPP}
  FOLDER "utils/tests"
  LIBS ${COMMON_LIBS})
set_target_properties(test_tlog_header_collision_cpp PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

# Link specific libraries for certain tests
foreach(tgt test_fmt_re2c test_fmt)
  if(TARGET ${tgt})
    target_link_libraries(${tgt} PRIVATE fmt_lexer)
  endif()
endforeach()

foreach(tgt test_cmeta_str)
  if(TARGET ${tgt})
    target_link_libraries(${tgt} PRIVATE Salts::SDS)
  endif()
endforeach()

# test_re exercises the internal SIMDe first-byte scanner (utils/src/re_scan.h)
if(TARGET test_re)
  target_include_directories(test_re PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SIMDE_INCLUDE_DIRS})
endif()

# The production default embeds static CFlow and CSTL into independent
# native runtime images. Their address-bearing reflected descriptors are
# image-local; exporting them reintroduces ASan ODR/interposition defects.
# Run the real dynamic-loader contract on every native host, not only the
# old #1187 feature branch. A deliberately shared CFlow/CSTL build has a
# different, independently accepted data-import ABI and is excluded here.
get_target_property(_salts_cstl_type cmeta_cstl TYPE)
get_target_property(_salts_cflow_type cmeta_cflow TYPE)
if(NOT CMAKE_CROSSCOMPILING AND
   _salts_cstl_type STREQUAL "STATIC_LIBRARY" AND
   _salts_cflow_type STREQUAL "STATIC_LIBRARY")
  add_executable(salts_static_owner_exports_test static_owner_exports_test.c)
  target_compile_features(salts_static_owner_exports_test PRIVATE c_std_11)
  target_link_libraries(salts_static_owner_exports_test PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(salts_static_owner_exports_test PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    FOLDER "utils/tests")
  add_test(NAME salts_static_owner_exports_test
    COMMAND "$<TARGET_FILE:salts_static_owner_exports_test>"
            "$<TARGET_FILE:salts>" "$<TARGET_FILE:cnet>")
  set_tests_properties(salts_static_owner_exports_test PROPERTIES TIMEOUT 60)
endif()
unset(_salts_cstl_type)
unset(_salts_cflow_type)
