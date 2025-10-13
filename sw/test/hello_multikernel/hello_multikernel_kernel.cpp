#include <rad_print.h>
#include <rad_defs.h>

extern "C" void hello_first_kernel(int *x, int *y, int *z, int *w) {
    rad_printf("Hello First World\n");
    rad_printf("Pointers x: %p, y: %p\n", x, y);
    rad_printf("Inputs: %d, %d\n", *x, *y);
    *z = *x + *y;
    *w = *z + 1;
    rad_printf("Results first kernel: %d, %d\n", *z, *w);
}
extern "C" void hello_second_kernel(int *z, int *w, int *u) {
    rad_printf("Hello Second World\n");
    rad_printf("Pointers z: %p, w: %p\n", z, w);
    rad_printf("Inputs: %d, %d\n", *z, *w);
    *u = *z * *w;
    rad_printf("Results second kernel: %d\n", *u);
}
