#include <vx_print.h>
extern "C" int hello_kernel(int x, int y) {
    vx_printf("Hello, World!\n");
    return x + y;
}