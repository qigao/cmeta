cmake_minimum_required(VERSION 3.20)
project(salts_asn1_installed_consumer LANGUAGES C CXX)

find_package(Salts CONFIG REQUIRED)

enable_testing()

add_executable(asn1_installed_c11 asn1_installed_c11.c)
target_compile_features(asn1_installed_c11 PRIVATE c_std_11)
target_link_libraries(asn1_installed_c11 PRIVATE Salts::ASN1)
set_target_properties(asn1_installed_c11 PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME asn1_installed_c11 COMMAND asn1_installed_c11)

add_executable(asn1_installed_cpp17 asn1_installed_cpp17.cpp)
target_compile_features(asn1_installed_cpp17 PRIVATE cxx_std_17)
target_link_libraries(asn1_installed_cpp17 PRIVATE Salts::ASN1)
set_target_properties(asn1_installed_cpp17 PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
add_test(NAME asn1_installed_cpp17 COMMAND asn1_installed_cpp17)
