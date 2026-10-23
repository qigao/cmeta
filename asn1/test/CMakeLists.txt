set(TEST_DER_TARGET test_asn1_der)

add_executable(${TEST_DER_TARGET} test_asn1_der.c)

target_link_libraries(${TEST_DER_TARGET} PRIVATE asn1_parser Salts::TinyTest)

add_test(NAME ${TEST_DER_TARGET} COMMAND ${TEST_DER_TARGET})

set_target_properties(${TEST_DER_TARGET} PROPERTIES FOLDER snmp/asn1_parser)

# Crypto Random tests
set(TEST_RANDOM_TARGET test_crypto_random)

add_executable(${TEST_RANDOM_TARGET} test_crypto_random.c)

target_link_libraries(${TEST_RANDOM_TARGET} PRIVATE asn1_parser Salts::TinyTest)

add_test(NAME ${TEST_RANDOM_TARGET} COMMAND ${TEST_RANDOM_TARGET})

set_target_properties(${TEST_RANDOM_TARGET} PROPERTIES FOLDER snmp/asn1_parser)

# X.509 Integration tests
set(TEST_X509_INTEGRATION_TARGET test_x509_integration)

add_executable(${TEST_X509_INTEGRATION_TARGET} test_x509_integration.c)

target_link_libraries(${TEST_X509_INTEGRATION_TARGET} PRIVATE asn1_parser Salts::TinyTest)

add_test(NAME ${TEST_X509_INTEGRATION_TARGET}
         COMMAND ${TEST_X509_INTEGRATION_TARGET})

set_target_properties(${TEST_X509_INTEGRATION_TARGET}
                      PROPERTIES FOLDER snmp/asn1_parser)

# BER Features tests (indefinite length, constructed primitives)
set(TEST_BER_TARGET test_ber_features)

add_executable(${TEST_BER_TARGET} test_ber_features.c)

target_link_libraries(${TEST_BER_TARGET} PRIVATE asn1_parser Salts::TinyTest)

add_test(NAME ${TEST_BER_TARGET} COMMAND ${TEST_BER_TARGET})

set_target_properties(${TEST_BER_TARGET} PROPERTIES FOLDER snmp/asn1_parser)
