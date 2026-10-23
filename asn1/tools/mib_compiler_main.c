#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fmt.h>
#include "mib_compiler.h"

void print_usage(const char *program_name) {
    printf("Usage: %s [options] <mib_file>\n", program_name);
    printf("\nOptions:\n");
    printf("  -o <dir>     Output directory (default: current directory)\n");
    printf("  -h, --help   Show this help message\n");
    printf("  --no-c       Don't generate C code\n");
    printf("  --no-runtime Don't generate runtime tables\n");
    printf("\nExamples:\n");
    printf("  %s SNMPv2-MIB.txt\n", program_name);
    printf("  %s -o generated/ RFC1213-MIB.txt\n", program_name);
    printf("  %s --no-runtime CISCO-MEMORY-POOL-MIB.txt\n", program_name);
}

int main(int argc, char *argv[]) {
    const char *mib_file = NULL;
    const char *output_dir = ".";
    int generate_c = 1;
    int generate_runtime = 1;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires an argument\n");
                return 1;
            }
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "--no-c") == 0) {
            generate_c = 0;
        } else if (strcmp(argv[i], "--no-runtime") == 0) {
            generate_runtime = 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            if (mib_file) {
                fprintf(stderr, "Error: Multiple MIB files specified\n");
                return 1;
            }
            mib_file = argv[i];
        }
    }
    
    if (!mib_file) {
        fprintf(stderr, "Error: No MIB file specified\n");
        print_usage(argv[0]);
        return 1;
    }
    
    printf("MIB Compiler v1.0\n");
    printf("==================\n");
    printf("Input file: %s\n", mib_file);
    printf("Output directory: %s\n", output_dir);
    printf("Generate C code: %s\n", generate_c ? "yes" : "no");
    printf("Generate runtime tables: %s\n", generate_runtime ? "yes" : "no");
    printf("\n");
    
    // Create output directory if it doesn't exist
#ifdef _WIN32
    char mkdir_cmd[512];
    fmt(mkdir_cmd, sizeof(mkdir_cmd), "mkdir \"{}\" 2>nul", output_dir);
    system(mkdir_cmd);
#else
    char mkdir_cmd[512];
    fmt(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"{}\"", output_dir);
    system(mkdir_cmd);
#endif
    
    // Compile the MIB
    int result = mib_compile_file(mib_file, output_dir);
    if (result != 0) {
        fprintf(stderr, "Error: MIB compilation failed\n");
        return 1;
    }
    
    printf("MIB compilation completed successfully!\n");
    return 0;
}
