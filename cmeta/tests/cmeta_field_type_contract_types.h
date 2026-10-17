#ifndef CMETA_FIELD_TYPE_CONTRACT_TYPES_H
#define CMETA_FIELD_TYPE_CONTRACT_TYPES_H

typedef int cmeta_test_int_array3[3];
typedef int (*cmeta_test_callback)(int);

typedef struct cmeta_field_contract_probe {
    const int const_value;
    volatile int volatile_value;
    cmeta_test_int_array3 values;
    int *pointer;
    cmeta_test_callback callback;
} cmeta_field_contract_probe;

#endif
