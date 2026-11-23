#include <cstdint>
#include <cstdio>
#include <cstring>
#include "rad.h"

static float bf16(unsigned short h) {
    unsigned w = (unsigned)h << 16;
    float f;
    memcpy(&f, &w, 4);
    return f;
}

int main() {
    const unsigned M = 640, N = 640, K = 640; // M and N multiples of 160, K of 64
    const unsigned LINE = 128;

    // random signs and mantissas, magnitudes from 2^-7 to 2^8: the sums round, so their order shows
    static unsigned short A[M * K], Bt[N * K]; // Bt[n * K + k] is B[k][n]
    static float C[M * N];
    unsigned seed = 1;
    auto element = [&] {
        seed = seed * 1664525 + 1013904223;
        return (unsigned short)((seed >> 31) << 15 | (120 + (seed >> 8) % 16) << 7 | (seed >> 16 & 127));
    };
    for (auto &a : A) a = element();
    for (auto &b : Bt) b = element();

    // each array on a cache line, and Bt an odd number of lines from A: a chunk's lines of A and of Bt, a
    // row every K / 64 lines, then fall in different sets of the L1 and none pushes another out
    void *mem;
    radMalloc(&mem, sizeof(A) + sizeof(Bt) + sizeof(C) + 4 * LINE);
    uintptr_t a = ((uintptr_t)mem + LINE - 1) / LINE * LINE;
    uintptr_t bt = a + (sizeof(A) + LINE - 1) / LINE * LINE;
    bt += ((bt - a) / LINE % 2 == 0) * LINE;
    uintptr_t c = bt + (sizeof(Bt) + LINE - 1) / LINE * LINE;
    void *dA = (void *)a, *dBt = (void *)bt, *dC = (void *)c;
    radMemcpyAsync(dA, A, sizeof(A), radMemcpyDir_H2D);
    radMemcpyAsync(dBt, Bt, sizeof(Bt), radMemcpyDir_H2D);

    radParamBuf params;
    params.push(dA);
    params.push(dBt);
    params.push(dC);
    params.push((int)N);
    params.push((int)K);
    // a block: 20 warps, and 320 rows of 36 words of shared memory
    radKernelLaunch("gemm_mma_c_kernel", {N / 160, M / 160, 1}, {32, 20, 1}, 320 * 144, &params);
    radMemcpyAsync(C, dC, sizeof(C), radMemcpyDir_D2H);
    radStreamSynchronize();

    // vx_mma adds each product, exact in fp32, to the sum in order of k
    unsigned bad = 0;
    for (unsigned i = 0; i < M; i++)
        for (unsigned j = 0; j < N; j++) {
            float ref = 0;
            for (unsigned k = 0; k < K; k++) {
                float product = bf16(A[i * K + k]) * bf16(Bt[j * K + k]);
                ref = ref + product;
            }
            bad += memcmp(&C[i * N + j], &ref, 4) != 0;
        }
    printf("%ux%ux%u gemm_mma_c: %u mismatches\n%s\n", M, N, K, bad, bad ? "FAIL" : "PASS");
    return 0;
}
