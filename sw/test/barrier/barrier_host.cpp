#include <cstdio>
#include "rad.h"

int main() {
    const unsigned num_blocks = 4, threads = 512;

    void *out;
    radMalloc(&out, num_blocks * threads * sizeof(int));

    radParamBuf params;
    params.push(out);
    radKernelLaunch("barrier_kernel", {num_blocks, 1, 1}, {threads, 1, 1}, threads * sizeof(int), &params);
    radStreamSynchronize();

    static int res[num_blocks * threads];
    radMemcpyAsync(res, out, sizeof(res), radMemcpyDir_D2H);
    radStreamSynchronize();

    unsigned wrong = 0;
    for (unsigned b = 0; b < num_blocks; b++)
        for (unsigned t = 0; t < threads; t++)
            wrong += res[b * threads + t] != (int)(b * threads + threads - 1 - t);
    printf("%s (%u wrong)\n", wrong ? "FAIL" : "PASS", wrong);
    return 0;
}
