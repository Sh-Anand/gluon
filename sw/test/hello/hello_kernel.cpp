#include <rad_print.h>
#include <rad_defs.h>

extern "C" int hello_kernel(int x, int y) {
    rad_printf("Hello, World\n");
    rad_printf("Inputs: %d, %d\n", x, y);
    int z = x + y;
    rad_printf("Result: %d\n", z);
    return z;
}
