#ifndef SALTS_TESTS_CMETA_CRYPTO_TEST_HOOKS_H
#define SALTS_TESTS_CMETA_CRYPTO_TEST_HOOKS_H

#include <stddef.h>

void *cmeta_crypto_test_alloc(size_t size);
void cmeta_crypto_test_free(void *data);

#endif
