%include {
#include <assert.h>
#include "ast.h"
#include <stdio.h>
}

%name Asn1Parse
%token_prefix TK_
%token_type {AstNode*}
%extra_argument {ParseState *state}

%syntax_error {
    fprintf(stderr, "Syntax Error!\n");
}

%parse_accept {
    printf("Parsing Complete Successfully.\n");
}

// ---------------------------------------------------------
// Grammar Rules
// ---------------------------------------------------------

main ::= module(M). { 
    state->root = M; 
}

// Example: MyModule DEFINITIONS ::= BEGIN ... END
module(R) ::= IDENTIFIER(Name) KW_DEFINITIONS KW_ASSIGN KW_BEGIN assignment_list(L) KW_END. {
    R = new_node(NODE_MODULE, Name->value, L, NULL);
    free_tree(Name); // Cleanup raw token wrapper, keep string in new node if needed
}

assignment_list(R) ::= assignment(A). { R = A; }
assignment_list(R) ::= assignment_list(L) assignment(A). {
    // Basic linking logic for simplicity
    R = L;
    AstNode *tmp = L;
    while(tmp->right) tmp = tmp->right;
    tmp->right = A;
}

// Example: MyType ::= SEQUENCE { ... }
assignment(R) ::= IDENTIFIER(Name) KW_ASSIGN type_def(T). {
    R = new_node(NODE_ASSIGNMENT, Name->value, T, NULL);
    free_tree(Name);
}

type_def(R) ::= KW_SEQUENCE LBRACE field_list(L) RBRACE. {
    R = new_node(NODE_SEQUENCE, "SEQUENCE", L, NULL);
}

type_def(R) ::= KW_SEQUENCE KW_OF type_name(T). {
    R = new_node(NODE_SEQUENCE_OF, "SEQUENCE OF", T, NULL);
}

type_def(R) ::= KW_SET LBRACE field_list(L) RBRACE. {
    R = new_node(NODE_SET, "SET", L, NULL);
}

type_def(R) ::= KW_SET KW_OF type_name(T). {
    R = new_node(NODE_SET_OF, "SET OF", T, NULL);
}

type_def(R) ::= KW_CHOICE LBRACE field_list(L) RBRACE. {
    R = new_node(NODE_CHOICE, "CHOICE", L, NULL);
}

// Example: field INTEGER
field_list(R) ::= field(F). { R = F; }
field_list(R) ::= field_list(L) COMMA field(F). {
    R = L;
    AstNode *tmp = L;
    while(tmp->right) tmp = tmp->right;
    tmp->right = F;
}

field(R) ::= IDENTIFIER(Name) type_name(T). {
    R = new_node(NODE_TYPE_DEF, Name->value, T, NULL);
    free_tree(Name);
}

type_name(R) ::= KW_INTEGER. { R = new_node(NODE_INTEGER, "INTEGER", NULL, NULL); }
type_name(R) ::= KW_BOOLEAN. { R = new_node(NODE_BOOLEAN, "BOOLEAN", NULL, NULL); }
type_name(R) ::= KW_OCTET KW_STRING. { R = new_node(NODE_OCTET_STRING, "OCTET STRING", NULL, NULL); }
type_name(R) ::= KW_BIT KW_STRING. { R = new_node(NODE_BIT_STRING, "BIT STRING", NULL, NULL); }
type_name(R) ::= KW_NULL. { R = new_node(NODE_NULL, "NULL", NULL, NULL); }
type_name(R) ::= IDENTIFIER(Name). {
    R = new_node(NODE_Identifier, Name->value, NULL, NULL);
    free_tree(Name);
}

// Destructors: How to free a token if a parse error causes it to be discarded
%destructor module { free_tree($$); }
%destructor assignment { free_tree($$); }
%destructor IDENTIFIER { free_tree($$); }