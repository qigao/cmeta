#ifndef AST_H
#define AST_H


#include "asn1_api.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// AST Node Types for Schema Parsing
typedef enum {
    NODE_MODULE,
    NODE_ASSIGNMENT,
    NODE_SEQUENCE,
    NODE_SEQUENCE_OF,
    NODE_SET,
    NODE_SET_OF,
    NODE_CHOICE,
    NODE_TYPE_DEF,
    NODE_INTEGER,
    NODE_BOOLEAN,
    NODE_OCTET_STRING,
    NODE_BIT_STRING,
    NODE_NULL,
    NODE_Identifier
} ast_node_type_t;

// AST Node Structure
typedef struct AstNode {
    ast_node_type_t type;
    char *value;
    struct AstNode *left;
    struct AstNode *right;
} AstNode;

// Parse State for Schema Parser
typedef struct {
    AstNode *root;
    int error;
    char error_msg[256];
} ParseState;

// Global parse state (used by scanner)
extern ParseState g_parse_state;

// AST Functions
SALTSNET_ASN1_C_API AstNode *new_node(ast_node_type_t type, const char *value, AstNode *left, AstNode *right);
SALTSNET_ASN1_C_API void free_tree(AstNode *node);
SALTSNET_ASN1_C_API void print_tree(AstNode *node, int indent);

// AST Traversal
SALTSNET_ASN1_C_API void ast_traverse_preorder(AstNode *node, void (*callback)(AstNode *node, void *data), void *data);
SALTSNET_ASN1_C_API void ast_traverse_postorder(AstNode *node, void (*callback)(AstNode *node, void *data), void *data);

// AST Query Functions
SALTSNET_ASN1_C_API AstNode *ast_find_node_by_type(AstNode *root, ast_node_type_t type);
SALTSNET_ASN1_C_API AstNode *ast_find_node_by_value(AstNode *root, const char *value);

#ifdef __cplusplus
}
#endif

#endif // AST_H