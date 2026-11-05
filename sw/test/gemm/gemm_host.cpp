#include <cstdio>
#include "rad.h"

int main() {
    const unsigned M = 32, N = 32, K = 32, TILE = 8;

    static int A[M * K], B[K * N], C[M * N];
    for (unsigned i = 0; i < M * K; i++) A[i] = i % 7 - 3;
    for (unsigned i = 0; i < K * N; i++) B[i] = i % 5 - 2;

    void *dA, *dB, *dC;
    radMalloc(&dA, sizeof(A));
    radMalloc(&dB, sizeof(B));
    radMalloc(&dC, sizeof(C));
    radMemcpyAsync(dA, A, sizeof(A), radMemcpyDir_H2D);
    radMemcpyAsync(dB, B, sizeof(B), radMemcpyDir_H2D);

    radParamBuf params;
    params.push(dA);
    params.push(dB);
    params.push(dC);
    params.push((int)N);
    params.push((int)K);
    radKernelLaunch("gemm_kernel", {N / TILE, M / TILE, 1}, {TILE, TILE, 1}, 0, &params);
    radMemcpyAsync(C, dC, sizeof(C), radMemcpyDir_D2H);
    radStreamSynchronize();

    unsigned bad = 0;
    for (unsigned i = 0; i < M; i++)
        for (unsigned j = 0; j < N; j++) {
            int ref = 0;
            for (unsigned k = 0; k < K; k++) ref += A[i * K + k] * B[k * N + j];
            bad += C[i * N + j] != ref;
        }
    printf("%ux%ux%u gemm: %u mismatches\n%s\n", M, N, K, bad, bad ? "FAIL" : "PASS");
    return 0;
}
