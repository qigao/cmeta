cmake_minimum_required(VERSION 3.20)
project(CNetSctpIndependentPionInstalledConsumer LANGUAGES C)

# Standalone Linux test executable: strictly consumes the previously
# INSTALLED public opaque Session package, never the Salts source-tree
# Association/DATA layouts or the production SaltsTargets.
find_package(CNetSctpPrototype CONFIG REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
add_executable(cnet_sctp_pion_oracle_peer
  ../pion_peer.c
  ../pion_dtls_transport.c)
target_link_libraries(cnet_sctp_pion_oracle_peer
  PRIVATE CNetSctpPrototype::Session OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(cnet_sctp_pion_oracle_peer PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(cnet_sctp_pion_oracle_peer PRIVATE /W4 /WX)
else()
  target_compile_options(cnet_sctp_pion_oracle_peer PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
