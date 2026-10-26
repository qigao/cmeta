# Count actual SaltsTargets export declarations; imported Core targets
# legitimately share the Salts:: namespace but are not owned by Salts.
if(NOT EXISTS "${Salts_DIR}/SaltsTargets.cmake")
  message(FATAL_ERROR "Salts source owner target export missing")
endif()
file(STRINGS "${Salts_DIR}/SaltsTargets.cmake" _lines
  REGEX "^add_(library|executable)\\(Salts::")
set(_actual)
foreach(_line IN LISTS _lines)
  string(REGEX MATCH "Salts::[A-Za-z0-9_]+" _target "${_line}")
  list(APPEND _actual "${_target}")
endforeach()
list(REMOVE_DUPLICATES _actual)
list(SORT _actual)
set(_expected
  Salts::CMeta Salts::ComponentABI Salts::PluginABI Salts::Plugin
  Salts::ComponentProviderABI Salts::Configurator
  Salts::ServiceHost Salts::CFlow)
list(SORT _expected)
if(NOT "${_actual}" STREQUAL "${_expected}")
  message(FATAL_ERROR
    "Only CMeta/ACE/CFlow may be exported by Salts: [${_actual}] != [${_expected}]")
endif()
