# TinyMock is the CMeta-aware mocking component.
#
#   CMeta::TinyMock -> Salts::TinyTest + CMeta::CMeta
#
# TinyTest owns no mock runtime or reflection schema. TinyMock owns only
# reflected history/actions/returns and consumes canonical CMeta metadata.

add_library(tinymock STATIC
  src/tinymock_value.c
  src/tinymock_history.c
  src/tinymock_reflect.c
  src/tinymock_actions.c
  src/tinymock_return.c)

cmake_config_target(tinymock
  VERSION "${PROJECT_VERSION}" SOVERSION "${CMETA_NATIVE_SOVERSION}"
  ALIAS CMeta::TinyMock
  FOLDER "tinymock"
  EXPORT_NAME TinyMock)

target_compile_features(tinymock PUBLIC c_std_11)

target_link_libraries(tinymock
  PUBLIC
    Salts::TinyTest
    CMeta::CMeta)

target_include_directories(tinymock
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

install(
  TARGETS tinymock
  EXPORT CMetaTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT TinyMock)

install(
  FILES
    include/tinymock.h
    include/tinymock_support.h
    include/tinymock_cmeta.h
    include/tinymock_value.h
    include/tinymock_function.h
    include/tinymock_history.h
    include/tinymock_reflect.h
    include/tinymock_actions.h
    include/tinymock_return.h
  DESTINATION include
  COMPONENT TinyMock)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
