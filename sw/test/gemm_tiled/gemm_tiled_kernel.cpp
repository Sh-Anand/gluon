#include <rad_defs.h>

#define TILE 16

extern __shared int smem[];

// one thread per element of C = A * B, staging TILE x TILE tiles of A and B in smem;
// A is MxK, B is KxN, all row-major, K a multiple of TILE
extern "C" __kernel__ void gemm_tiled_kernel(const int *A, const int *B, int *C, int N, int K) {
    unsigned tx = rad_thread_idx_x(), ty = rad_thread_idx_y(), bx = rad_block_idx_x(), by = rad_block_idx_y();

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
