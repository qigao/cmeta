#ifndef SALTS_READINESS_CONTRACT_SUITE_H
#define SALTS_READINESS_CONTRACT_SUITE_H

#include <readiness.h>

#include <stddef.h>
#include <stdint.h>

typedef struct readiness_contract_fixture readiness_contract_fixture;

typedef enum readiness_contract_hook {
  READINESS_CONTRACT_HOOK_REGISTER = 0,
  READINESS_CONTRACT_HOOK_ARM,
  READINESS_CONTRACT_HOOK_UNARM,
  READINESS_CONTRACT_HOOK_CLOSE,
  READINESS_CONTRACT_HOOK_SHUTDOWN,
  READINESS_CONTRACT_HOOK_COUNT
} readiness_contract_hook;

typedef struct readiness_contract_factory {
  readiness_contract_fixture *(*create)(readiness_config config,
                                        readiness_reactor *reactor, int *status);
  void (*destroy)(readiness_contract_fixture *fixture);
  int (*emit_resource)(readiness_contract_fixture *fixture, intptr_t native_resource,
                       readiness_events events);
  int (*emit_token)(readiness_contract_fixture *fixture, uint64_t token,
                    readiness_events events, int status);
  int (*fail_backend)(readiness_contract_fixture *fixture, int status);
  uint64_t (*token_for_resource)(readiness_contract_fixture *fixture, intptr_t native_resource);
  uint64_t (*arm_token_for_resource)(readiness_contract_fixture *fixture,
                                     intptr_t native_resource);
  int (*emit_arm_token)(readiness_contract_fixture *fixture, uint64_t token,
                        uint64_t arm_token, readiness_events events);
  void (*fail_next_arm)(readiness_contract_fixture *fixture, int status);
  void (*fail_hook)(readiness_contract_fixture *fixture, readiness_contract_hook hook, int status,
                    size_t calls);
  size_t (*backend_close_calls)(readiness_contract_fixture *fixture);
  size_t (*backend_unarm_calls)(readiness_contract_fixture *fixture);
  size_t (*backend_reentrant_checks)(readiness_contract_fixture *fixture);
  void (*block_hook)(readiness_contract_fixture *fixture, readiness_contract_hook hook);
  void (*block_hook_on_call)(readiness_contract_fixture *fixture, readiness_contract_hook hook,
                             size_t call);
  void (*release_hook)(readiness_contract_fixture *fixture, readiness_contract_hook hook);
  int (*wait_hook_calls)(readiness_contract_fixture *fixture, readiness_contract_hook hook,
                         size_t calls, uint64_t timeout_ns);
  int (*wait_admission_closed)(readiness_contract_fixture *fixture);
  uint64_t (*hook_last_sequence)(readiness_contract_fixture *fixture, readiness_contract_hook hook);
} readiness_contract_factory;

const readiness_contract_factory *readiness_contract_factory_get(void);

#endif
