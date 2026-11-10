#ifndef TINYMOCK_REFLECT_FIXTURE_H
#define TINYMOCK_REFLECT_FIXTURE_H

#include <cmeta/data_reflect.h>
#include <cmeta/flags.h>
#include "tinymock_history.h"

typedef struct MatchUser { bool active; int id; } MatchUser;
typedef struct MatchRequest { MatchUser user; double score; } MatchRequest;
typedef MatchRequest MatchProjection;

cmeta_reflect_value(MatchUser, "test.MatchUser",
    cmeta_field(bool, active)
    cmeta_field(int, id)
);
cmeta_reflect_value(MatchRequest, "test.MatchRequest",
    cmeta_data_field(MatchUser, user, cmeta_reflected_data(MatchUser),
        cmeta_reflected_storage(MatchUser))
    cmeta_field(double, score)
);
cmeta_reflect_data(MatchProjection, "test.MatchProjection",
    cmeta_field(double, score)
);

/* Snapshot authority is explicit and separate from a read-only projection. */
static bool match_request_copy(void *destination, const void *source) {
    return cmeta_data_trait_copy_construct(cmeta_reflected_data(MatchRequest), destination, source);
}
static void match_request_destroy(void *object) {
    cmeta_data_trait_destroy(cmeta_reflected_data(MatchRequest), object);
}
static const cmeta_type_traits match_request_traits = {
    CMETA_TRAIT_COPY | CMETA_TRAIT_DESTROY, NULL, NULL, NULL,
    match_request_copy, NULL, match_request_destroy
};
static cmeta_type_desc match_request_snapshot_type;
static const cmeta_param_desc match_request_param = {
    sizeof(cmeta_param_desc), "request", &match_request_snapshot_type, CMETA_PARAM_IN
};
static const cmeta_function_desc match_request_function = {
    sizeof(cmeta_function_desc), "send", &cmeta_type_void, &match_request_param, 1u,
    CMETA_EFFECT_STATEFUL, CMETA_PROP_NONE, CMETA_RESULT_UNKNOWN
};

enum { MATCH_BYTES_EXTENT = 4 };
typedef unsigned char MatchBytes[MATCH_BYTES_EXTENT];
CMETA_DEFINE_FIXED_BYTES(match_bytes, MatchBytes, MATCH_BYTES_EXTENT,
    "test.MatchBytes", "MatchBytes");
cmeta_flags(MatchFlags, "test.MatchFlags",
    cmeta_flag(READ, UINT64_C(1), "read")
    cmeta_flag(HIGH, UINT64_C(1) << 63, "high")
);
static cmeta_status match_read_failure(const void *object,
    const unsigned char **data, size_t *size) {
    (void)object;
    *data = NULL;
    *size = 0u;
    return CMETA_CALLBACK_ERROR;
}
static const cmeta_param_desc match_pointer_param = {
    sizeof(cmeta_param_desc), "pointer", &cmeta_type_void_ptr, CMETA_PARAM_IN | CMETA_PARAM_BORROWED
};
static const cmeta_function_desc match_pointer_function = {
    sizeof(cmeta_function_desc), "borrow", &cmeta_type_void, &match_pointer_param, 1u,
    CMETA_EFFECT_STATEFUL, CMETA_PROP_NONE, CMETA_RESULT_UNKNOWN
};

#endif
