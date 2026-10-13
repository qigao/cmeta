# Loaded only for project(Salts) of an immutable, pinned test checkout.
# Defer until after cstl/CMakeLists.txt declares the *static* cmeta_cstl
# target. Do not mutate production source, installed package or ABI.
if(NOT CMAKE_PROJECT_NAME STREQUAL "Salts")
  message(FATAL_ERROR "CSTL visibility overlay is only valid inside project(Salts)")
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cstl PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
