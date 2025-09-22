#ifdef RAD_TARGET
extern "C" int printf(const char *, ...);
#else
#include <cstdio>
#include "rad.h"
#endif

#ifdef RAD_TARGET

int hello_kernel(int x) {
    if (x == 0)
        return 17;
    else
        return 82;
}

#endif

#ifdef RAD_HOST

int main() {
    printf("Launching kernel\n");
    radKernelLaunch("hello_kernel");
    return 0;
}

#endif
