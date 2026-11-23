#include <rad_defs.h>

#define BM 160  // rows of C a block computes
#define BN 160  // columns of C a block computes
#define BK 64   // columns of A (rows of B) staged at a time: one cache line of each row
#define LDS 36  // words from one staged row to the next: 32, and 4 of padding against bank conflicts

extern __shared unsigned smem[];

// C = A * B with vx_mma, written plainly in C.  A is MxK in bf16, by rows; Bt is B, KxN in bf16, by columns;
// C is MxN in fp32, by rows.  A word of A or Bt holds two elements.  M and N are multiples of 160, K of 64.
//
// A block computes 160x160 of C with 20 warps.  Warp m owns 2x5 tiles of 16x8, from row 32 (m / 4) and
// column 40 (m % 4) of the block.  For each 64 columns of K, every warp stages 8 rows of A and 8 of Bt into
// shared memory, then every warp multiplies: no warp does anything the others do not.
extern "C" __kernel__ void gemm_mma_c_kernel(const unsigned *A, const unsigned *Bt, float *C, int N, int K) {
    unsigned lane = rad_thread_idx_x(), m = rad_thread_idx_y(), g = lane / 4, t = lane % 4;
    unsigned row0 = rad_block_idx_y() * BM, col0 = rad_block_idx_x() * BN;
    unsigned kw = K / 2; // words in a row of A or Bt
    unsigned wr = 32 * (m / 4), wc = 40 * (m % 4);
    __shared unsigned *As = smem, *Bs = smem + BM * LDS;

    float c[2][5][4] = {};
    for (unsigned k0 = 0; k0 < kw; k0 += BK / 2) {
        for (int r = 0; r < 8; r++) {
            As[(8 * m + r) * LDS + lane] = A[(row0 + 8 * m + r) * kw + k0 + lane];
            Bs[(8 * m + r) * LDS + lane] = Bt[(col0 + 8 * m + r) * kw + k0 + lane];
        }
        __syncthreads();
        for (int kq = 0; kq < BK / 16; kq++) {
            unsigned a[2][4], b[5][2];
            for (int i = 0; i < 2; i++)
                for (int r = 0; r < 4; r++)
                    a[i][r] = As[(wr + 16 * i + g + 8 * (r % 2)) * LDS + 8 * kq + t + 4 * (r / 2)];
            for (int j = 0; j < 5; j++)
                for (int r = 0; r < 2; r++)
                    b[j][r] = Bs[(wc + 8 * j + g) * LDS + 8 * kq + t + 4 * r];
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 5; j++)
                    rad_mma_bf16(c[i][j], a[i], b[j]);
        }
        __syncthreads();
    }
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 5; j++)
            for (int r = 0; r < 4; r++)
                C[(row0 + wr + 16 * i + g + 8 * (r / 2)) * N + col0 + wc + 8 * j + 2 * t + r % 2] = c[i][j][r];
}
