#include <rad_defs.h>

#define TILE 16

// one thread per element of C = A * B; A is MxK, B is KxN, all row-major, K a multiple of TILE
extern "C" __kernel__ void gemm_kernel(const int *A, const int *B, int *C, int N, int K) {
    unsigned tx = rad_thread_idx_x(), ty = rad_thread_idx_y(), bx = rad_block_idx_x(), by = rad_block_idx_y();

    unsigned row = by * TILE + ty, col = bx * TILE + tx;
    int acc = 0;
    for (int k0 = 0; k0 < K; k0 += TILE) // inner loop unrolled TILE times
        for (int k = 0; k < TILE; k++)
            acc += A[row * K + k0 + k] * B[(k0 + k) * N + col];
    C[row * N + col] = acc;
}
