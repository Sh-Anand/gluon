#include <rad_defs.h>

extern "C" __kernel__ int hello_except_kernel(int code) {
    volatile int zero = code - code;
    int crash = code / zero;
    return crash;
}
