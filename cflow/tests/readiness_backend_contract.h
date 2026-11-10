#ifndef SALTS_READINESS_BACKEND_CONTRACT_H
#define SALTS_READINESS_BACKEND_CONTRACT_H

#include <readiness.h>

#include <stddef.h>
#include <stdint.h>

typedef struct readiness_backend_contract_fixture readiness_backend_contract_fixture;

typedef struct readiness_backend_contract_factory {
  readiness_backend_contract_fixture *(*create)(readiness_config config,
                                                readiness_reactor *reactor,
                                                int *status);
  void (*destroy)(readiness_backend_contract_fixture *fixture);
  intptr_t (*resource)(readiness_backend_contract_fixture *fixture, size_t index);
  int (*make_readable)(readiness_backend_contract_fixture *fixture, size_t index);
  int (*drain_readable)(readiness_backend_contract_fixture *fixture, size_t index);
} readiness_backend_contract_factory;

const readiness_backend_contract_factory *readiness_backend_contract_factory_get(void);

#endif /* SALTS_READINESS_BACKEND_CONTRACT_H */
