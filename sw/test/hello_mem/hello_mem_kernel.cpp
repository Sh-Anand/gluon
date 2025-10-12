#include <rad_print.h>
#include <rad_defs.h>

extern "C" void hello_mem_kernel(int *x, int *y, int *z) {
    rad_printf("Hello, World\n");
    rad_printf("Pointers x: %p, y: %p\n", x, y);
    rad_printf("Inputs: %d, %d\n", *x, *y);
    *z = *x + *y;
    rad_printf("Result: %d\n", *z);
}
