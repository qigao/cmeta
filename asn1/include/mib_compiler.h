#ifndef MIB_COMPILER_H
#define MIB_COMPILER_H


#include "asn1_api.h"
#include "ast.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// MIB Compiler API
typedef struct {
    char *module_name;
    AstNode *root;
    char *output_dir;
    int generate_c_code;
    int generate_runtime_tables;
} mib_compile_context_t;

// Main compiler functions
SALTSNET_ASN1_C_API int mib_compile_file(const char *mib_file, const char *output_dir);
SALTSNET_ASN1_C_API int mib_compile_string(const char *mib_content, const char *output_dir);
SALTSNET_ASN1_C_API mib_compile_context_t *mib_compile_create_context(void);
SALTSNET_ASN1_C_API void mib_compile_destroy_context(mib_compile_context_t *ctx);

// Code generation
SALTSNET_ASN1_C_API int mib_generate_c_header(mib_compile_context_t *ctx, const char *filename);
SALTSNET_ASN1_C_API int mib_generate_c_source(mib_compile_context_t *ctx, const char *filename);
SALTSNET_ASN1_C_API int mib_generate_runtime_table(mib_compile_context_t *ctx, const char *filename);

// OID management
typedef struct {
    uint32_t *components;
    size_t count;
    char *name;
    char *description;
} mib_oid_entry_t;

typedef struct {
    mib_oid_entry_t *entries;
    size_t count;
    size_t capacity;
} mib_oid_table_t;

SALTSNET_ASN1_C_API mib_oid_table_t *mib_extract_oids(AstNode *root);
SALTSNET_ASN1_C_API void mib_oid_table_destroy(mib_oid_table_t *table);

#ifdef __cplusplus
}
#endif

#endif // MIB_COMPILER_H