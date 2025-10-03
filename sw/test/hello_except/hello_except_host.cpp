#include <cstdio>
#include "rad.h"
#include "driver.h"

int main() {
    printf("Launching kernel\n");
    if (!rad::InitConnection()) {
        printf("Failed to initialize connection\n");
        return 1;
    }
    radDim3 grid = {1, 2, 2};
    radDim3 block = {1, 4, 4};
    radKernelLaunch("hello_except_kernel", grid, block);
    rad::ShutdownConnection();
    return 0;
}
