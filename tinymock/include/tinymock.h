#ifndef TINYMOCK_H
#define TINYMOCK_H

/*
 * Primary public include for TinyMock.
 *
 * Keep the free-function bridge first. When
 * TINYMOCK_GENERATE_FUNCTION_OVERRIDES is defined, tinymock_function.h
 * installs the CMeta FunctionDecl extension hooks before other CMeta-backed
 * TinyMock interfaces are included.
 */
#include "tinymock_function.h"
#include "tinymock_cmeta.h"

#endif /* TINYMOCK_H */
