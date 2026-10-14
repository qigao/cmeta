# Use the same host re2c binary and Unicode 17 property tables provided by
# the canonical vcpkg-cache setup-re2c-tools action.  Runtime consumers do
# not require re2c, ICU, SaltsUtils or external Unicode data files.
if(NOT DEFINED ENV{RE2C_ROOT} OR NOT IS_DIRECTORY "$ENV{RE2C_ROOT}")
  message(FATAL_ERROR "Salts::Unicode requires RE2C_ROOT with Unicode 17 re2c stdlib")
endif()
set(RE2C_ROOT "$ENV{RE2C_ROOT}")
set(RE2C_UNICODE_PROPERTIES "${RE2C_ROOT}/share/re2c/stdlib/unicode_properties.re")
set(RE2C_UNICODE_CATEGORIES "${RE2C_ROOT}/share/re2c/stdlib/unicode_categories.re")
foreach(_table IN ITEMS "${RE2C_UNICODE_PROPERTIES}" "${RE2C_UNICODE_CATEGORIES}")
  if(NOT EXISTS "${_table}")
    message(FATAL_ERROR "Required Unicode 17 re2c table is missing: ${_table}")
  endif()
endforeach()
# Generated Unicode 17 semantics cannot silently change when the *latest* re2c
# tool package upgrades. These content hashes are for the Unicode 17.0.0
# upstream re2c 4.6 stdlib data, not a pin on the re2c tool package version.
# Normalize Windows CRLF (Git checkout) to canonical LF before hashing.
file(READ "${RE2C_UNICODE_PROPERTIES}" _unicode17_properties)
string(REPLACE "\r\n" "\n" _unicode17_properties "${_unicode17_properties}")
string(SHA256 _unicode17_properties_sha256 "${_unicode17_properties}")
if(NOT _unicode17_properties_sha256 STREQUAL
       "56b5f16e4e516a598125760ee7f4fae60a5db6727b8cf3dfb5f020152ef2aa21")
  message(FATAL_ERROR
    "re2c Unicode property database differs from canonical Unicode 17.0.0; "
    "explicit data/API/conformance upgrade required")
endif()
file(READ "${RE2C_UNICODE_CATEGORIES}" _unicode17_categories)
string(REPLACE "\r\n" "\n" _unicode17_categories "${_unicode17_categories}")
string(SHA256 _unicode17_categories_sha256 "${_unicode17_categories}")
if(NOT _unicode17_categories_sha256 STREQUAL
       "3d0a319806e42a7b476f9d474c104f7e734c42eeab8fb36b2b82591cd55b0689")
  message(FATAL_ERROR
    "re2c Unicode category database differs from canonical Unicode 17.0.0; "
    "explicit data/API/conformance upgrade required")
endif()
unset(_unicode17_properties)
unset(_unicode17_categories)
unset(_unicode17_properties_sha256)
unset(_unicode17_categories_sha256)

# Default Full Case Folding belongs to the same pinned Unicode 17.0.0
# database as re2c properties/categories. Both authoritative source bytes and
# the committed generated C11 table are immutable release inputs. Normalize
# checkout CRLF before hashing (Windows); regeneration requires an explicit
# new data/API review, never a silent SDK semantic change.
set(_unicode17_casefold_source "${CMAKE_CURRENT_SOURCE_DIR}/data/17.0.0/CaseFolding.txt")
set(_unicode17_casefold_generated "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_casefold_data.h")
foreach(_fixture IN ITEMS "${_unicode17_casefold_source}" "${_unicode17_casefold_generated}")
  if(NOT EXISTS "${_fixture}")
    message(FATAL_ERROR "Required Unicode 17 casefold release fixture is missing: ${_fixture}")
  endif()
endforeach()
file(READ "${_unicode17_casefold_source}" _unicode17_casefold_source_text)
string(REPLACE "\r\n" "\n" _unicode17_casefold_source_text "${_unicode17_casefold_source_text}")
string(SHA256 _unicode17_casefold_source_sha256 "${_unicode17_casefold_source_text}")
if(NOT _unicode17_casefold_source_sha256 STREQUAL
       "ff8d8fefbf123574205085d6714c36149eb946d717a0c585c27f0f4ef58c4183")
  message(FATAL_ERROR
    "Unicode 17 CaseFolding.txt differs from pinned authoritative C/F data; "
    "explicit data/API/provenance review required")
