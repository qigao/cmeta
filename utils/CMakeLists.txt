set(TARGET_NAME salts)
# Add parser subdirectory first (generates lexer)
add_subdirectory(parser)
cmake_add_source(SOURCE_FILES RECURSE
  DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
find_path(SIMDE_INCLUDE_DIRS "simde/x86/sse2.h" REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(base64 CONFIG REQUIRED)
find_package(GmSSL CONFIG REQUIRED)

add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
# Configure target
cmake_config_target(
  ${TARGET_NAME}
  VERSION "${PROJECT_VERSION}" SOVERSION "${SALTS_NATIVE_SOVERSION}"
  ALIAS
  Salts::Core
  FOLDER
  "utils"
  EXPORT_NAME
  Core)

# Salts::ASN1 is an implementation-only DSO dependency of Core.  Keep it out
# of Core's public link interface, while making the installed Linux SDK
# relocatable: DT_RUNPATH on an executable is not used to resolve an indirect
# dependency such as Core -> ASN1.
if(UNIX AND NOT APPLE)
  set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
         $<INSTALL_INTERFACE:include>
  PRIVATE $<BUILD_INTERFACE:${SIMDE_INCLUDE_DIRS}>
          $<BUILD_INTERFACE:${FMT_LEXER_INCLUDE_DIRS}>
          $<BUILD_INTERFACE:${LOG_PATTERN_LEXER_INCLUDE_DIRS}>)

# Keep shared-library producer/consumer state on Salts::Core itself.
# The INTERFACE definition is exported into SaltsTargets.cmake, so only
# targets that link Salts::Core receive the consumer-side import marker.
target_compile_definitions(
  ${TARGET_NAME}
  PUBLIC
    $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>,$<CONFIG:RelWithDebInfo>>:SALTS_LOG_CAPTURE_SOURCE=0>
  PRIVATE
    "$<$<PLATFORM_ID:Windows>:SALTS_API=__declspec(dllexport)>"
  INTERFACE
    "$<$<PLATFORM_ID:Windows>:SALTS_API=__declspec(dllimport)>")
target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
target_compile_options(${TARGET_NAME}
  PUBLIC $<$<COMPILE_LANG_AND_ID:C,MSVC>:-experimental:c11atomics>)
if(APPLE)
  target_compile_definitions(${TARGET_NAME} PRIVATE _DARWIN_C_SOURCE=1)
elseif(UNIX)
  target_compile_definitions(
    ${TARGET_NAME} PRIVATE _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L
                           _XOPEN_SOURCE=700)
endif()

# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Salts::CMeta
         Salts::Platform
         Salts::Concurrency
         Salts::Coroutine
  PRIVATE Salts::ASN1
          Salts::CSTL
          Salts::CFlow
          ${FMT_LEXER_LIBRARY} Salts::SDS # Simple Dynamic Strings
          aklomp::base64 zstd::libzstd GmSSL::GmSSL
          ${LOG_PATTERN_LEXER_LIBRARY} # Log pattern lexer
)

if(WIN32)
  target_link_libraries(${TARGET_NAME} PRIVATE iphlpapi ws2_32)
endif()

install(
  TARGETS ${TARGET_NAME}
  EXPORT SaltsTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install headers
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h")

# Install extra parser header
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/parser/fmt_lexer.h
        DESTINATION include)
# Tests
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(SALTS_BUILD_CRYPTO_FUZZERS)
  add_subdirectory(fuzz)
endif()
if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
