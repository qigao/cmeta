# Static CSTL module-private native test only; published Salts rc unchanged.
if(NOT CMAKE_PROJECT_NAME STREQUAL "Salts" OR BUILD_SHARED_LIBS)
  message(FATAL_ERROR "Only static Salts permits the private CSTL test overlay")
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cstl PROPERTIES
  C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
