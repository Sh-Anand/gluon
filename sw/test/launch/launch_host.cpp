#include <cstdio>
#include "rad.h"

#define DEPTH 6

// how many of `n` ints at `dev` differ from base + index
static unsigned wrong(void *dev, unsigned n, int base) {
    int res[64];
    radMemcpyAsync(res, dev, n * sizeof(int), radMemcpyDir_D2H);
    radStreamSynchronize();
    unsigned w = 0;
    for (unsigned i = 0; i < n; i++)
        w += res[i] != base + (int)i;
    return w;
}

// launch `parent` on `stream`: it launches a child grid that fills `*out` with base + index
static void launch_parent(void **out, int base, radStream_t stream) {
    void *blk;
    radMalloc(out, 32 * sizeof(int));
    radMalloc(&blk, 16);
    radParamBuf params;
    params.push(*out);
    params.push(blk);
    params.push(base);
    radKernelLaunch("parent", {1, 1, 1}, {32, 1, 1}, 0, &params, stream);
}

int main() {
    // every sync below must return only once the launched kernels have run too

    // a kernel launches another
    void *out;
    launch_parent(&out, 1000, 0);
    radStreamSynchronize();
    unsigned w = wrong(out, 32, 1000);

    // a kernel launches itself
    void *rout, *rblk;
    radMalloc(&rout, (DEPTH + 1) * sizeof(int));
    radMalloc(&rblk, (DEPTH + 2) * 16);
    radParamBuf params;
    params.push(rout);
    params.push(rblk);
    params.push(0);
    radKernelLaunch("recurse", {1, 1, 1}, {1, 1, 1}, 0, &params);
    radStreamSynchronize();
    w += wrong(rout, DEPTH + 1, 100);

    // kernels on two streams launch at once
    radStream_t s1, s2;
    radCreateStream(&s1);
    radCreateStream(&s2);
    void *out1, *out2;
    launch_parent(&out1, 2000, s1);
    launch_parent(&out2, 3000, s2);
    radStreamSynchronize(s1);
    radStreamSynchronize(s2);
    w += wrong(out1, 32, 2000) + wrong(out2, 32, 3000);

    printf("%s (%u wrong)\n", w ? "FAIL" : "PASS", w);
    return 0;
}
