#include "cmeta_fastpath_fixture.h"
#include "tinytest.h"
#include <thread.h>

enum {
    FASTPATH_INPUT = 5,
    FASTPATH_OLD_RESULT = 8,
    FASTPATH_NEW_RESULT = 10,
    FASTPATH_WORKERS = 4,
    FASTPATH_ROUNDS = 10000,
    FASTPATH_POLL_ROUNDS = 10000,
    FASTPATH_POLL_MS = 1,
    FASTPATH_PUBLISHED = 42
};
cmeta_static_call(fastpath_test_slot, fastpath_add);
cmeta_static_call(fastpath_float_slot, fastpath_float);
cmeta_static_call(fastpath_pair_slot, fastpath_aggregate);
cmeta_static_call(fastpath_callback_slot, fastpath_choose);
cmeta_static_call(fastpath_void_slot, fastpath_void);
cmeta_static_call(fastpath_zero_slot, fastpath_zero);
cmeta_static_call(fastpath_void_zero_slot, fastpath_void_zero);
cmeta_static_call(fastpath_borrow_slot, fastpath_borrow);
cmeta_static_call(fastpath_publication_slot, fastpath_unpublished);

typedef struct publication_state {
    cmeta_fast_key_state key;
    int payload;
    int observed;
} publication_state;

static void publication_reader(void *arg) {
    publication_state *state = (publication_state *)arg;
    for (int round = 0; round < FASTPATH_POLL_ROUNDS; ++round) {
        const bool enabled = cmeta_fast_branch(&state->key);
        if (enabled) {
            state->observed = state->payload;
            return;
        }
        cmeta_sleep_ms(FASTPATH_POLL_MS);
    }
}

static void target_publication_reader(void *arg) {
    publication_state *state = (publication_state *)arg;
    for (int round = 0; round < FASTPATH_POLL_ROUNDS; ++round) {
        fastpath_publication_slot_target_type target =
            fastpath_publication_slot_load(&fastpath_publication_slot);
        if (target == fastpath_published) {
            state->observed = target();
            return;
        }
        cmeta_sleep_ms(FASTPATH_POLL_MS);
    }
}

typedef struct replacement_state {
    atomic_int errors;
    atomic_int updates;
} replacement_state;

static void replacement_writer(void *arg) {
    replacement_state *state = (replacement_state *)arg;
    for (int round = 0; round < FASTPATH_ROUNDS; ++round) {
        cmeta_status status = round % 2 == 0
            ? cmeta_static_update(fastpath_test_slot, fastpath_add)
            : cmeta_static_update(fastpath_test_slot, fastpath_other);
        if (status != CMETA_OK) atomic_fetch_add(&state->errors, 1);
        else atomic_fetch_add(&state->updates, 1);
        cmeta_thread_yield();
    }
}

static void replacement_reader(void *arg) {
    replacement_state *state = (replacement_state *)arg;
    for (int round = 0; round < FASTPATH_ROUNDS; ++round) {
        int value = cmeta_static_invoke(fastpath_test_slot, FASTPATH_INPUT);
        if (value != FASTPATH_OLD_RESULT && value != FASTPATH_NEW_RESULT)
            atomic_fetch_add(&state->errors, 1);
        cmeta_thread_yield();
    }
}

