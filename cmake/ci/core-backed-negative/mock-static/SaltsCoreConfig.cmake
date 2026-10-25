# Deliberately malformed package for NEGATIVE qualification only. The
# actual Salts profile must reject an imported STATIC Core.
foreach(_component IN ITEMS Core Platform Concurrency Coroutine NativeIO)
  if(_component STREQUAL "Core")
    add_library(SaltsCore::Core STATIC IMPORTED GLOBAL)
  else()
    add_library(SaltsCore::${_component} SHARED IMPORTED GLOBAL)
  endif()
endforeach()
