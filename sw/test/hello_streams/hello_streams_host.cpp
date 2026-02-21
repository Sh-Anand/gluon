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
    radMemCpy(x_ptr, &x, sizeof(x), radMemCpyDir_H2D);
    radMemCpy(y_ptr, &x, sizeof(x), radMemCpyDir_H2D);
    radError err;
    radGetError(&err);
    radGetError(&err);

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

    radWaitEvent(&e1, s2);
    radWaitEvent(&e2, s1);

    radKernelLaunch("plus_3", grid, block, &params_one, s1);
    radKernelLaunch("times_2", grid, block, &params_two, s2);
    int a, b;
    radMemCpy(&a, x_ptr, sizeof(a), radMemCpyDir_D2H, s1);
    radMemCpy(&b, y_ptr, sizeof(b), radMemCpyDir_D2H, s2);

    // hack because error reporting is currently broken
    radGetError(&err, s1);
    radGetError(&err, s2);
    radGetError(&err, s1);
    radGetError(&err, s2);
    radGetError(&err, s1);
    radGetError(&err, s2);

    printf("Received host final results: %d, %d\n", a, b);    
}