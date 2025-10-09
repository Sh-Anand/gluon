#include <rad_print.h>

extern "C" int hello_args_kernel(int x, int y, int z, int w, int v, int u, int t, int s, int r, int q, int p, int o) {
    rad_printf("Args: %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d\n", x, y, z, w, v, u, t, s, r, q, p, o);
    int res = x + y + z + w + v + u + t + s + r + q + p + o;
    rad_printf("Result: %d\n", res);
    return res;
}