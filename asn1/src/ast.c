#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Global parse state
ParseState g_parse_state = {0};

AstNode *new_node(ast_node_type_t type, const char *value, AstNode *left, AstNode *right) {
    AstNode *node = (AstNode *)malloc(sizeof(AstNode));
    if (!node) return NULL;
    
    node->type = type;
    node->value = value ? strdup(value) : NULL;
    node->left = left;
    node->right = right;
    
    return node;
}

void free_tree(AstNode *node) {
    if (!node) return;
    
    free_tree(node->left);
    free_tree(node->right);
    free(node->value);
    free(node);
}

void print_tree(AstNode *node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    const char *type_names[] = {
        "MODULE", "ASSIGNMENT", "SEQUENCE", "SEQUENCE_OF", "SET", "SET_OF",
        "CHOICE", "TYPE_DEF", "INTEGER", "BOOLEAN", "OCTET_STRING", 
        "BIT_STRING", "NULL", "IDENTIFIER"
    };
    
    printf("%s", type_names[node->type]);
    if (node->value) {
        printf(": %s", node->value);
    }
    printf("\n");
    
    if (node->left) {
        print_tree(node->left, indent + 1);
    }
    if (node->right) {
        print_tree(node->right, indent + 1);
    }
}

void ast_traverse_preorder(AstNode *node, void (*callback)(AstNode *node, void *data), void *data) {
    if (!node || !callback) return;
    
    callback(node, data);
    ast_traverse_preorder(node->left, callback, data);
    ast_traverse_preorder(node->right, callback, data);
}

void ast_traverse_postorder(AstNode *node, void (*callback)(AstNode *node, void *data), void *data) {
    if (!node || !callback) return;
    
    ast_traverse_postorder(node->left, callback, data);
    ast_traverse_postorder(node->right, callback, data);
    callback(node, data);
}

AstNode *ast_find_node_by_type(AstNode *root, ast_node_type_t type) {
    if (!root) return NULL;
    
    if (root->type == type) return root;
    
    AstNode *found = ast_find_node_by_type(root->left, type);
    if (found) return found;
    
    return ast_find_node_by_type(root->right, type);
}

AstNode *ast_find_node_by_value(AstNode *root, const char *value) {
    if (!root || !value) return NULL;
    
    if (root->value && strcmp(root->value, value) == 0) return root;
    
    AstNode *found = ast_find_node_by_value(root->left, value);
    if (found) return found;
    
    return ast_find_node_by_value(root->right, value);
}