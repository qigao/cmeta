#ifndef CMETA_ENUM_TU_CONTRACT_TYPES_H
#define CMETA_ENUM_TU_CONTRACT_TYPES_H

#include <cmeta/enum.h>

Enum(CMetaEnumTuContractState,
    (CMETA_ENUM_TU_IDLE,  "idle"),
    (CMETA_ENUM_TU_READY, 7, "ready"),
    (CMETA_ENUM_TU_DONE,  "done")
);

const cmeta_enum_desc *cmeta_enum_tu_peer_meta(void);

#endif
