set(TARGET_NAME cmeta_cmeta)

add_library(${TARGET_NAME}
  src/abi.c
  src/cmeta.c
  src/component.c
  src/container_type.c
  src/data.c
  src/declared_type.c
  src/enum.c
  src/fingerprint.c
  src/manifest_view.c
  src/entry.c
  src/function.c
  src/fixed_array.c
  src/interface.c
  src/operation.c
  src/object.c
  src/infer.c
  src/invokable.c
  src/type_identity.c
  src/vector.c)

cmake_config_target(
  ${TARGET_NAME}
  VERSION "${PROJECT_VERSION}" SOVERSION "${CMETA_NATIVE_SOVERSION}"
  ALIAS CMeta::CMeta
  FOLDER "cmeta"
  EXPORT_NAME CMeta)

# A static CMeta archive may be embedded by Core and multiple bridge DSOs.
# Keep each image's immutable descriptors and helpers local instead of exposing
# preemptible duplicate globals. Type compatibility is semantic, not address-based.
# Do not apply hidden visibility to an explicitly requested shared CMeta target.
get_target_property(cmeta_library_type ${TARGET_NAME} TYPE)
if(cmeta_library_type STREQUAL "STATIC_LIBRARY")
  set_target_properties(${TARGET_NAME} PROPERTIES C_VISIBILITY_PRESET hidden)
  target_compile_definitions(${TARGET_NAME} PRIVATE
    "$<$<COMPILE_LANGUAGE:ASM>:CMETA_STATIC_LIBRARY=1>")
endif()

target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
target_compile_options(${TARGET_NAME}
  PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:preprocessor>)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

install(
  TARGETS ${TARGET_NAME}
  EXPORT CMetaTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  DIRECTORY include/cmeta
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

if(CMETA_BUILD_NATIVE_THUNKS)
  add_subdirectory(native)
endif()

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(BUILD_BENCHMARKS OR CMETA_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
