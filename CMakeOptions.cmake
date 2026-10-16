include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)

# Address Sanitizer - only enabled for Debug builds
cmake_dependent_option(ENABLE_ASAN "Enable Address Sanitizer" ON
                       "CMAKE_BUILD_TYPE STREQUAL Debug" OFF)

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_BENCHMARKS "Build benchmark executables" ON)
option(NATIVE_IO_BUILD_BENCHMARKS
       "Build NativeIO benchmark executables independently" OFF)
option(COROUTINE_BUILD_BENCHMARKS
       "Build Coroutine benchmark executables independently" OFF)
option(CNET_BUILD_BENCHMARKS
       "Build CNet benchmark executables independently" OFF)
option(CFLOW_BUILD_BENCHMARKS
       "Build CFlow benchmark executables independently" OFF)
option(CONCURRENCY_BUILD_BENCHMARKS
       "Build Concurrency benchmark executables independently" OFF)
option(CFLOW_ENABLE_MINICORO
       "Build the optional minicoro-backed CFlow Resumable adapter" OFF)
option(CMETA_BUILD_BENCHMARKS
       "Build CMeta benchmarks independently" OFF)
option(CMETA_BUILD_NATIVE_THUNKS
       "Build opt-in exact x86-64 native specialization" OFF)
option(SALTS_ENABLE_COMPONENT_RUNTIME_CANDIDATE
       "Build isolated Salts Configurator/ServiceHost candidate (no SDK export)" OFF)

option(SALTS_ENABLE_EPOLL_READINESS
       "Enable the Linux epoll readiness backend" OFF)
if(SALTS_ENABLE_EPOLL_READINESS AND
   NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
          "SALTS_ENABLE_EPOLL_READINESS is supported only on Linux")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
