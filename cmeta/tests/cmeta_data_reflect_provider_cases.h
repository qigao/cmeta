#include <cmeta/fixed_array.h>
#include <stdlib.h>
#include <string.h>

enum { REFLECT_TEXT_LIMIT = 16, REFLECT_ARRAY_COUNT = 3 };
typedef struct ReflectText { unsigned char *bytes; size_t size; } ReflectText;
static size_t reflect_text_allocations, reflect_text_releases;
static size_t reflect_text_assignments, reflect_text_fail_assignment;
static bool reflect_text_refuse_restore; /* violate provider no-fail contract */

static bool reflect_text_is_zero(const void *object) {
    const ReflectText *text = (const ReflectText *)object;
    return text->bytes == NULL && text->size == 0u;
}
static cmeta_status reflect_text_init(void *object) {
    ReflectText *text = (ReflectText *)object;
    text->bytes = NULL;
    text->size = 0u;
    return CMETA_OK;
}
static void reflect_text_restore(void *object) {
    ReflectText *text = (ReflectText *)object;
    /* Only the explicit checked-close negative test enables this: a broken
     * external no-fail provider leaves the original owning value live.
     */
    if (reflect_text_refuse_restore) return;
    if (text->bytes != NULL) {
        free(text->bytes);
        ++reflect_text_releases;
    }
    text->bytes = NULL;
    text->size = 0u;
}
static cmeta_status reflect_text_assign(void *object, const unsigned char *bytes,
                                       size_t size, size_t max_bytes) {
    ReflectText *text = (ReflectText *)object;
    if (size > max_bytes || size > REFLECT_TEXT_LIMIT) return CMETA_CAPACITY_EXCEEDED;
    if (size == 0u) return CMETA_OK;
    text->bytes = (unsigned char *)malloc(size);
    if (text->bytes == NULL) return CMETA_OUT_OF_MEMORY;
    ++reflect_text_allocations;
    text->size = size;
    memcpy(text->bytes, bytes, size);
    /* Fail after acquiring a resource, exercising the provider boundary's
     * partial-value cleanup as well as the aggregate's accepted-prefix unwind. */
    ++reflect_text_assignments;
    return reflect_text_assignments == reflect_text_fail_assignment
        ? CMETA_CALLBACK_ERROR : CMETA_OK;
}
static cmeta_status reflect_text_read(const void *object, const unsigned char **bytes,
                                     size_t *size) {
    const ReflectText *text = (const ReflectText *)object;
    *bytes = text->bytes;
    *size = text->size;
    return CMETA_OK;
}
static void reflect_text_move(void *destination, void *source) {
    ReflectText *from = (ReflectText *)source;
    *(ReflectText *)destination = *from;
    from->bytes = NULL;
    from->size = 0u;
}

