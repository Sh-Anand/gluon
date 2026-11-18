#include <rad_defs.h>

#define DEPTH 6

// each thread of 4 blocks of 8 writes its index, offset by `base`
extern "C" __kernel__ void child(int *out, int base) {
    unsigned i = rad_block_idx_x() * 8 + rad_thread_idx_x();
    out[i] = base + i;
}

// one thread launches `child` over `out`
extern "C" __kernel__ void parent(int *out, int base) {
    if (rad_thread_idx_x() == 0)
        rad_launch(child, 4, 1, 1, 8, 1, 1, 0, out, base);
}

// launches itself until `depth` reaches DEPTH
extern "C" __kernel__ void recurse(int *out, int depth) {
    out[depth] = 100 + depth;
    if (depth < DEPTH)
        rad_launch(recurse, 1, 1, 1, 1, 1, 1, 0, out, depth + 1);
}

extern "C" __kernel__ void leaf(int *slot, int value) {
    *slot = value;
}

// every thread launches a `leaf` of its own: more launches at once than the command processor
// has slots, so most go through the launch queue, and 32 threads take their params blocks at once
extern "C" __kernel__ void fanout(int *out, int base) {
    unsigned t = rad_thread_idx_x();
    rad_launch(leaf, 1, 1, 1, 1, 1, 1, 0, out + t, base + t);
}