endif()
file(READ "${_unicode17_casefold_generated}" _unicode17_casefold_generated_text)
string(REPLACE "\r\n" "\n" _unicode17_casefold_generated_text "${_unicode17_casefold_generated_text}")
string(SHA256 _unicode17_casefold_generated_sha256 "${_unicode17_casefold_generated_text}")
if(NOT _unicode17_casefold_generated_sha256 STREQUAL
       "9a262099e14fa7520bfc12d491da271260ba246a722190f2ad49b551bdb8a14b")
  message(FATAL_ERROR
    "Generated Unicode 17 Default Full Case Folding table differs from "
    "its qualified immutable fixture; rerun generator and qualify changes")
endif()
unset(_unicode17_casefold_source)
unset(_unicode17_casefold_generated)
unset(_unicode17_casefold_source_text)
unset(_unicode17_casefold_generated_text)
unset(_unicode17_casefold_source_sha256)
unset(_unicode17_casefold_generated_sha256)
unset(_fixture)

execute_process(
  COMMAND "${RE2C_EXECUTABLE}" --version
  RESULT_VARIABLE _re2c_exit
  OUTPUT_VARIABLE _re2c_output
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _re2c_exit EQUAL 0 OR
   NOT _re2c_output MATCHES "re2c ([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
  message(FATAL_ERROR "Cannot determine re2c version for Salts::Unicode")
endif()
set(_re2c_version "${CMAKE_MATCH_1}")
if(_re2c_version VERSION_LESS 4.6)
  message(FATAL_ERROR "Salts::Unicode requires re2c 4.6 or newer (found ${_re2c_version})")
endif()
get_filename_component(_re2c_actual "${RE2C_EXECUTABLE}" REALPATH)
get_filename_component(_re2c_root_actual "${RE2C_ROOT}" REALPATH)
cmake_path(IS_PREFIX _re2c_root_actual "${_re2c_actual}" NORMALIZE _unicode_re2c_in_root)
if(NOT _unicode_re2c_in_root)
  message(FATAL_ERROR "re2c executable does not belong to RE2C_ROOT: ${RE2C_EXECUTABLE}")
endif()

set(SALTS_UNICODE_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/src/salts_unicode.re)
set(SALTS_UNICODE_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/salts_unicode_gen.c)

add_custom_command(
  OUTPUT ${SALTS_UNICODE_LEXER_GEN}
  COMMAND
    ${RE2C_EXECUTABLE} --lang c --utf8 --encoding-policy fail
    --no-generation-date -W -Werror -I "${RE2C_ROOT}/share/re2c/stdlib" -o
    ${SALTS_UNICODE_LEXER_GEN} ${SALTS_UNICODE_LEXER_RE}
  DEPENDS ${SALTS_UNICODE_LEXER_RE} ${RE2C_UNICODE_PROPERTIES}
          ${RE2C_UNICODE_CATEGORIES}
  COMMENT "Generating Unicode 17.0.0 UTF-8 scanner with re2c"
  VERBATIM)

add_library(
  salts_unicode STATIC
  include/salts_unicode.h src/salts_unicode_case.c src/unicode_case_data.h
  src/salts_unicode_casefold.c src/unicode_casefold_data.h
  include/salts_unicode_normalize.h src/salts_unicode_normalize.c src/unicode_normalization_data.h
  src/salts_unicode_grapheme.c src/unicode_grapheme_data.h
  src/salts_unicode_word.c src/unicode_word_data.h
  src/salts_unicode_line_break.c src/unicode_line_break_data.h
  src/salts_unicode_bidi.c src/unicode_bidi_data.h
  src/salts_unicode_emoji.c src/salts_unicode_emoji.h src/unicode_emoji_data.h
  src/salts_unicode_names.c ${SALTS_UNICODE_LEXER_GEN})
cmake_config_target(
  salts_unicode
  ALIAS
  Salts::Unicode
  FOLDER
  "unicode"
  EXPORT_NAME
  Unicode)

target_include_directories(
  salts_unicode
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/unicode>)
target_link_libraries(salts_unicode PUBLIC Salts::Core)
target_compile_features(salts_unicode PUBLIC c_std_11)

install(
  TARGETS salts_unicode
  EXPORT SaltsTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/salts_unicode.h include/salts_unicode_normalize.h DESTINATION include/unicode)
install(FILES data/LICENSE.txt
        DESTINATION share/licenses/salts_unicode)

if(BUILD_TESTS)
  add_subdirectory(test)
endif()
