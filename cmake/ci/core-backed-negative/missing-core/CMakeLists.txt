cmake_minimum_required(VERSION 3.20)
project(Salts3MissingCoreMustFail LANGUAGES C CXX)
# An external consumer discovers only SaltsConfig. With the native package
# deliberately disabled, find_dependency(SaltsCore 0.1 REQUIRED) must fail.
find_package(Salts 3.0 CONFIG REQUIRED)
message(FATAL_ERROR "A Salts 3.0 client unexpectedly omitted Core dependency")
