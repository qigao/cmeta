foreach(example_name IN ITEMS
    cflow_native_socket_example
    cflow_native_pipe_example
    cflow_native_file_example)
  cmake_add_test(
    ${example_name}
    SOURCES ${example_name}.c
    LIBS CMeta::CFlow
    FOLDER "cflow/examples")
  set_tests_properties(${example_name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

if(WIN32)
  target_link_libraries(cflow_native_socket_example PRIVATE ws2_32)
endif()
