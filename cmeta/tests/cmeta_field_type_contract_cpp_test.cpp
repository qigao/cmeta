#include <cmeta/struct.h>
#include "cmeta_field_type_contract_types.h"

cmeta_require_field(cmeta_field_contract_probe, const_value, const int);
cmeta_require_field(cmeta_field_contract_probe, volatile_value, volatile int);
cmeta_require_field(cmeta_field_contract_probe, values, cmeta_test_int_array3);
cmeta_require_field(cmeta_field_contract_probe, pointer, int *);
cmeta_require_field(cmeta_field_contract_probe, callback, cmeta_test_callback);

int main() {
    return 0;
}
