#include <rad_defs.h>

#define TILE 8

// one thread per element of C = A * B; A is MxK, B is KxN, all row-major
extern "C" void gemm_kernel(const int *A, const int *B, int *C, int N, int K) {
    unsigned tx, ty, bx, by;
    asm volatile("csrr %0, %1" : "=r"(tx) : "i"(CSR_THREAD_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(ty) : "i"(CSR_THREAD_IDX_Y));
    asm volatile("csrr %0, %1" : "=r"(bx) : "i"(CSR_BLOCK_IDX_X));
    asm volatile("csrr %0, %1" : "=r"(by) : "i"(CSR_BLOCK_IDX_Y));

    unsigned row = by * TILE + ty, col = bx * TILE + tx;
    int acc = 0;
    for (int k = 0; k < K; k++)
        acc += A[row * K + k] * B[k * N + col];
    C[row * N + col] = acc;
}
