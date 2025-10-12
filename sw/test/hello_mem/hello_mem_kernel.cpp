#include <rad_print.h>
#include <rad_defs.h>

extern "C" int hello_mem_kernel(int *x, int *y) {
    rad_printf("Hello, World\n");
    rad_printf("Pointers x: %p, y: %p\n", x, y);
    rad_printf("Inputs: %d, %d\n", *x, *y);
    int z = *x + *y;
    rad_printf("Result: %d\n", z);
    return z;
}
