cmake_minimum_required(VERSION 3.20)
project(plugin_abi_installed_consumer LANGUAGES C)

find_package(CMeta CONFIG REQUIRED)

if(NOT TARGET CMeta::PluginABI)
  message(FATAL_ERROR "installed package is missing CMeta::PluginABI")
endif()
if(NOT TARGET CMeta::Plugin)
  message(FATAL_ERROR "installed package is missing CMeta::Plugin")
endif()

foreach(retired_target IN ITEMS
    Salts::BindingsCpp
    Salts::Lua
    Salts::QuickJS
    Salts::CBind
    Salts::PluginCFlow
    Salts::PluginCFlowABI)
  if(TARGET ${retired_target})
    message(FATAL_ERROR
      "installed Salts 1.8 package still exports retired target ${retired_target}")
  endif()
endforeach()

get_target_property(plugin_abi_type CMeta::PluginABI TYPE)
if(NOT plugin_abi_type STREQUAL "INTERFACE_LIBRARY")
  message(FATAL_ERROR
    "CMeta::PluginABI must remain header/CMeta-only, got ${plugin_abi_type}")
endif()

add_executable(plugin_abi_installed_consumer main.c)
target_link_libraries(plugin_abi_installed_consumer PRIVATE CMeta::PluginABI)
target_compile_features(plugin_abi_installed_consumer PRIVATE c_std_11)
