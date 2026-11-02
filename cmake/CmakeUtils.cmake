# Project-independent target and executable helpers.
include_guard(GLOBAL)

function(_cmake_check_arguments caller prefix)
    if(DEFINED ${prefix}_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${caller}: unknown arguments: ${${prefix}_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED ${prefix}_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${caller}: missing values for: ${${prefix}_KEYWORDS_MISSING_VALUES}")
    endif()
endfunction()

# Only explicitly supplied attributes are changed. VERSION/SOVERSION, including
# zero, belong to the caller's ABI policy; omitted attributes retain their value.
function(cmake_config_target target_name)
    set(options)
    set(oneValueArgs FOLDER VERSION SOVERSION EXPORT_NAME ALIAS OUTPUT_NAME)
    set(multiValueArgs)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
    _cmake_check_arguments(cmake_config_target ARG)

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "cmake_config_target: target '${target_name}' does not exist")
    endif()
    get_target_property(target_type "${target_name}" TYPE)

    if(DEFINED ARG_ALIAS)
        if(target_type STREQUAL "EXECUTABLE")
            add_executable(${ARG_ALIAS} ALIAS ${target_name})
        else()
            add_library(${ARG_ALIAS} ALIAS ${target_name})
        endif()
    endif()

    foreach(attribute IN ITEMS FOLDER EXPORT_NAME OUTPUT_NAME VERSION SOVERSION)
        if(DEFINED ARG_${attribute})
            set_target_properties("${target_name}" PROPERTIES
                ${attribute} "${ARG_${attribute}}")
        endif()
    endforeach()
endfunction()

function(_cmake_create_executable target_name sources libraries definitions includes folder)
  if(TARGET "${target_name}")
    message(FATAL_ERROR "Cannot create executable: target '${target_name}' already exists")
  endif()
  if("${sources}" STREQUAL "")
    message(FATAL_ERROR "Cannot create executable: SOURCES is required for '${target_name}'")
  endif()

  add_executable("${target_name}" ${sources})
  if(NOT "${libraries}" STREQUAL "")
    target_link_libraries("${target_name}" PRIVATE ${libraries})
  endif()
  if(NOT "${definitions}" STREQUAL "")
    target_compile_definitions("${target_name}" PRIVATE ${definitions})
  endif()
  if(NOT "${includes}" STREQUAL "")
    target_include_directories("${target_name}" PRIVATE ${includes})
  endif()
  if(NOT "${folder}" STREQUAL "")
    cmake_config_target("${target_name}" FOLDER "${folder}")
  endif()
endfunction()

# Executable helpers require SOURCES. LIBS, DEFS and INCLUDES are PRIVATE;
# FOLDER is optional. They never choose dependencies, flags or installation rules.
function(cmake_add_executable target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_executable ARG)
  _cmake_create_executable("${target_name}" "${ARG_SOURCES}" "${ARG_LIBS}"
    "${ARG_DEFS}" "${ARG_INCLUDES}" "${ARG_FOLDER}")
endfunction()

# The caller enables testing and owns additional CTest properties.
function(cmake_add_test target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_test ARG)
  _cmake_create_executable("${target_name}" "${ARG_SOURCES}" "${ARG_LIBS}"
    "${ARG_DEFS}" "${ARG_INCLUDES}" "${ARG_FOLDER}")
  add_test(NAME "${target_name}" COMMAND "${target_name}")
endfunction()

# Benchmark registration and runtime arguments belong to the caller.
function(cmake_add_benchmark target_name)
  set(options)
  set(oneValueArgs FOLDER)
  set(multiValueArgs SOURCES LIBS DEFS INCLUDES)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "${options}" "${oneValueArgs}" "${multiValueArgs}")
  _cmake_check_arguments(cmake_add_benchmark ARG)
  _cmake_create_executable("${target_name}" "${ARG_SOURCES}" "${ARG_LIBS}"
    "${ARG_DEFS}" "${ARG_INCLUDES}" "${ARG_FOLDER}")
endfunction()
