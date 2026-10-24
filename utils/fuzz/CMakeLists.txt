if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "SALTS_BUILD_CRYPTO_FUZZERS requires Clang libFuzzer")
endif()

function(salts_add_crypto_fuzzer target source corpus)
  set(runtime_corpus "${CMAKE_CURRENT_BINARY_DIR}/${target}-corpus")
  file(COPY "${corpus}/" DESTINATION "${runtime_corpus}")
  add_executable(${target} ${source})
  target_compile_features(${target} PRIVATE c_std_11)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${target} PRIVATE Salts::Core)
  set_target_properties(${target} PROPERTIES FOLDER "utils/fuzz")
  if(BUILD_TESTING)
    add_test(NAME ${target}
      COMMAND ${target} -runs=20000 -max_len=4096 ${runtime_corpus})
    set_tests_properties(${target} PROPERTIES
      LABELS "crypto;fuzz"
      TIMEOUT 180)
  endif()
endfunction()

salts_add_crypto_fuzzer(
  fuzz_cmeta_sm4_gcm cmeta_sm4_gcm_fuzz.c
  ${CMAKE_CURRENT_SOURCE_DIR}/corpus/sm4_gcm)
salts_add_crypto_fuzzer(
  fuzz_cmeta_sm2_verify cmeta_sm2_verify_fuzz.c
  ${CMAKE_CURRENT_SOURCE_DIR}/corpus/sm2_verify)
