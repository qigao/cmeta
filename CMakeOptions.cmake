# Every real Salts build is CMeta/ACE/CFlow. No legacy native profile.
option(BUILD_TESTS "Old source-tree tests (unsupported)" OFF)
option(BUILD_EXAMPLES "Old examples (unsupported)" OFF)
option(BUILD_BENCHMARKS "Old benchmarks (unsupported)" OFF)
option(CFLOW_BUILD_BENCHMARKS "Old CFlow benchmarks (unsupported)" OFF)
option(CMETA_BUILD_NATIVE_THUNKS "Old native thunks (unsupported)" OFF)
option(CFLOW_ENABLE_MINICORO "Second Coroutine owner (unsupported)" OFF)
if(BUILD_TESTS OR BUILD_EXAMPLES OR BUILD_BENCHMARKS OR
   CFLOW_BUILD_BENCHMARKS OR CMETA_BUILD_NATIVE_THUNKS OR
   CFLOW_ENABLE_MINICORO OR BUILD_SHARED_LIBS)
  message(FATAL_ERROR "No alternate native/compatibility Salts build; use installed SDK conformance")
endif()
