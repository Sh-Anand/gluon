#ifndef RADIANCE_DRIVER_H
#define RADIANCE_DRIVER_H

#include <stddef.h>

#define RAD_GPU_DRAM_SIZE (512u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int x;
    unsigned int y;
    unsigned int z;
} radDim3;

void radKernelLaunch(const char *kernel_name, radDim3 grid_dim, radDim3 block_dim);

void radMalloc(void **ptr, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif  // RADIANCE_DRIVER_H
