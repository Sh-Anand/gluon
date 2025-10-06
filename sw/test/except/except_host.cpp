#include <cstdio>
#include "rad.h"

int main() {
    printf("Launching kernel\n");
    radDim3 grid = {1, 2, 2};
    radDim3 block = {1, 4, 4};
    radParamBuf params;
    params.push(1);
    radKernelLaunch("hello_except_kernel", grid, block, &params);
    return 0;
}
