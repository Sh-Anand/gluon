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
    const unsigned M = 640, N = 640, K = 640; // M a multiple of 320, N of 128, K of 64
    const unsigned LINE = 128;
    // a row is LD elements from the next, 16 more than K: a row then starts a quarter of a line further into
    // its line than the row before
    const unsigned LD = K + 16;

    // the same matrices as gemm_mma: random signs and mantissas, magnitudes from 2^-7 to 2^8
    static unsigned short A[M * LD], Bt[N * LD]; // A[m * LD + k]; Bt[n * LD + k] is B[k][n]
    static float C[M * N];
    unsigned seed = 1;
    auto element = [&] {
        seed = seed * 1664525 + 1013904223;
        return (unsigned short)((seed >> 31) << 15 | (120 + (seed >> 8) % 16) << 7 | (seed >> 16 & 127));
    };
    for (unsigned m = 0; m < M; m++)
        for (unsigned k = 0; k < K; k++) A[m * LD + k] = element();
    for (unsigned n = 0; n < N; n++)
        for (unsigned k = 0; k < K; k++) Bt[n * LD + k] = element();

    void *mem;
    radMalloc(&mem, sizeof(A) + sizeof(Bt) + sizeof(C) + 4 * LINE);
    uintptr_t a = ((uintptr_t)mem + LINE - 1) / LINE * LINE;
    uintptr_t bt = a + (sizeof(A) + LINE - 1) / LINE * LINE;
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
    params.push((int)LD);
    // a block: 16 warps, and two panels of 16 steps by 454 words of shared memory
    radKernelLaunch("gemm_tiled_kernel", {N / 128, M / 320, 1}, {32, 16, 1}, 2 * 16 * 454 * 4, &params);
    radMemcpyAsync(C, dC, sizeof(C), radMemcpyDir_D2H);
    radStreamSynchronize();

    // each product, exact in fp32, is added to the sum in order of k
    unsigned bad = 0;
    for (unsigned i = 0; i < M; i++)
        for (unsigned j = 0; j < N; j++) {
            float ref = 0;
            for (unsigned k = 0; k < K; k++) {
                float product = bf16(A[i * LD + k]) * bf16(Bt[j * LD + k]);
                ref = ref + product;
            }
            bad += memcmp(&C[i * N + j], &ref, 4) != 0;
        }
    printf("%ux%ux%u gemm_tiled: %u mismatches\n%s\n", M, N, K, bad, bad ? "FAIL" : "PASS");
    return 0;
}
