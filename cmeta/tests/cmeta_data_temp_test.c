#include "tinytest.h"
#include <cmeta/data.h>
#include <cmeta/cnet_policy.h>
#include <cmeta/object.h>

#include <stdint.h>
#include <string.h>

typedef struct cmeta_data_temp_overaligned {
  _Alignas(64) unsigned char bytes[64];
} cmeta_data_temp_overaligned;

static unsigned cmeta_data_temp_init_calls;
static unsigned cmeta_data_temp_restore_calls;

static cmeta_status cmeta_data_temp_test_init(void *object) {
  ++cmeta_data_temp_init_calls;
  memset(object, 0x5a, sizeof(cmeta_data_temp_overaligned));
  return CMETA_OK;
}

static void cmeta_data_temp_test_restore(void *object) {
  ++cmeta_data_temp_restore_calls;
  memset(object, 0, sizeof(cmeta_data_temp_overaligned));
}

static void cmeta_data_temp_test_move(void *destination, void *source) {
  memcpy(destination, source, sizeof(cmeta_data_temp_overaligned));
  memset(source, 0, sizeof(cmeta_data_temp_overaligned));
}

static const cmeta_type_desc cmeta_data_temp_overaligned_type = {
    "cmeta_data_temp_overaligned",
    sizeof(cmeta_data_temp_overaligned),
    CMETA_ALIGNOF(cmeta_data_temp_overaligned),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    NULL};
static const cmeta_data_construct_ops cmeta_data_temp_overaligned_ops = {
    sizeof(cmeta_data_construct_ops),
    CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION,
    &cmeta_data_temp_overaligned_type,
    cmeta_data_temp_test_init,
    cmeta_data_temp_test_restore,
    cmeta_data_temp_test_move,
    CMETA_LIFECYCLE_INIT_NOFAIL | CMETA_LIFECYCLE_MOVABLE};
static const unsigned char cmeta_data_temp_overaligned_shape = 0u;
static const cmeta_data_desc cmeta_data_temp_overaligned_data = {sizeof(cmeta_data_desc),
                                                                 CMETA_DATA_DESC_ABI_VERSION,
                                                                 "test.cmeta.data.temp.overaligned",
                                                                 "over-aligned temporary value",
                                                                 CMETA_DATA_CUSTOM,
                                                                 &cmeta_data_temp_overaligned_type,
                                                                 &cmeta_data_temp_overaligned_shape,
                                                                 NULL,
                                                                 NULL,
                                                                 NULL,
                                                                 NULL,
                                                                 NULL,
                                                                 NULL,
                                                                 NULL,
                                                                 &cmeta_data_temp_overaligned_ops};

suite("CMeta data temporary storage") {
  before_each() {
    cmeta_data_temp_init_calls = 0u;
    cmeta_data_temp_restore_calls = 0u;
  }

  it("allocates and releases over-aligned values on every supported CRT") {
    cmeta_data_temp temp = {0};

    check_true(cmeta_data_desc_valid(&cmeta_data_temp_overaligned_data));
    check_equal(cmeta_data_temp_open(&cmeta_data_temp_overaligned_data,
                                     sizeof(cmeta_data_temp_overaligned), &temp),
                CMETA_OK);
    check_not_null(temp.storage);
    check_equal(temp.extent, sizeof(cmeta_data_temp_overaligned));
    check_equal(temp.alignment, (size_t)64u);
    check_equal((uintptr_t)temp.storage % temp.alignment, (uintptr_t)0u);
    check_equal(cmeta_data_temp_init_calls, 1u);
    check_equal(((const cmeta_data_temp_overaligned *)temp.storage)->bytes[0], (unsigned char)0x5a);

    cmeta_data_temp_close(&temp);

    check_equal(cmeta_data_temp_restore_calls, 1u);
    check_null(temp.storage);
    check_null(temp.data);
    check_equal(temp.extent, (size_t)0u);
    check_equal(temp.alignment, (size_t)0u);
    check_equal(temp.lifecycle, CMETA_DATA_TEMP_NONE);
  }
}

static cmeta_status cnet_cleanup_error_body(void *user) {
    (void)user;
    return CMETA_INVALID_ARGUMENT;
}
suite("CNet policy reflection and explicit cleanup") {
    it("inspects native limits without granting construct or write authority") {
        cnet_send_limits limits = {0};
        limits.connection_bytes = 4096u;
        const cmeta_data_desc *data = cmeta_reflected_data(cnet_send_limits);
        cmeta_object_ref object;
        const cmeta_data_desc *field_data = NULL;
        const void *field = NULL;
        check_true(cmeta_data_desc_valid(data));
        check_false(cmeta_data_struct_constructible(data));
        check_equal(cmeta_object_borrow(&object, &limits, data, NULL), CMETA_OK);
        check_equal(cmeta_object_field_read(&object, "connection_bytes", &field_data, &field), CMETA_OK);
        check_equal(*(const size_t *)field, (size_t)4096u);
        cmeta_object_release(&object);
    }
    it("releases only the caller buffer reference when a scoped operation fails") {
        mem_buffer_t *owned = mem_get_buffer(mem_global(), 16u);
        check_not_null(owned);
        mem_buffer_t *other = mem_buffer_retain(owned);
        cmeta_cleanup obligation = CMETA_CLEANUP_INIT;
        check_equal(cmeta_cnet_buffer_cleanup(&obligation, &owned), CMETA_OK);
        cmeta_status status = cmeta_with_cleanups(&obligation, 1u, cnet_cleanup_error_body, NULL);
        bool cleared = owned == NULL;
        uint32_t remaining = mem_buffer_ref_count(other);
        cmeta_cleanup_run(&obligation);
        mem_buffer_release(other);
        check_equal(status, CMETA_INVALID_ARGUMENT);
        check_true(cleared);
        check_equal(remaining, (uint32_t)1u);
    }
}
