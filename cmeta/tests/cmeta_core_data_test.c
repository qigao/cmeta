#include "tinytest.h"

#include <cmeta/core_data.h>

static bool peer_uuid_is_zero(const void *object) {
    return cmeta_uuid_cmeta_buffer_ops.is_zero(object);
}

static cmeta_status peer_uuid_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
    return cmeta_uuid_cmeta_buffer_ops.assign(object, data, size, max_bytes);
}

static void peer_uuid_restore_zero(void *object) {
    cmeta_uuid_cmeta_buffer_ops.restore_zero(object);
}

static cmeta_status peer_uuid_init_zero(void *object) {
    return cmeta_uuid_cmeta_buffer_ops.init_zero(object);
}

static void peer_uuid_move(void *destination, void *source) {
    cmeta_uuid_cmeta_buffer_ops.move(destination, source);
}

static cmeta_status peer_uuid_copy(void *destination, const void *source) {
    return cmeta_uuid_cmeta_fixed_ops.copy(destination, source);
}

static const cmeta_type_identity peer_uuid_identity =
    CMETA_TYPE_ID_ATOM_INIT("salts.uuid");

static const cmeta_type_desc peer_uuid_type = {
    "cmeta_uuid_t", sizeof(cmeta_uuid_t), CMETA_ALIGNOF(cmeta_uuid_t),
    CMETA_T_OBJECT, NULL, NULL, &peer_uuid_identity
};

static const cmeta_data_buffer_shape peer_uuid_shape = {
    CMETA_DATA_BUFFER_OWNED
};

static const cmeta_data_buffer_ops peer_uuid_buffer_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &peer_uuid_type, CMETA_DATA_BUFFER_OWNED,
    peer_uuid_is_zero, peer_uuid_assign, peer_uuid_restore_zero, NULL,
    peer_uuid_init_zero, peer_uuid_move
};

static const cmeta_data_fixed_ops peer_uuid_fixed_ops = {
    sizeof(cmeta_data_fixed_ops), CMETA_DATA_FIXED_OPS_ABI_VERSION,
    &peer_uuid_type, sizeof(cmeta_uuid_t),
    peer_uuid_is_zero, peer_uuid_copy, peer_uuid_restore_zero
};

static const cmeta_data_desc peer_uuid_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "salts.uuid.data",
    .display_name = "cmeta_uuid_t",
    .kind = CMETA_DATA_STRING,
    .storage_type = &peer_uuid_type,
    .shape = &peer_uuid_shape,
    .buffer_ops = &peer_uuid_buffer_ops,
    .fixed_ops = &peer_uuid_fixed_ops
};

suite("CMeta Core data admission") {
    it("admits equivalent UUID providers without callback address identity") {
        check_true(cmeta_data_desc_equal(&peer_uuid_data, &cmeta_uuid_cmeta_data));
        check_true(cmeta_uuid_cmeta_data_valid(&peer_uuid_data));
    }

    it("rejects UUID providers with incompatible identity or lifecycle ABI") {
        cmeta_data_desc candidate = peer_uuid_data;
        cmeta_data_buffer_ops buffer_ops = peer_uuid_buffer_ops;
        cmeta_data_fixed_ops fixed_ops = peer_uuid_fixed_ops;

        candidate.stable_id = "salts.uuid.incompatible";
        check_false(cmeta_uuid_cmeta_data_valid(&candidate));

        candidate = peer_uuid_data;
        candidate.buffer_ops = &buffer_ops;
        buffer_ops.assign = NULL;
        check_false(cmeta_uuid_cmeta_data_valid(&candidate));

        candidate = peer_uuid_data;
        candidate.fixed_ops = &fixed_ops;
        fixed_ops.extent = sizeof(cmeta_uuid_t) - 1u;
        check_false(cmeta_uuid_cmeta_data_valid(&candidate));
    }
}
