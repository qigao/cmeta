#include <cmeta/fast_key.h>

bool cmeta_fast_key_read(const cmeta_fast_key_state *key) {
    return cmeta_fast_branch(key);
}

int cmeta_fast_key_set(cmeta_fast_key_state *key, bool enabled) {
    if (key == NULL)
        return SALTS_EINVAL;
    atomic_store_explicit(&key->enabled, enabled, memory_order_release);
    return SALTS_OK;
}

bool cmeta_fast_key_consume(cmeta_fast_key_state *key) {
    if (key == NULL)
        return false;
    return atomic_exchange_explicit(
        &key->enabled, false, memory_order_acq_rel);
}

int cmeta_fast_enable(cmeta_fast_key_state *key) {
    return cmeta_fast_key_set(key, true);
}

int cmeta_fast_disable(cmeta_fast_key_state *key) {
    return cmeta_fast_key_set(key, false);
}
