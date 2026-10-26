cmake_minimum_required(VERSION 3.20)
project(CNetSctpOpaqueRealDtlsConsumer LANGUAGES C)
include(CTest)
find_package(CNetSctpPrototype CONFIG REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
add_executable(cnet_sctp_opaque_real_dtls_consumer ../dtls_records.c)
target_link_libraries(cnet_sctp_opaque_real_dtls_consumer PRIVATE
  CNetSctpPrototype::Session OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(cnet_sctp_opaque_real_dtls_consumer PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(cnet_sctp_opaque_real_dtls_consumer PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_opaque_real_dtls_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME cnet_sctp_opaque_real_dtls_consumer
  COMMAND cnet_sctp_opaque_real_dtls_consumer)
