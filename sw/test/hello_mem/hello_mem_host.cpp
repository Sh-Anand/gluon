#include <cstdio>
#include "rad.h"

int main() {
    printf("Allocating GPU memory\n");
    int x[2] = {5, 6};
    void *x_ptr;
    radMalloc(&x_ptr, 8);
    printf("Copying to GPU memory\n");
    radMemCpy(x_ptr, x, 8, radMemCpyDir_H2D);
    radError err;
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    
    printf("Launching kernel\n");
    radDim3 grid = {1, 1, 1};
    radDim3 block = {1, 1, 2};
    radParamBuf params;
    uint32_t x_ptr_u32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(x_ptr));
    uint32_t y_ptr_u32 = x_ptr_u32 + 4;

    printf("Pushing device pointers 0x%x and 0x%x\n", x_ptr_u32, y_ptr_u32);
    
    params.push(x_ptr_u32);
    params.push(y_ptr_u32);
    radKernelLaunch("hello_mem_kernel", grid, block, &params);
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("PC: 0x%x\n", err.pc);
    return 0;
}
