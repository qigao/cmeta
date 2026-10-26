/* Compile the unmodified production Disruptor C translation unit exactly
 * once with separately owned GAS atomic storage. All function declarations
 * stay in the original public opaque header. No production files are edited.
 */
#define SALTS_TEST_ONLY_ASM_DISRUPTOR 1
#include "disruptor_asm_adapter.h"
#include "../../../concurrency/src/disruptor.c"
