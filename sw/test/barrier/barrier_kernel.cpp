#include <rad_defs.h>

#define THREADS 512

extern __shared int smem[];

// each thread writes its slot, later warps later; after the barrier it reads the mirrored slot
extern "C" void barrier_kernel(int *out) {
    unsigned tid, bx;
    asm volatile("csrr %0, %1" : "=r"(tid) : "i"(CSR_THREAD_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(bx) : "i"(CSR_BLOCK_IDX_X));

    volatile __shared int *s = smem;
    for (volatile unsigned i = 0; i < (tid >> 5) * 20; i++);
    s[tid] = bx * THREADS + tid;
    __syncthreads();
    out[bx * THREADS + tid] = s[THREADS - 1 - tid];
}
