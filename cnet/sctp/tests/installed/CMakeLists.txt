cmake_minimum_required(VERSION 3.20)
project(CNetSctpInstalledConsumer LANGUAGES C)
find_package(CNetSctpPrototype 0.1 CONFIG REQUIRED)
add_executable(cnet_sctp_installed_consumer main.c)
target_link_libraries(cnet_sctp_installed_consumer PRIVATE
  CNetSctpPrototype::Session)
set_target_properties(cnet_sctp_installed_consumer PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
enable_testing()
add_test(NAME cnet_sctp_installed_consumer COMMAND cnet_sctp_installed_consumer)
