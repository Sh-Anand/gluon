#include <cstdio>
#include "rad.h"

int main() {
    printf("Allocating GPU memory\n");
    int x[2] = {5, 6};
    void* x_ptr;
    radMalloc(&x_ptr, 12);
    printf("Copying to GPU memory\n");
    radMemCpy(x_ptr, x, 8, radMemCpyDir_H2D);
    radStreamSynchronize();
    
    printf("Launching kernel\n");
    radDim3 grid = {1, 1, 1};
    radDim3 block = {1, 1, 2};
    radParamBuf params;
    void* y_ptr = (char*) x_ptr + 4;
    void* z_ptr = (char*) x_ptr + 8;
    printf("Pushing device pointers 0x%p and 0x%p and 0x%p\n", (uint8_t*)x_ptr, (uint8_t*)y_ptr, (uint8_t*)z_ptr); 
    params.push(x_ptr);
    params.push(y_ptr);
    params.push(z_ptr);
    radKernelLaunch("hello_mem_kernel", grid, block, &params);
    radStreamSynchronize();

    int z;
    radMemCpy(&z, z_ptr, 4, radMemCpyDir_D2H);
    radStreamSynchronize();
    printf("Host received: %d\n", z);
    return 0;
}
