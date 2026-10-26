/* Test the ACTUAL backend selector in the ACTUAL production Disruptor TU.
 * Public opaque API, no alternate algorithm or test copy of Atomic macros.
 */
#define SALTS_DISRUPTOR_BACKEND_ASM 1
#if defined(SALTS_TEST_ASM_ORDERED)
#define SALTS_DISRUPTOR_ASM_ORDERED 1
#endif
#include "../../../concurrency/src/disruptor.c"
