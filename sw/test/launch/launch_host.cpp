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

// launch `kernel` as one block of `threads`; every kernel here takes an output buffer and an int
static void launch(const char *kernel, unsigned threads, void *out, int arg, radStream_t stream = 0) {
    radParamBuf params;
    params.push(out);
    params.push(arg);
    radKernelLaunch(kernel, {1, 1, 1}, {threads, 1, 1}, 0, &params, stream);
}

int main() {
    // every sync below must return only once the launched kernels have run too
    void *out, *out1, *out2;
    radMalloc(&out, 32 * sizeof(int));
    radMalloc(&out1, 32 * sizeof(int));
    radMalloc(&out2, 32 * sizeof(int));

    // a kernel launches another: `parent` launches a child grid that fills `out` with base + index
    launch("parent", 32, out, 1000);
    radStreamSynchronize();
    unsigned w = wrong(out, 32, 1000);

    // a kernel launches itself
    launch("recurse", 1, out, 0);
    radStreamSynchronize();
    w += wrong(out, DEPTH + 1, 100);

    // kernels on two streams launch at once
    radStream_t s1, s2;
    radCreateStream(&s1);
    radCreateStream(&s2);
    launch("parent", 32, out1, 2000, s1);
    launch("parent", 32, out2, 3000, s2);
    radStreamSynchronize(s1);
    radStreamSynchronize(s2);
    w += wrong(out1, 32, 2000) + wrong(out2, 32, 3000);

    // 32 threads launch at once, 48 times over.  The command processor has slots for 8 at a time, so 24
    // of each 32 go through the launch queue: 1,152 in all, past its 1,024 slots
    for (int i = 0; i < 48; i++) {
        launch("fanout", 32, out, 4000 + 100 * i);
        radStreamSynchronize();
        w += wrong(out, 32, 4000 + 100 * i);
    }

    printf("%s (%u wrong)\n", w ? "FAIL" : "PASS", w);
    return 0;
}
