#include <string.h>

#include "cmeta_enum_tu_contract_types.h"

int main(void) {
    const cmeta_enum_desc *local = CMetaEnumTuContractState_meta();
    const cmeta_enum_desc *peer = cmeta_enum_tu_peer_meta();
    size_t i;

    if (local == NULL || peer == NULL || local == peer) return 1;
    if (local->name == NULL || peer->name == NULL ||
        strcmp(local->name, peer->name) != 0) return 2;
    if (local->count != peer->count || local->count != 3u) return 3;

    for (i = 0u; i < local->count; ++i) {
        if (local->items[i].value != peer->items[i].value) return 4;
        if (strcmp(local->items[i].symbol, peer->items[i].symbol) != 0) return 5;
        if (strcmp(local->items[i].text, peer->items[i].text) != 0) return 6;
    }

    if (CMetaEnumTuContractState_to_string(CMETA_ENUM_TU_READY) == NULL ||
        strcmp(CMetaEnumTuContractState_to_string(CMETA_ENUM_TU_READY), "ready") != 0)
        return 7;
    return 0;
}
