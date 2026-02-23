#include <cstdio>
#include <cstdlib>
#include "rad.h"

int main() {
    printf("Performing memcpy\n");
    
    size_t bytes = 1024;
    
    void *dst;
    radMalloc(&dst, bytes);
    void *src = malloc(bytes);
    memset(src, 5, bytes);
    radMemCpy(dst, src, bytes, radMemCpyDir_H2D);
    radStreamSynchronize();
    free(src);
    
    void *ptr = malloc(bytes);
    radMemCpy(ptr, dst, bytes, radMemCpyDir_D2H);
    radStreamSynchronize();

    for (size_t i = 0; i < bytes; i++) {
        printf("%d ", ((char *)ptr)[i]);
    }

    free(ptr);
    return 0;
}
