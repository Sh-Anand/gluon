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
    free(src);

    radError err;
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    
    void *ptr = malloc(bytes);
    radMemCpy(ptr, dst, bytes, radMemCpyDir_D2H);
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);

    for (size_t i = 0; i < bytes; i++) {
        printf("%d ", ((char *)ptr)[i]);
    }

    free(ptr);
    return 0;
}