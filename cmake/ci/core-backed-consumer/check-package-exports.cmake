# Inspect the actual installed SaltsTargets import set. The reduced 3.0
# profile promises exactly these eight ORIGINAL production targets and no
# historical Runtime owner or test harness accidentally mixed into the SDK.
get_property(_salts3_imported DIRECTORY PROPERTY IMPORTED_TARGETS)
set(_salts3_actual)
foreach(_import IN LISTS _salts3_imported)
  if(_import MATCHES "^Salts::")
    list(APPEND _salts3_actual "${_import}")
  endif()
endforeach()
list(SORT _salts3_actual)
set(_salts3_expected
  Salts::CMeta
  Salts::ComponentABI
  Salts::PluginABI
  Salts::Plugin
  Salts::ComponentProviderABI
  Salts::Configurator
  Salts::ServiceHost
  Salts::CFlow)
list(SORT _salts3_expected)
if(NOT "${_salts3_actual}" STREQUAL "${_salts3_expected}")
  message(FATAL_ERROR
    "Salts 3.0 exported target closure differs from the qualified eight: "
    "actual [${_salts3_actual}], expected [${_salts3_expected}]")
endif()
message(STATUS "Salts 3.0 exact eight-target production export set: PASS")
