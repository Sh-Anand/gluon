#include <rad_print.h>
#include <rad_defs.h>

extern __shared int smem[];

extern "C" void smem_kernel(int a, int b, int c, int *out) {
    unsigned bx;
    asm volatile("csrr %0, %1" : "=r"(bx) : "i"(CSR_BLOCK_IDX_X));

    // volatile so the reads below actually come from smem
    volatile __shared int *s = smem;
    s[0] = a + 1;
    s[1] = b + 1;
    s[2] = c + 1;
    s[3] = bx;

    int ra = s[0], rb = s[1], rc = s[2], tag = s[3];
    unsigned base = (unsigned)smem;
    rad_printf("block %d smem@%d: %d %d %d tag %d\n", bx, base, ra, rb, rc, tag);

    int *o = out + bx * 5;
    o[0] = ra;
    o[1] = rb;
    o[2] = rc;
    o[3] = tag;
    o[4] = base;
}
