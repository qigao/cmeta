#include "tinymock_reflect_fixture.h"
#include <math.h>
#include <string.h>

suite("TinyMock reflected parameter matching") {
    static tinymock_cmeta_history history;
    static tinymock_cmeta_captor captor;
    static tinymock_reflect_result result;

    before_each() {
        match_request_snapshot_type = *cmeta_reflected_storage(MatchRequest);
        match_request_snapshot_type.traits = &match_request_traits;
        tinymock_cmeta_history_init(&history, &match_request_function);
        tinymock_cmeta_captor_init(&captor);
    }
    after_each() {
        tinymock_cmeta_captor_destroy(&captor);
        tinymock_cmeta_history_destroy(&history);
    }
    it("compares fields rather than native padding and reports nested mismatch") {
        MatchRequest actual, expected;
        enum { ACTUAL_PADDING = 0x11, EXPECTED_PADDING = 0x77, USER_ID = 42, OTHER_ID = 7 };
        memset(&actual, ACTUAL_PADDING, sizeof(actual));
        memset(&expected, EXPECTED_PADDING, sizeof(expected));
        actual.user.active = expected.user.active = true;
        actual.user.id = expected.user.id = USER_ID;
        actual.score = expected.score = 1.0;
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchRequest),
            &actual, &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        check_equal(result.path, "");
        expected.user.id = OTHER_ID;
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchRequest),
            &actual, &expected, NULL, &result), CMETA_OK);
        check_false(result.equal);
        check_equal(result.path, "$.user.id");
    }
    it("matches the recorded value after caller mutation and preserves captor ownership") {
        MatchRequest actual = {{true, 42}, 1.0};
        MatchRequest expected = actual;
        const tinymock_cmeta_arg_view view = {&actual, false, NULL};
        check_true(tinymock_cmeta_history_record(&history, &match_request_function, 1u, &view));
        actual.user.id = 7;
        check_equal(tinymock_cmeta_history_arg_match_data_name(&history, 0u, "request",
            cmeta_reflected_data(MatchRequest), &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        expected.user.id = actual.user.id;
        check_equal(tinymock_cmeta_history_arg_match_data(&history, 0u, 0u,
            cmeta_reflected_data(MatchRequest), &expected, NULL, &result), CMETA_OK);
        check_false(result.equal);
        check_equal(result.path, "request.user.id");
        check_true(tinymock_cmeta_captor_capture(&captor, &history, 0u, 0u));
        tinymock_cmeta_history_reset(&history, &match_request_function);
        expected.user.id = 42;
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchRequest),
            tinymock_cmeta_captor_value(&captor), &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        check_equal(tinymock_cmeta_history_arg_match_data(&history, 0u, 0u,
            cmeta_reflected_data(MatchRequest), &expected, NULL, &result), CMETA_INVALID_ARGUMENT);
        check_false(result.equal);
        check_equal(result.path, "");
    }
    it("compares only published fields without granting snapshot authority") {
        MatchProjection actual = {{true, 42}, 1.0}, expected = {{false, 7}, 1.0};
        check_false(cmeta_data_value_copy_supported(cmeta_reflected_data(MatchProjection)));
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchProjection),
            &actual, &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        tinymock_cmeta_value value;
        tinymock_cmeta_value_init(&value);
        check_false(tinymock_cmeta_value_copy(&value,
            cmeta_reflected_storage(MatchProjection), &actual));
        check_false(value.constructed);
        check_null(value.allocation);
    }
    it("uses canonical floating equality for NaNs and signed zero") {
        double actual = NAN, expected = NAN;
        check_equal(tinymock_cmeta_data_match(&cmeta_data_double,
            &actual, &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        actual = 0.0;
        expected = -0.0;
        check_equal(tinymock_cmeta_data_match(&cmeta_data_double,
            &actual, &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
    }
    it("reports exhausted traversal and byte limits separately from inequality") {
        MatchRequest value = {{true, 42}, 1.0};
        tinymock_reflect_limits limits = {1u, TINYMOCK_REFLECT_DEFAULT_NODES,
            TINYMOCK_REFLECT_DEFAULT_BYTES};
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchRequest),
            &value, &value, &limits, &result), CMETA_CAPACITY_EXCEEDED);
        limits.max_depth = TINYMOCK_REFLECT_MAX_DEPTH;
        limits.max_nodes = 1u;
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchRequest),
            &value, &value, &limits, &result), CMETA_CAPACITY_EXCEEDED);
        limits.max_nodes = TINYMOCK_REFLECT_DEFAULT_NODES;
        limits.max_bytes = 1u;
        check_equal(tinymock_cmeta_data_match(cmeta_reflected_data(MatchRequest),
            &value, &value, &limits, &result), CMETA_CAPACITY_EXCEEDED);
        check_false(result.equal);
        check_equal(result.path, "");
    }
    it("rejects different native storage and missing calls or parameters") {
        MatchRequest value = {{true, 42}, 1.0};
        int expected = 42;
        const tinymock_cmeta_arg_view view = {&value, false, NULL};
        check_true(tinymock_cmeta_history_record(&history, &match_request_function, 1u, &view));
        check_equal(tinymock_cmeta_history_arg_match_data(&history, 0u, 0u,
            &cmeta_data_int, &expected, NULL, &result), CMETA_TYPE_MISMATCH);
        check_equal(tinymock_cmeta_history_arg_match_data(&history, 1u, 0u,
            cmeta_reflected_data(MatchRequest), &value, NULL, &result), CMETA_INVALID_ARGUMENT);
        check_equal(tinymock_cmeta_history_arg_match_data_name(&history, 0u, "missing",
            cmeta_reflected_data(MatchRequest), &value, NULL, &result), CMETA_INVALID_ARGUMENT);
    }
    it("keeps pointer identity matching separate from reflected pointee values") {
        MatchRequest value = {{true, 42}, 1.0};
        void *pointer = &value;
        const tinymock_cmeta_arg_view view = {&pointer, true, pointer};
        tinymock_cmeta_history_reset(&history, &match_pointer_function);
        check_true(tinymock_cmeta_history_record(&history, &match_pointer_function, 1u, &view));
        check_true(tinymock_cmeta_history_arg_pointer_equal(&history, 0u, 0u, pointer));
        check_equal(tinymock_cmeta_history_arg_match_data(&history, 0u, 0u,
            cmeta_reflected_data(MatchRequest), &value, NULL, &result), CMETA_TYPE_MISMATCH);
        check_false(result.equal);
    }
    it("compares bounded byte providers and propagates their read failure") {
        MatchBytes actual = {1u, 0u, 1u, 0u}, expected = {1u, 0u, 1u, 0u};
        tinymock_reflect_limits limits = {TINYMOCK_REFLECT_MAX_DEPTH,
            TINYMOCK_REFLECT_DEFAULT_NODES, MATCH_BYTES_EXTENT};
        check_equal(tinymock_cmeta_data_match(&match_bytes_cmeta_data,
            &actual, &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        expected[1] = 1u;
        check_equal(tinymock_cmeta_data_match(&match_bytes_cmeta_data,
            &actual, &expected, NULL, &result), CMETA_OK);
        check_false(result.equal);
        check_equal(result.path, "$");
        check_equal(tinymock_cmeta_data_match(&match_bytes_cmeta_data,
            &actual, &expected, &limits, &result), CMETA_CAPACITY_EXCEEDED);
        cmeta_data_desc data = match_bytes_cmeta_data;
        cmeta_data_buffer_ops ops = match_bytes_cmeta_buffer_ops;
        data.buffer_ops = &ops;
        ops.read = match_read_failure;
        check_equal(tinymock_cmeta_data_match(&data,
            &actual, &expected, NULL, &result), CMETA_CALLBACK_ERROR);
        check_false(result.equal);
        check_equal(result.path, "");
    }
    it("uses the canonical unsigned enum domain without signed narrowing") {
        MatchFlags actual = MatchFlags_HIGH, expected = MatchFlags_HIGH;
        check_equal(tinymock_cmeta_data_match(MatchFlags_cmeta_data(),
            &actual, &expected, NULL, &result), CMETA_OK);
        check_true(result.equal);
        expected = MatchFlags_READ;
        check_equal(tinymock_cmeta_data_match(MatchFlags_cmeta_data(),
            &actual, &expected, NULL, &result), CMETA_OK);
        check_false(result.equal);
    }
    it("rejects malformed field storage and an overlong diagnostic path") {
        MatchUser value = {true, 42};
        cmeta_data_desc data = *cmeta_reflected_data(MatchUser);
        cmeta_data_reflection_shape shape = MatchUser__data_shape;
        cmeta_struct_desc layout = *shape.structure.layout;
        cmeta_field_desc layout_fields[] = {layout.fields[0], layout.fields[1]};
        cmeta_data_field_desc fields[] = {shape.structure.fields[0], shape.structure.fields[1]};
        char long_name[TINYMOCK_REFLECT_PATH_BYTES];
        data.shape = &shape;
        shape.structure.layout = &layout;
        shape.structure.fields = fields;
        layout.fields = layout_fields;
        fields[1].value = &cmeta_data_double;
        check_equal(tinymock_cmeta_data_match(&data,
            &value, &value, NULL, &result), CMETA_INVALID_ARGUMENT);
        fields[1] = MatchUser__data_fields[1];
        memset(long_name, 'x', sizeof(long_name) - 1u);
        long_name[sizeof(long_name) - 1u] = '\0';
        fields[0].name = layout_fields[0].name = long_name;
        check_equal(tinymock_cmeta_data_match(&data,
            &value, &value, NULL, &result), CMETA_CAPACITY_EXCEEDED);
        check_equal(result.path, "");
    }
    it("rejects unsupported semantic containers instead of comparing storage bytes") {
        const cmeta_data_collection_view value = {NULL, 0u, sizeof(int), &cmeta_data_int};
        check_equal(tinymock_cmeta_data_match(&cmeta_data_sequence_view,
            &value, &value, NULL, &result), CMETA_TRAIT_MISSING);
        check_false(result.equal);
        check_equal(result.path, "");
    }
    it("checks storage extent even when the semantic type identity is unchanged") {
        MatchRequest value = {{true, 42}, 1.0};
        const tinymock_cmeta_arg_view view = {&value, false, NULL};
        cmeta_type_desc storage = match_request_snapshot_type;
        cmeta_data_desc data = *cmeta_reflected_data(MatchRequest);
        /* V1 permits a layout descriptor separate from its storage descriptor. */
        data.abi_version = CMETA_DATA_DESC_ABI_VERSION;
        data.storage_type = &storage;
        storage.size += sizeof(int);
        check_true(cmeta_data_desc_valid(&data));
        check_true(tinymock_cmeta_history_record(&history, &match_request_function, 1u, &view));
        check_equal(tinymock_cmeta_history_arg_match_data(&history, 0u, 0u,
            &data, &value, NULL, &result), CMETA_TYPE_MISMATCH);
        check_false(result.equal);
        check_equal(result.path, "");
    }
    it("rejects dynamic fields without invoking an implicit access provider") {
        MatchUser value = {true, 42};
        cmeta_data_desc data = *cmeta_reflected_data(MatchUser);
        cmeta_data_struct_shape shape = MatchUser__data_shape.structure;
        cmeta_struct_desc layout = *shape.layout;
        cmeta_field_desc layout_fields[] = {layout.fields[0], layout.fields[1]};
        cmeta_data_field_desc fields[] = {shape.fields[0], shape.fields[1]};
        data.abi_version = CMETA_DATA_DESC_ABI_VERSION;
        data.shape = &shape;
        shape.layout = &layout;
        shape.fields = fields;
        layout.fields = layout_fields;
        fields[0].offset = layout_fields[0].offset = CMETA_FIELD_DYNAMIC_OFFSET;
        check_equal(tinymock_cmeta_data_match(&data,
            &value, &value, NULL, &result), CMETA_TRAIT_MISSING);
        check_false(result.equal);
        check_equal(result.path, "");
    }
}
