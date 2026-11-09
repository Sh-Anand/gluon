#include <rad_defs.h>

#define TILE 8

extern __shared int smem[];

// one thread per element of C = A * B, staging TILE x TILE tiles of A and B in smem;
// A is MxK, B is KxN, all row-major, K a multiple of TILE
extern "C" void gemm_tiled_kernel(const int *A, const int *B, int *C, int N, int K) {
    unsigned tx, ty, bx, by;
    asm volatile("csrr %0, %1" : "=r"(tx) : "i"(CSR_THREAD_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(ty) : "i"(CSR_THREAD_IDX_Y));
    asm volatile("csrr %0, %1" : "=r"(bx) : "i"(CSR_BLOCK_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(by) : "i"(CSR_BLOCK_IDX_Y));

    __shared int *As = smem, *Bs = smem + TILE * TILE;
    unsigned row = by * TILE + ty, col = bx * TILE + tx;
    int acc = 0;
    for (int k0 = 0; k0 < K; k0 += TILE) {
        As[ty * TILE + tx] = A[row * K + k0 + tx];
        Bs[ty * TILE + tx] = B[(k0 + ty) * N + col];
        __syncthreads();
        for (int k = 0; k < TILE; k++)
            acc += As[ty * TILE + k] * Bs[k * TILE + tx];
        __syncthreads();
    }
    C[row * N + col] = acc;
}
