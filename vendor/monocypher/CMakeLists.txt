# Monocypher - A high-quality cryptographic library https://monocypher.org/

# Compile the unmodified vendor source once through the CNet-private namespace.
# A raw Monocypher archive embedded into more than one DSO creates symbol/ASan
# ODR collisions, including crypto_argon2_no_extras.
add_library(monocypher STATIC
  cnet_monocypher.c cnet_monocypher_namespace.h monocypher.h)

target_include_directories(
  monocypher PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

set_target_properties(monocypher PROPERTIES
  FOLDER "vendor"
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
