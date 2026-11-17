#include <rad_defs.h>

#define DEPTH 6

// each thread of 4 blocks of 8 writes its index, offset by `base`
extern "C" __kernel__ void child(int *out, int base) {
    unsigned i = rad_block_idx_x() * 8 + rad_thread_idx_x();
    out[i] = base + i;
}

// one thread launches `child` over `out`, with the child's params in `blk`
extern "C" __kernel__ void parent(int *out, unsigned *blk, int base) {
    if (rad_thread_idx_x() == 0) {
        blk[0] = 8;
        blk[1] = (unsigned)out;
        blk[2] = base;
        rad_launch(child, 4, 1, 1, 8, 1, 1, 0, blk);
    }
}

// launches itself until `depth` reaches DEPTH, each level's params in the block after its own
extern "C" __kernel__ void recurse(int *out, unsigned *blk, int depth) {
    out[depth] = 100 + depth;
    if (depth < DEPTH) {
        unsigned *next = blk + 4;
        next[0] = 12;
        next[1] = (unsigned)out;
        next[2] = (unsigned)next;
        next[3] = depth + 1;
        rad_launch(recurse, 1, 1, 1, 1, 1, 1, 0, next);
    }
}
