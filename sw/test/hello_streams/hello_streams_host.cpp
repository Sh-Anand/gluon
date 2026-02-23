#include <cstdio>
#include "rad.h"

int main() {

    radStream_t s1, s2;
    radCreateStream(&s1);
    radCreateStream(&s2);

    int x = 7;
    void* x_ptr;
    void* y_ptr;
    radMalloc(&x_ptr, sizeof(x));
    radMalloc(&y_ptr, sizeof(x));
    printf("Copying to GPU Memory\n");
    radMemcpyAsync(x_ptr, &x, sizeof(x), radMemcpyDir_H2D);
    radMemcpyAsync(y_ptr, &x, sizeof(x), radMemcpyDir_H2D);
    radStreamSynchronize();

    radDim3 grid = {1, 1, 1};
    radDim3 block = {1, 1, 1};
    radParamBuf params_one, params_two;
    params_one.push(x_ptr);
    params_two.push(y_ptr);

    radEvent_t e1, e2;
    radKernelLaunch("times_2", grid, block, &params_one, s1);
    radEventRecord(&e1, s1);
    radKernelLaunch("plus_3", grid, block, &params_two, s2);
    radEventRecord(&e2, s2);

    radStreamWaitEvent(&e1, s2);
    radStreamWaitEvent(&e2, s1);

    radKernelLaunch("plus_3", grid, block, &params_one, s1);
    radKernelLaunch("times_2", grid, block, &params_two, s2);
    int a, b;
    radMemcpyAsync(&a, x_ptr, sizeof(a), radMemcpyDir_D2H, s1);
    radMemcpyAsync(&b, y_ptr, sizeof(b), radMemcpyDir_D2H, s2);
    radStreamSynchronize(s1);
    radStreamSynchronize(s2);

    printf("Received host final results: %d, %d\n", a, b);    
}
