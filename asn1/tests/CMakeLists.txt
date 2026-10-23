cmake_add_test(asn1_reader_test
  SOURCES asn1_reader_test.c
  LIBS Salts::ASN1 Salts::TinyTest
  FOLDER "asn1/tests")

cmake_add_test(asn1_header_cpp_test
  SOURCES asn1_header_cpp_test.cpp
  LIBS Salts::ASN1 Salts::TinyTest
  FOLDER "asn1/tests")

cmake_add_test(asn1_value_test
  SOURCES asn1_value_test.c
  LIBS Salts::ASN1 Salts::TinyTest
  FOLDER "asn1/tests")

set_target_properties(asn1_reader_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
set_target_properties(asn1_header_cpp_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

set_target_properties(asn1_value_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
