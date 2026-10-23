# CTest creates a fresh installed SDK prefix and a separate consumer build.
foreach(arg IN ITEMS SALTS_BINARY_DIR CONSUMER_SOURCE_DIR WORK_DIR CTEST_COMMAND)
  if(NOT DEFINED ${arg} OR "${${arg}}" STREQUAL "")
    message(FATAL_ERROR "missing installed CNet test argument: ${arg}")
  endif()
endforeach()
set(prefix "${WORK_DIR}/prefix")
set(build "${WORK_DIR}/consumer-build")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${SALTS_BINARY_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE installed)
if(NOT installed EQUAL 0)
  message(FATAL_ERROR "Salts SDK installation failed: ${installed}")
endif()
foreach(header IN ITEMS
    "cnet/manager.h" "cnet/client_pool.h" "cnet/recovery_policy.h"
    "cnet/name_lookup.h"
    "cnet/managed_dial.h" "cnet/sg_host.h" "salts/native_io_sharded.h")
  if(NOT EXISTS "${prefix}/include/${header}")
    message(FATAL_ERROR "installed CNet SDK missing header: ${header}")
  endif()
endforeach()
if(NOT EXISTS "${prefix}/lib/cmake/Salts/SaltsTargets.cmake")
  message(FATAL_ERROR "installed CNet SDK missing Salts targets export")
endif()

set(configure_args
  -S "${CONSUMER_SOURCE_DIR}" -B "${build}"
  "-DSALTS_TEST_PREFIX=${prefix}"
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE)
if(DEFINED SALTS_GENERATOR AND NOT SALTS_GENERATOR STREQUAL "")
  list(APPEND configure_args -G "${SALTS_GENERATOR}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args}
  RESULT_VARIABLE configured)
if(NOT configured EQUAL 0)
  message(FATAL_ERROR "installed CNet consumer configure failed: ${configured}")
endif()
set(build_args --build "${build}")
set(test_args --test-dir "${build}" --output-on-failure)
if(DEFINED SALTS_TEST_CONFIG AND NOT SALTS_TEST_CONFIG STREQUAL "")
  list(APPEND build_args --config "${SALTS_TEST_CONFIG}")
  list(APPEND test_args -C "${SALTS_TEST_CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${build_args}
  RESULT_VARIABLE built)
if(NOT built EQUAL 0)
  message(FATAL_ERROR "installed CNet consumer build failed: ${built}")
endif()
if(WIN32)
  set(ENV{PATH} "${prefix}/bin;$ENV{PATH}")
elseif(APPLE)
  if(DEFINED ENV{DYLD_LIBRARY_PATH} AND NOT "$ENV{DYLD_LIBRARY_PATH}" STREQUAL "")
    set(ENV{DYLD_LIBRARY_PATH} "${prefix}/lib:$ENV{DYLD_LIBRARY_PATH}")
  else()
    set(ENV{DYLD_LIBRARY_PATH} "${prefix}/lib")
  endif()
elseif(UNIX)
  if(DEFINED ENV{LD_LIBRARY_PATH} AND NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
    set(ENV{LD_LIBRARY_PATH} "${prefix}/lib:$ENV{LD_LIBRARY_PATH}")
  else()
    set(ENV{LD_LIBRARY_PATH} "${prefix}/lib")
  endif()
endif()
execute_process(COMMAND "${CTEST_COMMAND}" ${test_args}
  RESULT_VARIABLE tested)
if(NOT tested EQUAL 0)
  message(FATAL_ERROR "installed CNet consumer runtime failed: ${tested}")
endif()
