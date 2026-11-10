#ifndef TINYMOCK_SUPPORT_H
#define TINYMOCK_SUPPORT_H

#include <tinytest.h>

#include <stddef.h>

#ifndef TINYMOCk_ASSERT
#define TINYMOCk_ASSERT(condition, ...) check((condition), __VA_ARGS__)
#endif

#ifndef TINYMOCk_MAX_ARGS
#define TINYMOCk_MAX_ARGS 6
#endif

#ifndef TINYMOCk_MAX_CALLS
#define TINYMOCk_MAX_CALLS 32
#endif

#define TINYMOCk_CAT(a, b) TINYMOCk_CAT_I(a, b)
#define TINYMOCk_CAT_I(a, b) a##b

#endif /* TINYMOCK_SUPPORT_H */
