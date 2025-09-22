#ifndef RADIANCE_DRIVER_H
#define RADIANCE_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

void radKernelLaunch(const char *kernel_name, ...);

#ifdef __cplusplus
}
#endif

#endif  // RADIANCE_DRIVER_H
