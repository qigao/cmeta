# Applied ONLY when configuring pinned Salts with static CFlow.
# To keep the comparison about CFlow only, the separately-proven CSTL
# static-hidden owner is the common baseline for both static CFlow modes.
# Shared-single uses true BUILD_SHARED_LIBS=ON with NO hidden overlays.
if(NOT CMAKE_PROJECT_NAME STREQUAL "Salts")
  message(FATAL_ERROR "CFlow ODR overlay is only valid in project(Salts)")
endif()
if(BUILD_SHARED_LIBS)
  message(FATAL_ERROR "Shared-single mode must not include a hidden overlay")
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cstl PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
if(CFLOW_DSO_HIDE_CFLOW)
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    set_target_properties cmeta_cflow PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
endif()
