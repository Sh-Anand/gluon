#include <cstdio>
#include "rad.h"

int main() {
    printf("Allocating GPU memory\n");
    int x[2] = {5, 6};
    void* x_ptr;
    radMalloc(&x_ptr, 16);
    printf("Copying to GPU memory\n");
    radMemCpy(x_ptr, x, 8, radMemCpyDir_H2D);
    radError err;
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    
    printf("Launching first kernel\n");
    radDim3 grid = {1, 1, 1};
    radDim3 block = {1, 1, 2};
    radParamBuf params_one;
    void* y_ptr = (char*) x_ptr + 4;
    void* z_ptr = (char*) x_ptr + 8;
    void* w_ptr = (char*) x_ptr + 12;
    printf("Pushing device pointers 0x%p and 0x%p and 0x%p and 0x%p\n", (uint8_t*)x_ptr, (uint8_t*)y_ptr, (uint8_t*)z_ptr, (uint8_t*)w_ptr); 
    params_one.push(x_ptr);
    params_one.push(y_ptr);
    params_one.push(z_ptr);
    params_one.push(w_ptr);
    radKernelLaunch("hello_first_kernel", grid, block, &params_one);
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("PC: 0x%x\n", err.pc);

    void *second_ptr;
    radMalloc(&second_ptr, 4);
    radParamBuf params_two;
    params_two.push(z_ptr);
    params_two.push(w_ptr);
    params_two.push(second_ptr);
    radKernelLaunch("hello_second_kernel", grid, block, &params_two);
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("PC: 0x%x\n", err.pc);

    int u;
    radMemCpy(&u, second_ptr, 4, radMemCpyDir_D2H);
    radGetError(&err); 
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("Host received final result: %d\n", u);

    int z, w;
    radMemCpy(&z, z_ptr, 4, radMemCpyDir_D2H);
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    radMemCpy(&w, w_ptr, 4, radMemCpyDir_D2H);
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("Host received intermediate results: %d, %d\n", z, w);

    return 0;
}
