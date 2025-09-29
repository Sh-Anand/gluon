#ifndef RADIANCE_DRIVER_H
#define RADIANCE_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int x;
    unsigned int y;
    unsigned int z;
} radDim3;

void radKernelLaunch(const char *kernel_name, radDim3 grid_dim, radDim3 block_dim);

#ifdef __cplusplus
}
#endif

#endif  // RADIANCE_DRIVER_H
