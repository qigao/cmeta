#include <cstl/typed.h>
#include <cmeta/core_data.h>

#include <stddef.h>
#include <tinytest.h>

cmeta_type(Map, ImportSafeTstrSmokeMap, tstr, int,
      CMETA_TSTR_TYPE_REF, CMETA_TSTR_DATA_REF,
      &cmeta_type_int, &cmeta_data_int);

suite("CSTL imported tstr map") {
  static ImportSafeTstrSmokeMap values;
  static tstr beta, alpha, query;

  before_each() {
    beta = tstr_dup("beta");
    alpha = tstr_dup("alpha");
    query = tstr_dup("alpha");
    check_not_null(beta);
    check_not_null(alpha);
    check_not_null(query);
  }
  after_each() {
    ImportSafeTstrSmokeMap_destroy(&values);
    tstr_free(alpha);
    tstr_free(beta);
    tstr_free(query);
    alpha = beta = query = NULL;
  }

  group("imported type and data identity") {
    it("exposes comparison traits and canonical descriptors") {
      check_not_null(cmeta_tstr_cmeta_type.traits);
      check_not_null(CMETA_TSTR_TYPE_REF->traits);
      check_not_equal(cmeta_tstr_cmeta_type.traits->flags & CMETA_TRAIT_COMPARE, 0u);
      check_not_equal(CMETA_TSTR_TYPE_REF->traits->flags & CMETA_TRAIT_COMPARE, 0u);
      check_true(cmeta_type_equal(CMETA_TSTR_TYPE_REF, &cmeta_tstr_cmeta_type));
      check_true(cmeta_data_desc_equal(CMETA_TSTR_DATA_REF, &cmeta_tstr_cmeta_data));
    }
  }

  group("typed map operations") {
    it("finds keys by value and preserves caller-owned strings on destruction") {
      check_equal(ImportSafeTstrSmokeMap_init(&values, 4u), STL_OK);
      check_equal(ImportSafeTstrSmokeMap_put(&values, beta, 20), STL_OK);
      check_equal(ImportSafeTstrSmokeMap_put(&values, alpha, 10), STL_OK);
      const int *found = ImportSafeTstrSmokeMap_get_const(&values, query);
      check_not_null(found);
      check_equal(*found, 10);
      check_equal(ImportSafeTstrSmokeMap_size(&values), 2u);
      ImportSafeTstrSmokeMap_destroy(&values);
      check_equal(tstr_len(alpha), 5u);
      check_equal(tstr_len(beta), 4u);
      check_equal(tstr_len(query), 5u);
    }

    it("reflects the canonical put signature") {
      const cmeta_receiver_operation_set *set = ImportSafeTstrSmokeMap_receiver_operation_set();
      check_true(cmeta_receiver_operation_set_valid(set));
      check_true(cmeta_generic_desc_equal(set->owner, &stl_map_generic_desc));
      const cmeta_receiver_operation *method = cmeta_receiver_operation_find(set, "put");
      check_not_null(method);
      check_true(method->abi->function == ImportSafeTstrSmokeMap_put_function());
      check_true(method->abi == ImportSafeTstrSmokeMap_put_function_abi());
      check_true(cmeta_type_equal(cmeta_function_param(method->abi->function, 1u)->type,
                                 CMETA_TSTR_TYPE_REF));
      check_true(cmeta_type_equal(cmeta_function_param(method->abi->function, 2u)->type,
                                 &cmeta_type_int));
    }
  }
}
