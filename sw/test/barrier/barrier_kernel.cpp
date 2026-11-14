#include <rad_defs.h>

#define THREADS 512

extern __shared int smem[];

// each thread writes its slot, later warps later; after the barrier it reads the mirrored slot
extern "C" void barrier_kernel(int *out) {
    unsigned tid = rad_thread_idx_x(), bx = rad_block_idx_x();

    volatile __shared int *s = smem;
    for (volatile unsigned i = 0; i < (tid >> 5) * 20; i++);
    s[tid] = bx * THREADS + tid;
    __syncthreads();
    out[bx * THREADS + tid] = s[THREADS - 1 - tid];
}
