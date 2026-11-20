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
    const unsigned M = 128, N = 128, K = 128;

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

    void *dA, *dBt, *dC;
    radMalloc(&dA, sizeof(A));
    radMalloc(&dBt, sizeof(Bt));
    radMalloc(&dC, sizeof(C));
    radMemcpyAsync(dA, A, sizeof(A), radMemcpyDir_H2D);
    radMemcpyAsync(dBt, Bt, sizeof(Bt), radMemcpyDir_H2D);

    radParamBuf params;
    params.push(dA);
    params.push(dBt);
    params.push(dC);
    params.push((int)N);
    params.push((int)K);
    radKernelLaunch("mma_kernel", {N / 8, M / 16, 1}, {32, 1, 1}, 0, &params);
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
    printf("%ux%ux%u mma: %u mismatches\n%s\n", M, N, K, bad, bad ? "FAIL" : "PASS");
    return 0;
}
