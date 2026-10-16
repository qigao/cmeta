/* ABI gate for the opt-in, nonpublished Configurator/ServiceHost SDK.
 * Salts #1153; initial public candidate epoch 1. No 2.3 compatibility claim. */
#ifndef SALTS_COMPONENT_RUNTIME_ABI_H
#define SALTS_COMPONENT_RUNTIME_ABI_H
#include <stdint.h>
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
#ifdef __cplusplus
}
#endif
#endif /* SALTS_COMPONENT_RUNTIME_ABI_H */
