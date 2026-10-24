if(NOT DEFINED SALTS_BINARY_DIR OR
   NOT DEFINED SALTS_TEST_VERSION OR
   NOT DEFINED CONSUMER_SOURCE_DIR OR
   NOT DEFINED WORK_DIR OR
   NOT DEFINED CTEST_COMMAND)
  message(FATAL_ERROR "installed consumer test arguments are required")
endif()

if(NOT DEFINED SALTS_TEST_CONFIG OR "${SALTS_TEST_CONFIG}" STREQUAL "")
  set(SALTS_TEST_CONFIG Release)
endif()
if(NOT SALTS_TEST_CONFIG MATCHES "^[A-Za-z0-9_+-]+$")
  message(FATAL_ERROR "Invalid installed SDK configuration")
endif()
if(NOT DEFINED SALTS_GENERATOR OR "${SALTS_GENERATOR}" STREQUAL "")
  message(FATAL_ERROR "Installed consumer must use parent CMake generator")
endif()

set(prefix "${WORK_DIR}/prefix")
set(build "${WORK_DIR}/build")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${SALTS_BINARY_DIR}"
    --config "${SALTS_TEST_CONFIG}" --prefix "${prefix}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "Salts SDK install failed: ${install_result}")
endif()

set(consumer_configure
  -S "${CONSUMER_SOURCE_DIR}" -B "${build}" -G "${SALTS_GENERATOR}"
  "-DCMAKE_PREFIX_PATH=${prefix}"
  "-DSALTS_TEST_PREFIX=${prefix}"
  "-DSalts_DIR=${prefix}/lib/cmake/Salts"
  "-DSALTS_TEST_VERSION=${SALTS_TEST_VERSION}"
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE)
foreach(pair IN ITEMS C_COMPILER CXX_COMPILER TOOLCHAIN_FILE)
  if(DEFINED SALTS_${pair} AND NOT "${SALTS_${pair}}" STREQUAL "")
    list(APPEND consumer_configure
      "-DCMAKE_${pair}=${SALTS_${pair}}")
  endif()
endforeach()
execute_process(
  COMMAND "${CMAKE_COMMAND}" ${consumer_configure}
  RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "installed Salts SDK consumer configure failed: ${configure_result}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${build}"
    --config "${SALTS_TEST_CONFIG}" --parallel 2
  RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "installed Salts SDK consumer build failed: ${build_result}")
endif()

# Imported Core is shared. Resolve only this exact installed SDK at runtime.
if(WIN32)
  set(ENV{PATH} "${prefix}/bin;$ENV{PATH}")
elseif(APPLE)
  set(ENV{DYLD_LIBRARY_PATH} "${prefix}/lib:$ENV{DYLD_LIBRARY_PATH}")
else()
  set(ENV{LD_LIBRARY_PATH} "${prefix}/lib:$ENV{LD_LIBRARY_PATH}")
endif()

execute_process(
  COMMAND "${CTEST_COMMAND}" --test-dir "${build}"
    -C "${SALTS_TEST_CONFIG}" --output-on-failure
  RESULT_VARIABLE test_result)
if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "installed Salts SDK consumer test failed: ${test_result}")
endif()
