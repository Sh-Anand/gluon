#include <cstdio>
#include <cstdlib>
#include "rad.h"

int main() {
    printf("Performing memcpy\n");
    radParamBuf params;
    size_t bytes = 1024;
    void *dst;
    radMalloc(&dst, bytes);
    void *src = malloc(bytes);
    memset(src, 5, bytes);
    radMemCpy(dst, src, bytes, radMemCpyDir_H2D);
    free(src);
    radError err;
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("PC: %d\n", err.pc);
    return 0;
}