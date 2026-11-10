cmake_add_test(tinymock_reflect_test
  SOURCES tinymock_reflect_test.c
  LIBS CMeta::TinyMock
  FOLDER "tinymock/tests")
cmake_add_test(tinymock_reflect_cpp_test
  SOURCES tinymock_reflect_cpp_test.cpp
  LIBS CMeta::TinyMock
  FOLDER "tinymock/tests")
