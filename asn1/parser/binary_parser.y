// Lemon grammar for binary ASN.1 DER/BER parsing
%include {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "asn1_types.h"
}

%name Asn1BinaryParse
%token_prefix BIN_TK_
%token_type {asn1_value_t*}
%default_type {asn1_value_t*}
%extra_argument {BinaryParseState *state}

%syntax_error {
    state->error = 1;
    snprintf(state->error_msg, sizeof(state->error_msg), 
             "Syntax error in ASN.1 binary data");
}

%parse_failure {
    state->error = 1;
    snprintf(state->error_msg, sizeof(state->error_msg), 
             "Parse failure in ASN.1 binary data");
}

// Start rule
start ::= value(V). {
    state->root = V;
}

// All ASN.1 values
value(A) ::= BIN_TK_BOOLEAN(B). { A = B; }
value(A) ::= BIN_TK_INTEGER(I). { A = I; }
value(A) ::= BIN_TK_OCTET_STRING(O). { A = O; }
value(A) ::= BIN_TK_BIT_STRING(B). { A = B; }
value(A) ::= BIN_TK_NULL(N). { A = N; }
value(A) ::= BIN_TK_OBJECT_IDENTIFIER(O). { A = O; }
value(A) ::= BIN_TK_SEQUENCE(S). { A = S; }
value(A) ::= BIN_TK_SET(S). { A = S; }
value(A) ::= BIN_TK_CONTEXT_SPECIFIC(C). { A = C; }

%code {
// Helper functions are moved to asn1_runtime.c
// This keeps the grammar simple and conflict-free
}