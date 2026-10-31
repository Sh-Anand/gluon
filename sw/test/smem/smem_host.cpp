#include <cstdio>
#include "rad.h"

int main() {
    const unsigned num_blocks = 8;
    const int a = 10, b = 20, c = 30;

    void *out;
    radMalloc(&out, num_blocks * 5 * sizeof(int));

    radParamBuf params;
    params.push(a);
    params.push(b);
    params.push(c);
    params.push(out);
    radKernelLaunch("smem_kernel", {num_blocks, 1, 1}, {1, 1, 1}, 4 * sizeof(int), &params);
    radStreamSynchronize();

    int res[num_blocks * 5];
    radMemcpyAsync(res, out, sizeof(res), radMemcpyDir_D2H);
    radStreamSynchronize();

    bool ok = true;
    for (unsigned i = 0; i < num_blocks; i++) {
        int *r = res + i * 5;
        printf("block %u smem@%d: %d %d %d tag %d\n", i, r[4], r[0], r[1], r[2], r[3]);
        ok &= r[0] == a + 1 && r[1] == b + 1 && r[2] == c + 1 && r[3] == (int)i;
    }
    printf("%s\n", ok ? "PASS" : "FAIL");
    return 0;
}
