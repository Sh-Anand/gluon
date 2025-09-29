#ifdef RAD_HOST
#include <cstdio>
#include "rad.h"
#include "rad_driver.h"
#endif

#ifdef RAD_TARGET

extern "C" int hello_kernel(int x) {
    if (x == 0)
        return 17;
    else
        return 82;
}

#endif

#ifdef RAD_HOST

int main() {
    printf("Launching kernel\n");
    if (!rad::InitConnection()) {
        printf("Failed to initialize connection\n");
        return 1;
    }
    radDim3 grid = {1, 2, 2};
    radDim3 block = {1, 4, 4};
    radKernelLaunch("hello_kernel", grid, block);
    rad::ShutdownConnection();
    return 0;
}

#endif