suite("CMeta static fastpath") {
    it("publishes bounded keys and rejects NULL control inputs") {
        SALTS_FAST_KEY(disabled, false);
        SALTS_FAST_KEY(enabled, true);
        check_false(cmeta_fast_branch(&disabled));
        check_true(cmeta_fast_key_read(&enabled));
        check_equal(cmeta_fast_enable(&disabled), CMETA_OK);
        check_equal(cmeta_fast_enable(&disabled), CMETA_OK);
        check_true(cmeta_fast_branch(&disabled));
        check_equal(cmeta_fast_disable(&disabled), CMETA_OK);
        check_false(cmeta_fast_branch(&disabled));
        check_equal(cmeta_fast_key_set(NULL, true), SALTS_EINVAL);
        check_equal(cmeta_fast_disable(NULL), SALTS_EINVAL);
    }

    it("acquire readers observe data published before enabling") {
        publication_state state = {{false}, 0, 0};
        cmeta_thread_t thread = NULL;
        check_equal(cmeta_thread_create(&thread, publication_reader, &state), 0);
        state.payload = FASTPATH_PUBLISHED;
        check_equal(cmeta_fast_enable(&state.key), CMETA_OK);
        check_equal(cmeta_thread_join(&thread), 0);
        cmeta_thread_destroy(&thread);
        check_equal(state.observed, FASTPATH_PUBLISHED);
    }

    it("replaces renamed and cross-TU targets while preserving strict descriptor equality") {
        const cmeta_function_abi_desc *original = fastpath_add_function_abi();
        const cmeta_function_abi_desc *replacement = fastpath_peer_abi();
        check_false(cmeta_function_abi_desc_equal(original, replacement));
        check_true(cmeta_function_abi_contract_compatible(original, replacement));
        check_true(cmeta_function_abi_contract_compatible(replacement, original));
        check_equal(cmeta_static_update(fastpath_test_slot, fastpath_add), CMETA_OK);
        check_equal(cmeta_static_invoke(fastpath_test_slot, FASTPATH_INPUT), FASTPATH_OLD_RESULT);
        check_equal(fastpath_test_slot_set(&fastpath_test_slot, fastpath_other, replacement), CMETA_OK);
        check_equal(cmeta_static_invoke(fastpath_test_slot, FASTPATH_INPUT), FASTPATH_NEW_RESULT);
    }

    it("target acquire loads observe data published before replacement") {
        publication_state state = {{false}, 0, 0};
        cmeta_thread_t thread = NULL;
        check_equal(cmeta_static_update(fastpath_publication_slot, fastpath_unpublished), CMETA_OK);
        fastpath_target_payload = 0;
        check_equal(cmeta_thread_create(&thread, target_publication_reader, &state), 0);
        fastpath_target_payload = FASTPATH_PUBLISHED;
        check_equal(cmeta_static_update(fastpath_publication_slot, fastpath_published), CMETA_OK);
        check_equal(cmeta_thread_join(&thread), 0);
        cmeta_thread_destroy(&thread);
        check_equal(state.observed, FASTPATH_PUBLISHED);
    }

    it("rejects incompatible effects and invalid updates without modifying the target") {
        check_equal(cmeta_static_update(fastpath_test_slot, fastpath_add), CMETA_OK);
        check_equal(cmeta_static_update(fastpath_test_slot, fastpath_effectful), CMETA_TYPE_MISMATCH);
        check_equal(fastpath_test_slot_set(NULL, fastpath_other, fastpath_peer_abi()), CMETA_INVALID_ARGUMENT);
        check_equal(fastpath_test_slot_set(&fastpath_test_slot, NULL, fastpath_peer_abi()), CMETA_INVALID_ARGUMENT);
        check_equal(fastpath_test_slot_set(&fastpath_test_slot, fastpath_other, NULL), CMETA_INVALID_ARGUMENT);
        check_true(fastpath_test_slot_load(&fastpath_test_slot) == fastpath_add);
        check_equal(cmeta_static_invoke(fastpath_test_slot, FASTPATH_INPUT), FASTPATH_OLD_RESULT);
    }

    it("requires explicit compatible carriers, types, flags and properties") {
        const cmeta_function_abi_desc *expected = fastpath_add_function_abi();
        cmeta_function_abi_desc candidate = *expected;
        cmeta_function_desc function = *expected->function;
        cmeta_param_desc parameter = function.params[0];
        cmeta_abi_carrier carrier = CMETA_ABI_SCALAR;
        candidate.function = &function;
        candidate.param_carriers = &carrier;
        function.params = &parameter;
        check_true(cmeta_function_abi_contract_compatible(expected, &candidate));
        carrier = CMETA_ABI_UNSPECIFIED;
        check_true(cmeta_function_abi_desc_valid(&candidate));
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        carrier = CMETA_ABI_FUNCTION_POINTER;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        carrier = CMETA_ABI_SCALAR;
        candidate.return_carrier = CMETA_ABI_UNSPECIFIED;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        candidate.return_carrier = expected->return_carrier;
        parameter.type = &cmeta_type_double;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        parameter.type = expected->function->params[0].type;
        parameter.flags = CMETA_PARAM_UNKNOWN;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        parameter.flags = expected->function->params[0].flags;
        function.result_flags = CMETA_RESULT_VALUE;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        function.result_flags = expected->function->result_flags;
        function.properties = CMETA_CONTRACT_PROPERTIES(pure);
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        function.properties = expected->function->properties;
        function.return_type = &cmeta_type_double;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        function.return_type = expected->function->return_type;
        candidate.param_count = 0u;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        candidate.param_count = expected->param_count;
        candidate.size = 0u;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        check_false(cmeta_function_abi_contract_compatible(NULL, expected));
    }

    it("rejects parameter ownership, result lifetime and pointee contract changes") {
        const cmeta_function_abi_desc *expected = fastpath_borrow_function_abi();
        cmeta_function_abi_desc candidate = *expected;
        cmeta_function_desc function = *expected->function;
        cmeta_param_desc parameter = function.params[0];
        cmeta_type_desc pointer = *parameter.type;
        candidate.function = &function;
        function.params = &parameter;
        parameter.flags = CMETA_PARAM_IN | CMETA_PARAM_OWNED;
        check_true(cmeta_function_abi_desc_valid(&candidate));
        check_equal(fastpath_borrow_slot_set(&fastpath_borrow_slot, fastpath_borrow, &candidate), CMETA_TYPE_MISMATCH);
        parameter.flags = expected->function->params[0].flags;
        function.result_flags = CMETA_RESULT_OWNED;
        check_equal(fastpath_borrow_slot_set(&fastpath_borrow_slot, fastpath_borrow, &candidate), CMETA_TYPE_MISMATCH);
        function.result_flags = expected->function->result_flags;
        pointer.pointee = &cmeta_type_int;
        parameter.type = &pointer;
        check_false(cmeta_function_abi_contract_compatible(expected, &candidate));
        check_true(fastpath_borrow_slot_load(&fastpath_borrow_slot) == fastpath_borrow);
    }

    it("preserves floating, aggregate, callback, borrowed pointer, void and zero-argument C ABI") {
        const fastpath_pair input = {1.5, 1};
        char borrowed[] = "borrowed";
        check_true(cmeta_static_invoke(fastpath_borrow_slot, borrowed) == borrowed);
        fastpath_pair output = cmeta_static_invoke(fastpath_pair_slot, input, 2.5);
        check_equal(output.value, 4.0);
        check_equal(output.count, 2);
        check_equal(cmeta_static_invoke(fastpath_float_slot, 1.5f, 2.5), 4.0);
        check_true(cmeta_static_invoke(fastpath_callback_slot, fastpath_add) == fastpath_add);
        check_equal(cmeta_static_invoke(fastpath_callback_slot, fastpath_add)(FASTPATH_INPUT), FASTPATH_OLD_RESULT);
        cmeta_static_invoke(fastpath_void_slot, FASTPATH_INPUT);
        check_equal(fastpath_void_sink, FASTPATH_INPUT);
        cmeta_static_invoke0(fastpath_void_zero_slot);
        check_equal(fastpath_void_sink, FASTPATH_PUBLISHED);
        check_equal(cmeta_static_invoke0(fastpath_zero_slot), FASTPATH_PUBLISHED);
    }

    it("supports two control writers and two readers without torn targets") {
        replacement_state state = {0};
        cmeta_thread_t threads[FASTPATH_WORKERS] = {NULL};
        int created = 0;
        int joined = 0;
        for (int index = 0; index < FASTPATH_WORKERS; ++index) {
            cmeta_thread_cb worker = index % 2 == 0 ? replacement_writer : replacement_reader;
            if (cmeta_thread_create(&threads[index], worker, &state) != 0) break;
            ++created;
        }
        for (int index = 0; index < created; ++index) {
            if (cmeta_thread_join(&threads[index]) == 0) ++joined;
            cmeta_thread_destroy(&threads[index]);
        }
        check_equal(created, FASTPATH_WORKERS);
        check_equal(joined, FASTPATH_WORKERS);
        check_equal(atomic_load(&state.errors), 0);
        check_equal(atomic_load(&state.updates), (FASTPATH_WORKERS / 2) * FASTPATH_ROUNDS);
        check_equal(cmeta_static_update(fastpath_test_slot, fastpath_add), CMETA_OK);
    }
}
