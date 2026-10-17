/* Negative contract tests for canonical FunctionDesc / FunctionAbi.
 * Keep this standalone C11 test independent of TinyTest so every failure
 * returns a nonzero process status in all supported build configurations.
 */
#include <cmeta/function.h>

#define REQUIRE(condition_) do { if (!(condition_)) return __LINE__; } while (0)

int main(void) {
    const cmeta_param_desc valid_param = {
        sizeof(cmeta_param_desc), "value", &cmeta_type_int, CMETA_PARAM_IN
    };
    const cmeta_param_desc valid_params[] = { valid_param };
    const cmeta_param_desc two_params[] = {
        valid_param,
        { sizeof(cmeta_param_desc), "other", &cmeta_type_int, CMETA_PARAM_IN }
    };
    const cmeta_abi_carrier valid_carriers[] = { CMETA_ABI_SCALAR };
    const cmeta_abi_carrier two_carriers[] = {
        CMETA_ABI_SCALAR, CMETA_ABI_SCALAR
    };
    cmeta_function_desc function = {
        sizeof(cmeta_function_desc), "contract_probe", &cmeta_type_int,
        valid_params, 1u, CMETA_EFFECT_PURE, CMETA_PROP_NONE, CMETA_RESULT_VALUE
    };
    cmeta_function_abi_desc abi = {
        sizeof(cmeta_function_abi_desc), &function, CMETA_ABI_SCALAR,
        valid_carriers, 1u
    };
    cmeta_param_desc param;
    cmeta_function_desc bad_function;
    cmeta_function_abi_desc bad_abi;

    REQUIRE(cmeta_param_desc_valid(&valid_param));
    REQUIRE(cmeta_function_desc_valid(&function));
    REQUIRE(cmeta_function_abi_desc_valid(&abi));

    REQUIRE(!cmeta_param_desc_valid(NULL));
    param = valid_param;
    param.size = sizeof(param) - 1u;
    REQUIRE(!cmeta_param_desc_valid(&param));
    param = valid_param;
    param.flags = UINT32_C(1) << 31;
    REQUIRE(!cmeta_param_desc_valid(&param));
    param = valid_param;
    param.flags = CMETA_PARAM_BORROWED;
    REQUIRE(!cmeta_param_desc_valid(&param));
    param = valid_param;
    param.flags = CMETA_PARAM_IN | CMETA_PARAM_BORROWED | CMETA_PARAM_OWNED;
    REQUIRE(!cmeta_param_desc_valid(&param));

    REQUIRE(!cmeta_function_desc_valid(NULL));
    bad_function = function;
    bad_function.size = sizeof(bad_function) - 1u;
    REQUIRE(!cmeta_function_desc_valid(&bad_function));
    bad_function = function;
    bad_function.params = NULL;
    REQUIRE(!cmeta_function_desc_valid(&bad_function));
    bad_function = function;
    bad_function.params = two_params;
    bad_function.param_count = 2u;
    REQUIRE(cmeta_function_desc_valid(&bad_function));
    {
        cmeta_param_desc duplicate_params[] = { two_params[0], two_params[1] };
        duplicate_params[1].name = duplicate_params[0].name;
        bad_function.params = duplicate_params;
        REQUIRE(!cmeta_function_desc_valid(&bad_function));
        duplicate_params[1] = two_params[1];
        duplicate_params[1].flags |= CMETA_PARAM_RECEIVER;
        bad_function.params = duplicate_params;
        REQUIRE(!cmeta_function_desc_valid(&bad_function));
    }
    bad_function = function;
    bad_function.result_flags = CMETA_RESULT_VALUE | CMETA_RESULT_OWNED;
    REQUIRE(!cmeta_function_desc_valid(&bad_function));
    bad_function = function;
    bad_function.result_flags = UINT32_C(1) << 31;
    REQUIRE(!cmeta_function_desc_valid(&bad_function));
    bad_function = function;
    bad_function.return_type = &cmeta_type_void;
    bad_function.result_flags = CMETA_RESULT_VALUE;
    REQUIRE(!cmeta_function_desc_valid(&bad_function));

    REQUIRE(!cmeta_function_abi_desc_valid(NULL));
    bad_abi = abi;
    bad_abi.size = sizeof(bad_abi) - 1u;
    REQUIRE(!cmeta_function_abi_desc_valid(&bad_abi));
    bad_abi = abi;
    bad_abi.param_count = 0u;
    REQUIRE(!cmeta_function_abi_desc_valid(&bad_abi));
    bad_abi = abi;
    bad_abi.param_carriers = NULL;
    REQUIRE(!cmeta_function_abi_desc_valid(&bad_abi));
    bad_abi = abi;
    bad_abi.return_carrier = (cmeta_abi_carrier)99;
    REQUIRE(!cmeta_function_abi_desc_valid(&bad_abi));
    bad_abi = abi;
    bad_abi.return_carrier = CMETA_ABI_OBJECT_POINTER;
    REQUIRE(!cmeta_function_abi_desc_valid(&bad_abi));
    bad_abi = abi;
    {
        cmeta_abi_carrier wrong_param_carriers[] = { CMETA_ABI_OBJECT_POINTER };
        bad_abi.param_carriers = wrong_param_carriers;
        REQUIRE(!cmeta_function_abi_desc_valid(&bad_abi));
    }
    bad_abi = abi;
    {
        cmeta_function_desc two_param_function = function;
        two_param_function.params = two_params;
        two_param_function.param_count = 2u;
        bad_abi.function = &two_param_function;
        bad_abi.param_count = 2u;
        bad_abi.param_carriers = two_carriers;
        REQUIRE(cmeta_function_abi_desc_valid(&bad_abi));
    }

    return 0;
}
