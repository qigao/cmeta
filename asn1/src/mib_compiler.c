#include "mib_compiler.h"
#include "ast.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declaration from scanner.re
void scan(const char *cursor);

mib_compile_context_t *mib_compile_create_context(void) {
    mib_compile_context_t *ctx = calloc(1, sizeof(mib_compile_context_t));
    if (!ctx) return NULL;
    
    ctx->generate_c_code = 1;
    ctx->generate_runtime_tables = 1;
    return ctx;
}

void mib_compile_destroy_context(mib_compile_context_t *ctx) {
    if (!ctx) return;
    
    free(ctx->module_name);
    free(ctx->output_dir);
    if (ctx->root) {
        free_tree(ctx->root);
    }
    free(ctx);
}

int mib_compile_file(const char *mib_file, const char *output_dir) {
    FILE *f = fopen(mib_file, "r");
    if (!f) {
        printf("Error: Cannot open MIB file: %s\n", mib_file);
        return -1;
    }
    
    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return -1;
    }
    
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);
    
    int result = mib_compile_string(content, output_dir);
    free(content);
    return result;
}

int mib_compile_string(const char *mib_content, const char *output_dir) {
    mib_compile_context_t *ctx = mib_compile_create_context();
    if (!ctx) return -1;
    
    ctx->output_dir = strdup(output_dir);
    
    // Parse the MIB content
    printf("Parsing MIB content...\n");
    scan(mib_content);
    
    // Get the parsed AST from global state
    ctx->root = g_parse_state.root;
    g_parse_state.root = NULL; // Transfer ownership
    
    if (!ctx->root) {
        printf("Error: Failed to parse MIB content\n");
        mib_compile_destroy_context(ctx);
        return -1;
    }
    
    // Extract module name from AST
    if (ctx->root->type == NODE_MODULE && ctx->root->value) {
        ctx->module_name = strdup(ctx->root->value);
    }
    
    // Generate outputs
    char filename[512];
    
    if (ctx->generate_c_code) {
        fmt(filename, sizeof(filename), "{}/{}.h", output_dir, ctx->module_name);
        mib_generate_c_header(ctx, filename);
        
        fmt(filename, sizeof(filename), "{}/{}.c", output_dir, ctx->module_name);
        mib_generate_c_source(ctx, filename);
    }
    
    if (ctx->generate_runtime_tables) {
        fmt(filename, sizeof(filename), "{}/{}_runtime.c", output_dir, ctx->module_name);
        mib_generate_runtime_table(ctx, filename);
    }
    
    printf("MIB compilation completed successfully\n");
    mib_compile_destroy_context(ctx);
    return 0;
}

int mib_generate_c_header(mib_compile_context_t *ctx, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return -1;
    
    fprintf(f, "/* Generated from MIB: %s */\n", ctx->module_name);
    fprintf(f, "#ifndef %s_H\n", ctx->module_name);
    fprintf(f, "#define %s_H\n\n", ctx->module_name);
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stddef.h>\n\n");
    
    // Generate OID constants
    mib_oid_table_t *oids = mib_extract_oids(ctx->root);
    if (oids) {
        fprintf(f, "/* OID Constants */\n");
        for (size_t i = 0; i < oids->count; i++) {
            fprintf(f, "extern const uint32_t %s_OID[];\n", oids->entries[i].name);
            fprintf(f, "#define %s_OID_LEN %zu\n", oids->entries[i].name, oids->entries[i].count);
        }
        fprintf(f, "\n");
        mib_oid_table_destroy(oids);
    }
    
    fprintf(f, "#endif /* %s_H */\n", ctx->module_name);
    fclose(f);
    return 0;
}

int mib_generate_c_source(mib_compile_context_t *ctx, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return -1;
    
    fprintf(f, "/* Generated from MIB: %s */\n", ctx->module_name);
    fprintf(f, "#include \"%s.h\"\n\n", ctx->module_name);
    
    // Generate OID definitions
    mib_oid_table_t *oids = mib_extract_oids(ctx->root);
    if (oids) {
        for (size_t i = 0; i < oids->count; i++) {
            fprintf(f, "const uint32_t %s_OID[] = {", oids->entries[i].name);
            for (size_t j = 0; j < oids->entries[i].count; j++) {
                if (j > 0) fprintf(f, ", ");
                fprintf(f, "%u", oids->entries[i].components[j]);
            }
            fprintf(f, "};\n");
        }
        mib_oid_table_destroy(oids);
    }
    
    fclose(f);
    return 0;
}

int mib_generate_runtime_table(mib_compile_context_t *ctx, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return -1;
    
    fprintf(f, "/* Runtime OID table for %s */\n", ctx->module_name);
    fprintf(f, "#include \"snmp_types.h\"\n");
    fprintf(f, "#include \"%s.h\"\n\n", ctx->module_name);
    
    mib_oid_table_t *oids = mib_extract_oids(ctx->root);
    if (oids) {
        fprintf(f, "static const snmp_oid_entry_t %s_oid_table[] = {\n", ctx->module_name);
        for (size_t i = 0; i < oids->count; i++) {
            fprintf(f, "    {\n");
            fprintf(f, "        .name = \"%s\",\n", oids->entries[i].name);
            fprintf(f, "        .oid = {.components = %s_OID, .count = %s_OID_LEN},\n", 
                    oids->entries[i].name, oids->entries[i].name);
            if (oids->entries[i].description) {
                fprintf(f, "        .description = \"%s\",\n", oids->entries[i].description);
            }
            fprintf(f, "    },\n");
        }
        fprintf(f, "};\n\n");
        
        fprintf(f, "const snmp_mib_module_t %s_module = {\n", ctx->module_name);
        fprintf(f, "    .name = \"%s\",\n", ctx->module_name);
        fprintf(f, "    .oid_table = %s_oid_table,\n", ctx->module_name);
        fprintf(f, "    .oid_count = %zu\n", oids->count);
        fprintf(f, "};\n");
        
        mib_oid_table_destroy(oids);
    }
    
    fclose(f);
    return 0;
}

mib_oid_table_t *mib_extract_oids(AstNode *root) {
    if (!root) return NULL;
    
    mib_oid_table_t *table = calloc(1, sizeof(mib_oid_table_t));
    if (!table) return NULL;
    
    table->capacity = 64;
    table->entries = calloc(table->capacity, sizeof(mib_oid_entry_t));
    if (!table->entries) {
        free(table);
        return NULL;
    }
    
    // TODO: Traverse AST and extract OID definitions
    // This is a simplified version - real implementation would walk the AST
    // and extract OBJECT IDENTIFIER assignments
    
    return table;
}

void mib_oid_table_destroy(mib_oid_table_t *table) {
    if (!table) return;
    
    for (size_t i = 0; i < table->count; i++) {
        free(table->entries[i].components);
        free(table->entries[i].name);
        free(table->entries[i].description);
    }
    free(table->entries);
    free(table);
}
