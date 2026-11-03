#include "harness.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 64;
    }
    for (int i = 1; i < argc; ++i) {
        FILE *file = fopen(argv[i], "rb");
        if (file == NULL) {
            return 1;
        }
        /* One byte above the largest harness INPUT_LIMIT detects oversized files. */
        static uint8_t data[65537];
        size_t size = fread(data, 1, sizeof(data), file);
        bool failed = ferror(file) != 0 || size == sizeof(data);
        if (fclose(file) != 0 || failed) {
            return 1;
        }
        if (LLVMFuzzerTestOneInput(data, size) != 0) {
            return 1;
        }
    }
    return 0;
}
