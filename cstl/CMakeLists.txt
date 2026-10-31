set(TARGET_NAME cmeta_cstl)

add_library(${TARGET_NAME}
  src/associative_meta.c
  src/construction_meta.c
  src/generic_meta.c
  src/instance_meta.c
  src/bplus_tree.c
  src/btree.c
  src/byte_buffer.c
  src/deque.c
  src/hash_map.c
  src/hash_set.c
  src/heap.c
  src/list.c
  src/map.c
  src/multimap.c
  src/rbtree.c
  src/sort.c
  src/set.c
  src/vec.c)

# A static CFlow/CSTL archive can be linked into more than one native DSO.
# Keep its reflected CMeta descriptor globals image-local, preserving public
# semantic type equality instead of making duplicate data symbols preemptible.
# This is a pure static-owner policy: explicitly requested shared libraries
# retain their existing export/link ABI and MUST be reviewed separately.
get_target_property(_salts_owner_type ${TARGET_NAME} TYPE)
if(_salts_owner_type STREQUAL "STATIC_LIBRARY")
  set_target_properties(${TARGET_NAME} PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
endif()
unset(_salts_owner_type)

cmake_config_target(
  ${TARGET_NAME}
  VERSION "${PROJECT_VERSION}" SOVERSION "${CMETA_NATIVE_SOVERSION}"
  ALIAS Salts::CSTL
  FOLDER "cstl"
  EXPORT_NAME CSTL)

target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
target_include_directories(
  ${TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(${TARGET_NAME} PUBLIC CMeta::CMeta)

add_library(cmeta_cstl_stream
  src/cflow_backend.c)
cmake_config_target(
  cmeta_cstl_stream
  VERSION "${PROJECT_VERSION}" SOVERSION "${CMETA_NATIVE_SOVERSION}"
  ALIAS Salts::CSTLStream
  FOLDER "cstl"
  EXPORT_NAME CSTLStream)
target_link_libraries(
  cmeta_cstl_stream
  PUBLIC Salts::CSTL CMeta::CFlow)
target_compile_features(cmeta_cstl_stream PUBLIC c_std_11)

install(
  TARGETS ${TARGET_NAME}
  EXPORT CMetaTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  TARGETS cmeta_cstl_stream
  EXPORT CMetaTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  DIRECTORY include/cstl
  DESTINATION include
  FILES_MATCHING PATTERN "*.h")

install(
  FILES include/cstl.h
  DESTINATION include)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
