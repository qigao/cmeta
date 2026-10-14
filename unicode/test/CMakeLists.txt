cmake_add_test(test_salts_unicode_normalize
  SOURCES test_salts_unicode_normalize.c
  LIBS Salts::Unicode Salts::TinyTest
  DEFS SALTS_NFC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/NormalizationTest.txt"
  FOLDER "unicode/tests")
set_tests_properties(test_salts_unicode_normalize PROPERTIES LABELS "unicode;nfc;conformance")

cmake_add_test(test_salts_unicode
  SOURCES test_salts_unicode.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")

cmake_add_test(test_salts_unicode_header_cpp
  SOURCES test_salts_unicode_header_cpp.cpp
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")

set_tests_properties(test_salts_unicode PROPERTIES LABELS "unicode;unit")
set_tests_properties(test_salts_unicode_header_cpp PROPERTIES LABELS "unicode;headers")

# Compare every valid Unicode scalar against the checked-in Unicode 17
# UnicodeData.txt General_Category P*/S* oracle; fail on a changed category.
cmake_add_test(test_salts_unicode_category_conformance
  SOURCES test_salts_unicode_category_conformance.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
target_compile_definitions(test_salts_unicode_category_conformance PRIVATE
  SALTS_UNICODE_CATEGORY_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/UnicodeData.txt")
set_tests_properties(test_salts_unicode_category_conformance PROPERTIES
  LABELS "unicode;general-category;conformance")

cmake_add_test(test_salts_unicode_grapheme
  SOURCES test_salts_unicode_grapheme.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
set_tests_properties(test_salts_unicode_grapheme PROPERTIES LABELS "unicode;grapheme")

cmake_add_test(test_salts_unicode_grapheme_conformance
  SOURCES test_salts_unicode_grapheme_conformance.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
target_compile_definitions(test_salts_unicode_grapheme_conformance
  PRIVATE SALTS_UNICODE_GRAPHEME_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/GraphemeBreakTest.txt")
set_tests_properties(test_salts_unicode_grapheme_conformance
  PROPERTIES LABELS "unicode;grapheme;conformance")


cmake_add_test(test_salts_unicode_word
  SOURCES test_salts_unicode_word.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
set_tests_properties(test_salts_unicode_word PROPERTIES LABELS "unicode;word")

cmake_add_test(test_salts_unicode_word_conformance
  SOURCES test_salts_unicode_word_conformance.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
target_compile_definitions(test_salts_unicode_word_conformance
  PRIVATE SALTS_UNICODE_WORD_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/WordBreakTest.txt")
set_tests_properties(test_salts_unicode_word_conformance
  PROPERTIES LABELS "unicode;word;conformance")
cmake_add_test(test_salts_unicode_line_break_conformance
  SOURCES test_salts_unicode_line_break_conformance.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
target_compile_definitions(test_salts_unicode_line_break_conformance
  PRIVATE SALTS_UNICODE_LINE_BREAK_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/LineBreakTest.txt")
set_tests_properties(test_salts_unicode_line_break_conformance
  PROPERTIES LABELS "unicode;line-break;conformance")
cmake_add_test(test_salts_unicode_bidi
  SOURCES test_salts_unicode_bidi.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
set_tests_properties(test_salts_unicode_bidi
  PROPERTIES LABELS "unicode;bidi")


cmake_add_test(test_salts_unicode_bidi_conformance
  SOURCES test_salts_unicode_bidi_conformance.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
target_compile_definitions(test_salts_unicode_bidi_conformance
  PRIVATE SALTS_UNICODE_BIDI_PARAGRAPH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/BidiParagraphTest.txt")
set_tests_properties(test_salts_unicode_bidi_conformance
  PROPERTIES LABELS "unicode;bidi;conformance")


cmake_add_test(test_salts_unicode_emoji
  SOURCES test_salts_unicode_emoji.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
set_tests_properties(test_salts_unicode_emoji
  PROPERTIES LABELS "unicode;emoji")

cmake_add_test(test_salts_unicode_emoji_conformance
  SOURCES test_salts_unicode_emoji_conformance.c
  LIBS Salts::Unicode Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include
  FOLDER "unicode/tests")
target_compile_definitions(test_salts_unicode_emoji_conformance
  PRIVATE SALTS_UNICODE_EMOJI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../data/17.0.0/emoji-data.txt")
set_tests_properties(test_salts_unicode_emoji_conformance
  PROPERTIES LABELS "unicode;emoji;conformance")

# Validate the installed SDK, not just in-tree link aliases. The same consumers
# must compile as independent C11 and C++17 projects against Salts::Unicode.
# Cross-compiled Android/iOS builds do not execute hosted CTest.
if(NOT CMAKE_CROSSCOMPILING)
  # The parent preset remains the compiler/ABI/flags owner. Preserve its resolved
  # values for the existing independent consumer; do not select Release or the
  # host's default compiler again inside CTest.
  set(_unicode_consumer_cache "${CMAKE_CURRENT_BINARY_DIR}/installed-consumer-cache.cmake")
  file(WRITE "${_unicode_consumer_cache}" "# Generated from the configured Salts build.\n")
  set(_unicode_consumer_variables
    CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES CMAKE_TOOLCHAIN_FILE
    CMAKE_C_COMPILER CMAKE_CXX_COMPILER
    CMAKE_C_COMPILER_ARG1 CMAKE_CXX_COMPILER_ARG1
    CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET
    CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
    CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT
    CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_SYSROOT CMAKE_PREFIX_PATH
    VCPKG_TARGET_TRIPLET VCPKG_HOST_TRIPLET VCPKG_INSTALLED_DIR
    VCPKG_MANIFEST_DIR VCPKG_MANIFEST_FEATURES VCPKG_MANIFEST_NO_DEFAULT_FEATURES
    VCPKG_OVERLAY_PORTS VCPKG_OVERLAY_TRIPLETS VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
  foreach(_kind IN ITEMS C CXX EXE_LINKER)
    list(APPEND _unicode_consumer_variables "CMAKE_${_kind}_FLAGS")
    foreach(_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER "${_config}" _config_upper)
      list(APPEND _unicode_consumer_variables "CMAKE_${_kind}_FLAGS_${_config_upper}")
    endforeach()
  endforeach()
  # Reuse Sanitizers.cmake's already resolved directory options, including its
  # language/configuration expressions, without a second sanitizer flag policy.
  get_directory_property(SALTS_UNICODE_TEST_COMPILE_OPTIONS COMPILE_OPTIONS)
  get_directory_property(SALTS_UNICODE_TEST_LINK_OPTIONS LINK_OPTIONS)
  list(APPEND _unicode_consumer_variables
    SALTS_UNICODE_TEST_COMPILE_OPTIONS SALTS_UNICODE_TEST_LINK_OPTIONS)
  list(REMOVE_DUPLICATES _unicode_consumer_variables)
  foreach(_variable IN LISTS _unicode_consumer_variables)
    if(DEFINED ${_variable})
      # Bracket arguments preserve lists, backslashes, quotes and literal dollars.
      set(_bracket "=")
      while("${${_variable}}" MATCHES "]${_bracket}]")
        string(APPEND _bracket "=")
      endwhile()
      file(APPEND "${_unicode_consumer_cache}"
        "set(${_variable} [${_bracket}[${${_variable}}]${_bracket}] CACHE STRING \"Parent Salts configuration\")\n")
    endif()
  endforeach()
  add_test(
    NAME test_salts_unicode_installed_sdk
    COMMAND "${CMAKE_COMMAND}"
      "-DSALTS_BINARY_DIR=${CMAKE_BINARY_DIR}"
      "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install_consumer"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-unicode/$<CONFIG>"
      "-DSALTS_TEST_CONFIG=$<CONFIG>"
      "-DCONSUMER_CACHE=${_unicode_consumer_cache}"
      "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
      "-DSALTS_BUILD_GENERATOR=${CMAKE_GENERATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_installed_unicode.cmake")
  set_tests_properties(test_salts_unicode_installed_sdk PROPERTIES
    LABELS "unicode;installed;sdk"
    TIMEOUT 300
    RUN_SERIAL TRUE)
endif()
