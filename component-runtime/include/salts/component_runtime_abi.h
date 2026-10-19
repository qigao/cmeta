/* Native ABI gate for the canonical Configurator/ServiceHost runtime.
 * Epoch 1.0 is the first ABI contract for this runtime; it is not compatible
 * with SDKs that do not provide these symbols and layouts. */
#ifndef SALTS_COMPONENT_RUNTIME_ABI_H
#define SALTS_COMPONENT_RUNTIME_ABI_H
#include <stdint.h>
#include <stddef.h>
#define SALTS_COMPONENT_RUNTIME_ABI_MAJOR 1u
#define SALTS_COMPONENT_RUNTIME_ABI_MINOR 0u
#define SALTS_COMPONENT_RUNTIME_ABI_VERSION 0x00010000u
#ifdef __cplusplus
extern "C" {
#endif
/* Link and execute against the library itself; do not infer the library ABI
 * merely from a matching installed header or CMake target. */
uint32_t salts_configurator_abi_version(void);
uint32_t salts_service_host_abi_version(void);
/* Exact-epoch link marker: an ABI-major mismatch must fail at link time,
 * including on Android/iOS where host execution is not available. */
void salts_service_host_abi_v1_0_required(void);
/* Runtime layout assertions detect a mismatched header/archive pair. */
size_t salts_configurator_abi_context_size(void);
size_t salts_service_host_abi_scope_size(void);
#ifdef __cplusplus
}
#endif
#endif /* SALTS_COMPONENT_RUNTIME_ABI_H */
