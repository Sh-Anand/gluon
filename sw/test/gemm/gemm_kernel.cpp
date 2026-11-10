#include <rad_defs.h>

#define TILE 16

// one thread per element of C = A * B; A is MxK, B is KxN, all row-major, K a multiple of TILE
extern "C" void gemm_kernel(const int *A, const int *B, int *C, int N, int K) {
    unsigned tx, ty, bx, by;
    asm volatile("csrr %0, %1" : "=r"(tx) : "i"(CSR_THREAD_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(ty) : "i"(CSR_THREAD_IDX_Y));
    asm volatile("csrr %0, %1" : "=r"(bx) : "i"(CSR_BLOCK_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(by) : "i"(CSR_BLOCK_IDX_Y));

    unsigned row = by * TILE + ty, col = bx * TILE + tx;
    int acc = 0;
    for (int k0 = 0; k0 < K; k0 += TILE) // inner loop unrolled TILE times
        for (int k = 0; k < TILE; k++)
            acc += A[row * K + k0 + k] * B[(k0 + k) * N + col];
    C[row * N + col] = acc;
}
