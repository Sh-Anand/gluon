#include <cstdio>
#include "rad.h"

int main() {
    printf("Launching kernel\n");
    radDim3 grid = {1, 2, 2};
    radDim3 block = {1, 4, 4};
    radParamBuf params;
    params.push(1);
    params.push(2);
    radKernelLaunch("hello_kernel", grid, block, &params);
    radError err;
    radGetError(&err);
    printf("Error: %d\n", err.err_code);
    printf("Command ID: %d\n", err.cmd_id);
    printf("PC: %d\n", err.pc);
    return 0;
}
