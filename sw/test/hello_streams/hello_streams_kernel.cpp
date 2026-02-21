#include <rad_print.h>
#include <rad_defs.h>

extern "C" void times_2(int *x) {
    rad_printf("Hello times 2\n");
    rad_printf("Inputs: %d\n", *x);
    *x = *x * 2;
    rad_printf("Results times 2: %d\n", *x);
}

extern "C" void plus_3(int *x) {
    rad_printf("Hello plus 3\n");
    rad_printf("Inputs: %d\n", *x);
    *x = *x + 3;
    rad_printf("Results plus 3: %d\n", *x);
}