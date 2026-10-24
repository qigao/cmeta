# Monocypher - A high-quality cryptographic library https://monocypher.org/

# Compile the vendored source once through the CNet-private namespace.
# A raw Monocypher archive embedded into more than one DSO creates symbol/ASan
# ODR collisions, including crypto_argon2_no_extras.
add_library(monocypher STATIC
  cnet_monocypher.c cnet_monocypher_namespace.h monocypher.h)

add_library(core_monocypher STATIC
  core_monocypher.c core_monocypher_ed25519.c
  core_monocypher_namespace.h monocypher.h
  monocypher-ed25519.h)

target_include_directories(
  monocypher PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_include_directories(
  core_monocypher PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

set_target_properties(monocypher PROPERTIES
  FOLDER "vendor"
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
set_target_properties(core_monocypher PROPERTIES
  FOLDER "vendor"
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
