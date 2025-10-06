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
    return 0;
}