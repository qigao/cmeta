# Test-only static-hidden image owner policy. No product SDK default changes.
if(NOT CMAKE_PROJECT_NAME STREQUAL "Salts")
  message(FATAL_ERROR "private CFlow/CSTL overlay belongs only to Salts")
endif()
if(BUILD_SHARED_LIBS)
  message(FATAL_ERROR "private overlay must never run under shared-single")
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cstl PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cflow PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
