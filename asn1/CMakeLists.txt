include(GNUInstallDirs)

set(TARGET_NAME salts_asn1)

add_library(${TARGET_NAME} SHARED src/asn1.c src/value.c)
cmake_config_target(
  ${TARGET_NAME}
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${SALTS_NATIVE_SOVERSION}"
  ALIAS Salts::ASN1
  FOLDER "asn1"
  EXPORT_NAME ASN1)

target_compile_features(${TARGET_NAME} PUBLIC c_std_11)
target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_compile_definitions(${TARGET_NAME}
  PRIVATE "$<$<PLATFORM_ID:Windows>:SALTS_ASN1_API=__declspec(dllexport)>"
  INTERFACE "$<$<PLATFORM_ID:Windows>:SALTS_ASN1_API=__declspec(dllimport)>")

install(TARGETS ${TARGET_NAME} EXPORT SaltsTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/salts DESTINATION include FILES_MATCHING PATTERN "*.h")

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
