#include <rad_defs.h>

// C = A * B with vx_mma: one warp for each 16x8 tile of C.  A is MxK in bf16, by rows; Bt is B, KxN in bf16,
// by columns; C is MxN in fp32, by rows.  A word of A or Bt holds two elements, so K / 2 words make a row
// of A or a column of B.  K is a multiple of 16
extern "C" __kernel__ void mma_kernel(const unsigned *A, const unsigned *Bt, float *C, int N, int K) {
    unsigned lane = rad_thread_idx_x(), g = lane / 4, t = lane % 4;
    unsigned row = rad_block_idx_y() * 16 + g, col = rad_block_idx_x() * 8;
    float c[4] = {0, 0, 0, 0};
    for (int k = 0; k < K / 2; k += 8) {
        unsigned a[4], b[2];
        for (int r = 0; r < 4; r++)
            a[r] = A[(row + 8 * (r % 2)) * (K / 2) + k + t + 4 * (r / 2)];
        for (int r = 0; r < 2; r++)
            b[r] = Bt[(col + g) * (K / 2) + k + t + 4 * r];
        rad_mma_bf16(c, a, b);
    }
    for (int r = 0; r < 4; r++)
        C[(row + 8 * (r / 2)) * N + col + 2 * t + r % 2] = c[r];
}
