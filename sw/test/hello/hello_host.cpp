#include <cstdio>
#include "rad.h"

int main() {
    printf("Launching kernel\n");
    radDim3 grid = {1, 1, 1};
    radDim3 block = {1, 1, 2};
    radParamBuf params;
    params.push(5);
    params.push(6);
    radKernelLaunch("hello_kernel", grid, block, &params);
    radStreamSynchronize();
    return 0;
}