static const cmeta_type_identity reflect_text_identity = CMETA_TYPE_ID_ATOM_INIT("test.reflect.Text");
static const cmeta_type_desc reflect_text_type = {
    "ReflectText", sizeof(ReflectText), CMETA_ALIGNOF(ReflectText),
    CMETA_T_OBJECT, NULL, NULL, &reflect_text_identity
};
static const cmeta_data_buffer_shape reflect_text_shape = {CMETA_DATA_BUFFER_OWNED};
static const cmeta_data_buffer_ops reflect_text_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &reflect_text_type, CMETA_DATA_BUFFER_OWNED, reflect_text_is_zero,
    reflect_text_assign, reflect_text_restore, reflect_text_read, reflect_text_init, reflect_text_move
};
static const cmeta_data_desc reflect_text_data = {
    sizeof(cmeta_data_desc), CMETA_DATA_DESC_ABI_VERSION, "test.reflect.Text", "ReflectText",
    CMETA_DATA_STRING, &reflect_text_type, &reflect_text_shape, &reflect_text_ops,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

typedef int ReflectArray[REFLECT_ARRAY_COUNT];
CMETA_DEFINE_FIXED_ARRAY(ReflectArrayProvider, ReflectArray, int, REFLECT_ARRAY_COUNT,
    &cmeta_data_int, "test.reflect.Array", "ReflectArray");
typedef struct ReflectManagedRecord {
    ReflectText first;
    ReflectText second;
    ReflectArray values;
} ReflectManagedRecord;
cmeta_reflect_value(ReflectManagedRecord, "test.reflect.ManagedRecord",
    cmeta_data_field(ReflectText, first, &reflect_text_data, &reflect_text_type)
    cmeta_data_field(ReflectText, second, &reflect_text_data, &reflect_text_type)
    cmeta_data_field(ReflectArray, values, &ReflectArrayProvider_cmeta_data,
        &ReflectArrayProvider_cmeta_type)
);

suite("CMeta reflected provider lifecycle") {
    static ReflectManagedRecord source, destination, moved;
    static const unsigned char bytes[] = {'a', 'b'};
    const cmeta_data_desc *data = cmeta_reflected_data(ReflectManagedRecord);
    before_each() {
        reflect_text_allocations = reflect_text_releases = 0u;
        reflect_text_assignments = reflect_text_fail_assignment = 0u;
        reflect_text_refuse_restore = false;
        check_equal(cmeta_data_value_init_zero(data, &source), CMETA_OK);
        check_equal(cmeta_data_value_init_zero(data, &destination), CMETA_OK);
        check_equal(cmeta_data_value_init_zero(data, &moved), CMETA_OK);
        check_equal(cmeta_data_buffer_assign(&reflect_text_data, &source.first,
            bytes, sizeof(bytes), REFLECT_TEXT_LIMIT), CMETA_OK);
        check_equal(cmeta_data_buffer_assign(&reflect_text_data, &source.second,
            bytes, sizeof(bytes), REFLECT_TEXT_LIMIT), CMETA_OK);
        source.values[0] = REFLECT_ARRAY_COUNT;
    }
    after_each() {
        reflect_text_refuse_restore = false;
        check_equal(cmeta_data_value_restore_zero(data, &source), CMETA_OK);
        check_equal(cmeta_data_value_restore_zero(data, &destination), CMETA_OK);
        check_equal(cmeta_data_value_restore_zero(data, &moved), CMETA_OK);
        check_equal(reflect_text_allocations, reflect_text_releases);
    }
    it("keeps temporary allocation and owner lease live on broken restore") {
        cmeta_data_temp temp = {0};
        const unsigned char raw[] = {0u, 0xffu, 0x42u};
        const unsigned char *borrowed = NULL;
        size_t borrowed_size = 0u;
        void *original_storage;

        check_equal(cmeta_data_temp_try_close(NULL), CMETA_OK);
        check_equal(cmeta_data_temp_try_close(&temp), CMETA_OK);
        check_equal(cmeta_data_temp_open(
            &reflect_text_data, sizeof(ReflectText), &temp), CMETA_OK);
        original_storage = temp.storage;
        check_not_null(original_storage);
        check_equal(cmeta_data_buffer_assign(
            &reflect_text_data, temp.storage, raw, sizeof(raw),
            REFLECT_TEXT_LIMIT), CMETA_OK);
        /* No-fail provider violation: the checked facade must NOT free or
         * silently clear the owner's allocation/borrowed descriptor.
         */
        reflect_text_refuse_restore = true;
        check_equal(cmeta_data_temp_try_close(&temp), CMETA_CALLBACK_ERROR);
        check_equal(temp.storage, original_storage);
        check_equal(temp.data, &reflect_text_data);
        check_equal(reflect_text_releases, (size_t)0);
        check_equal(cmeta_data_buffer_read(
            &reflect_text_data, temp.storage, sizeof(raw),
            &borrowed, &borrowed_size), CMETA_OK);
        check_equal(borrowed_size, sizeof(raw));
        check_equal(borrowed, raw, sizeof(raw));
        /* The test controls this deliberately broken provider and repairs
         * it explicitly. Production callers MUST NOT automatically retry an
         * arbitrary failed no-fail cleanup or unload that code module.
         */
        reflect_text_refuse_restore = false;
        check_equal(cmeta_data_temp_try_close(&temp), CMETA_OK);
        check_true(temp.storage == NULL && temp.data == NULL);
        check_equal(cmeta_data_temp_try_close(&temp), CMETA_OK);
        check_equal(reflect_text_releases, (size_t)1);
    }
    it("unwinds both the failed field and accepted prefix exactly once") {
        enum { FAILED_COPY_RELEASES = 2 };
        bool zero = false;
        reflect_text_fail_assignment = reflect_text_assignments + FAILED_COPY_RELEASES;
        check_equal(cmeta_data_value_copy(data, &destination, &source), CMETA_CALLBACK_ERROR);
        check_equal(cmeta_data_value_is_zero(data, &destination, &zero), CMETA_OK);
        check_true(zero);
        check_equal(reflect_text_releases, (size_t)FAILED_COPY_RELEASES);
        check_equal(source.first.bytes, bytes, sizeof(bytes));
        check_equal(source.second.bytes, bytes, sizeof(bytes));
        check_equal(source.values[0], REFLECT_ARRAY_COUNT);
    }
    it("deep copies provider fields and transfers ownership without an allocation") {
        bool zero = false;
        check_equal(cmeta_data_value_copy(data, &destination, &source), CMETA_OK);
        check_true(destination.first.bytes != source.first.bytes);
        check_true(destination.second.bytes != source.second.bytes);
        check_equal(destination.first.bytes, bytes, sizeof(bytes));
        check_equal(destination.values[0], source.values[0]);
        const size_t allocations_before_move = reflect_text_allocations;
        check_equal(cmeta_data_value_move(data, &moved, &destination), CMETA_OK);
        check_equal(reflect_text_allocations, allocations_before_move);
        check_equal(cmeta_data_value_is_zero(data, &destination, &zero), CMETA_OK);
        check_true(zero);
        check_equal(moved.values[0], source.values[0]);
    }
    it("fingerprints string and sequence storage using their canonical providers") {
        const cmeta_fingerprint_limits limits = {CMETA_FINGERPRINT_DEFAULT_DEPTH,
            CMETA_FINGERPRINT_DEFAULT_NODES, CMETA_FINGERPRINT_DEFAULT_ROWS,
            CMETA_FINGERPRINT_DEFAULT_STRING_BYTES};
        uint64_t fingerprint = 0;
        check_equal(cmeta_contract_fingerprint_struct(StructMeta(ReflectManagedRecord),
            &limits, &fingerprint), CMETA_OK);
    }
}
